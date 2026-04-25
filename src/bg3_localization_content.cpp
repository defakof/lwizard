#include "bg3_localization_content.h"
#include "lwizard_divine.h"
#include "lwizard_log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>
#include <QXmlStreamReader>

#include <uibase/ifiletree.h>
#include <uibase/imodinterface.h>
#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

namespace {

constexpr int kCacheJsonVersion       = 2;
constexpr int kLegacyCacheJsonVersion = 1;

/** Minimum loca UUID count to consider translation-mod overlap (small mods may have only a handful). */
constexpr int kMinUuidsTranslation = 3;
/** Minimum matching UUIDs vs a reference mod (must still meet kOverlapRecall). */
constexpr int kMinOverlapAbs       = 2;
/** |intersection| / |candidate UUIDs| to classify as translation mod. */
constexpr double kOverlapRecall    = 0.40;

/**
 * When a mod has no .loca UUID overlap (e.g. XML-only localization), pair a translation
 * pack to its base mod by display name if the base has no strings for the scan language.
 */
static QString guessTranslationBaseModName(const QString& modName, const QStringList& allModNames)
{
  static const QStringList kSuffixes = {
      QStringLiteral(" - Russian Translation"),
      QStringLiteral(" Russian Translation"),
      QStringLiteral(" (Russian Translation)"),
      QStringLiteral(" — Russian Translation"),
      QStringLiteral(" - RUS"),
      QStringLiteral(" RUS"),
      QStringLiteral("_RUS"),
      QStringLiteral(" - RU"),
      QStringLiteral(" RU"),
      QStringLiteral("_RU"),
      QStringLiteral(" (Russian)"),
      QStringLiteral(" (RU)"),
  };

  for (const QString& suf : kSuffixes) {
    if (!modName.endsWith(suf, Qt::CaseInsensitive))
      continue;
    const QString stem = modName.left(modName.size() - suf.size()).trimmed();
    if (stem.isEmpty())
      continue;
    for (const QString& n : allModNames) {
      if (n.compare(stem, Qt::CaseInsensitive) == 0)
        return n;
    }
  }
  return {};
}

/** Parse BG3 localization XML (loca convert output or Mods/**\/Localization/*\/\/*.xml). */
static QJsonObject parseBg3LocalizationXmlContent(QIODevice* io)
{
  QXmlStreamReader xr(io);
  QJsonObject      out;

  auto pickKey = [](const QXmlStreamAttributes& a) -> QString {
    static const QStringList keys = {QStringLiteral("contentuid"), QStringLiteral("uuid"),
                                     QStringLiteral("key"), QStringLiteral("id")};
    for (const QString& k : keys) {
      for (const auto& attr : a) {
        if (attr.name().compare(k, Qt::CaseInsensitive) == 0)
          return attr.value().toString();
      }
    }
    return {};
  };

  while (!xr.atEnd()) {
    xr.readNext();
    if (!xr.isStartElement())
      continue;

    const QString name = xr.name().toString();
    if (name.compare(QStringLiteral("content"), Qt::CaseInsensitive) != 0 &&
        name.compare(QStringLiteral("entry"), Qt::CaseInsensitive) != 0 &&
        name.compare(QStringLiteral("string"), Qt::CaseInsensitive) != 0)
      continue;

    const QString k = pickKey(xr.attributes());
    if (k.isEmpty())
      continue;
    const QString v = xr.readElementText(QXmlStreamReader::IncludeChildElements);
    out[k]          = v;
  }

  if (xr.hasError())
    return {};
  return out;
}

static QByteArray localizationXmlFileToJsonMapCompressed(const QString& xmlAbsPath)
{
  QFile f(xmlAbsPath);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  const QJsonObject out = parseBg3LocalizationXmlContent(&f);
  if (out.isEmpty())
    return {};
  const QByteArray json = QJsonDocument(out).toJson(QJsonDocument::Compact);
  return qCompress(json, 9);
}

static const QString kPersistentKey()
{
  return QStringLiteral("localization_scan_cache");
}

static const QString kStringsPersistentKey()
{
  return QStringLiteral("localization_embedded_strings_cache");
}

QByteArray variantToJsonBytes(const QVariant& v)
{
  if (!v.isValid())
    return {};
  if (v.typeId() == QMetaType::QByteArray)
    return v.toByteArray();
  return v.toString().toUtf8();
}

QStringList jsonArrayToStringList(const QJsonValue& value)
{
  QStringList out;
  const QJsonArray arr = value.toArray();
  out.reserve(arr.size());
  for (const QJsonValue& item : arr) {
    const QString text = item.toString().trimmed();
    if (!text.isEmpty())
      out.append(text);
  }
  return out;
}

QStringList normalizedLinkedMods(QStringList mods)
{
  QSet<QString> seen;
  QStringList   out;
  for (QString& mod : mods) {
    mod = mod.trimmed();
    if (mod.isEmpty() || seen.contains(mod))
      continue;
    seen.insert(mod);
    out.append(mod);
  }
  out.sort(Qt::CaseInsensitive);
  return out;
}

QString contentIdLabel(int contentId)
{
  switch (contentId) {
    case BG3LocalizationContent::CONTENT_EMBEDDED:
      return QStringLiteral("embedded");
    case BG3LocalizationContent::CONTENT_INSTALLED:
      return QStringLiteral("installed/redundant");
    case BG3LocalizationContent::CONTENT_AVAILABLE:
      return QStringLiteral("available");
    case BG3LocalizationContent::CONTENT_OUTDATED:
      return QStringLiteral("outdated");
    case BG3LocalizationContent::CONTENT_UNAVAILABLE:
      return QStringLiteral("unavailable");
    case BG3LocalizationContent::CONTENT_TRANSLATION_MOD:
      return QStringLiteral("translation-mod");
    default:
      return QStringLiteral("unknown");
  }
}

class ScopedScanTimer
{
public:
  explicit ScopedScanTimer(qint64* bucket) : m_bucket(bucket) { m_timer.start(); }
  ~ScopedScanTimer()
  {
    if (m_bucket)
      *m_bucket += m_timer.elapsed();
  }

private:
  qint64*       m_bucket = nullptr;
  QElapsedTimer m_timer;
};

QString normalizedPakEntry(QString entry)
{
  const int tab = entry.indexOf(QChar('\t'));
  if (tab >= 0)
    entry = entry.left(tab);
  entry = entry.trimmed();
  entry.replace(QChar('\\'), QChar('/'));
  return entry;
}

}  // namespace

BG3LocalizationContent::BG3LocalizationContent(MOBase::IOrganizer* organizer)
    : QObject(nullptr), m_organizer(organizer)
{}

// ---------------------------------------------------------------------------
// ModDataContent interface
// ---------------------------------------------------------------------------

std::vector<MOBase::ModDataContent::Content>
BG3LocalizationContent::getAllContents() const
{
  return {
      Content(CONTENT_EMBEDDED,    "Translation: Embedded",            ":/lwizard/1_embedded.ico"),
      Content(CONTENT_INSTALLED,   "Translation: Installed / redundant (other lang or base mod)",
            ":/lwizard/2_installed.ico"),
      Content(CONTENT_AVAILABLE,   "Translation: Available on Nexus",   ":/lwizard/3_available.ico"),
      Content(CONTENT_OUTDATED,    "Translation: Installed, outdated",  ":/lwizard/4_outdated.ico"),
      Content(CONTENT_UNAVAILABLE, "Translation: Not available",        ":/lwizard/5_unavailable.ico"),
      Content(CONTENT_TRANSLATION_MOD, "Translation: Mod (UUID match)", ":/lwizard/2_installed.ico"),
  };
}

std::vector<int> BG3LocalizationContent::getContentsFor(
    std::shared_ptr<const MOBase::IFileTree> fileTree) const
{
  const QString language = currentLanguage();
  const QString modName  = fileTree->name();
  if (auto* mod = m_organizer->modList()->getMod(modName); mod && mod->isSeparator())
    return {};

  // Only use cache — icons appear after an explicit scanAll().
  auto lock = QMutexLocker(&m_cacheMutex);
  auto it   = m_cache.constFind(modName);
  if (it != m_cache.constEnd() && it->language == language && it->contentId != CONTENT_NONE)
    return {it->contentId};

  return {};
}

void BG3LocalizationContent::clearCache()
{
  auto lock = QMutexLocker(&m_cacheMutex);
  m_cache.clear();
}

bool BG3LocalizationContent::clearAllCaches()
{
  {
    auto lock = QMutexLocker(&m_autoScanMutex);
    if (m_scanning.load() || m_autoScanRunning)
      return false;
    m_autoScanQueue.clear();
    m_autoScanPending.clear();
  }

  {
    auto lock = QMutexLocker(&m_cacheMutex);
    m_cache.clear();
  }

  QJsonObject scanRoot;
  scanRoot[QStringLiteral("v")]    = kCacheJsonVersion;
  scanRoot[QStringLiteral("mods")] = QJsonObject{};
  m_organizer->setPersistent(QStringLiteral("lwizard"), kPersistentKey(),
                             QVariant(QJsonDocument(scanRoot).toJson(QJsonDocument::Compact)),
                             true);

  QJsonObject stringsRoot;
  stringsRoot[QStringLiteral("v")]    = 1;
  stringsRoot[QStringLiteral("mods")] = QJsonObject{};
  m_organizer->setPersistent(QStringLiteral("lwizard"), kStringsPersistentKey(),
                             QVariant(QJsonDocument(stringsRoot).toJson(QJsonDocument::Compact)),
                             true);

  LWizardLog::info(QStringLiteral("All LWizard localization scan caches cleared."));
  emit contentCacheUpdated();
  return true;
}

BG3LocalizationContent::CacheEntry
BG3LocalizationContent::entryForCurrentLanguage(const QString& modName) const
{
  const QString language = currentLanguage();
  auto          lock     = QMutexLocker(&m_cacheMutex);
  const auto    it       = m_cache.constFind(modName);
  if (it == m_cache.constEnd() || it->language != language)
    return {};
  return *it;
}

QString BG3LocalizationContent::translationTargetFor(const QString& modName) const
{
  const CacheEntry entry = entryForCurrentLanguage(modName);
  if (!entry.relationshipsKnown)
    return {};
  return entry.translationTarget;
}

QStringList BG3LocalizationContent::separateTranslationsFor(const QString& modName) const
{
  const CacheEntry entry = entryForCurrentLanguage(modName);
  if (!entry.relationshipsKnown)
    return {};
  return entry.separateTranslations;
}

QStringList BG3LocalizationContent::linkedModsFor(const QString& modName) const
{
  const CacheEntry entry = entryForCurrentLanguage(modName);
  if (!entry.relationshipsKnown)
    return {};

  QStringList linked;
  if (!entry.translationTarget.isEmpty())
    linked.append(entry.translationTarget);
  linked.append(entry.separateTranslations);
  return normalizedLinkedMods(linked);
}

QString BG3LocalizationContent::contentTooltipFor(const QString& modName) const
{
  const CacheEntry entry = entryForCurrentLanguage(modName);
  if (!entry.relationshipsKnown)
    return {};

  if (!entry.translationTarget.isEmpty()) {
    return QStringLiteral("Translation for: %1").arg(entry.translationTarget);
  }

  if (entry.separateTranslations.isEmpty())
    return {};

  if (entry.separateTranslations.size() == 1) {
    return QStringLiteral("Separate translation installed: %1")
        .arg(entry.separateTranslations.constFirst());
  }

  QString tooltip = QStringLiteral("Separate translations installed:");
  for (const QString& mod : entry.separateTranslations)
    tooltip += QStringLiteral("\n- %1").arg(mod);
  return tooltip;
}

bool BG3LocalizationContent::hasLinkedMods(const QString& modName) const
{
  return !linkedModsFor(modName).isEmpty();
}

void BG3LocalizationContent::markNexusAvailable(const QString& modName,
                                                 const QList<int>& nexusModIds)
{
  const QString lang = currentLanguage();
  bool changed = false;

  {
    QMutexLocker lk(&m_cacheMutex);
    auto it = m_cache.find(modName);
    if (it != m_cache.end() && it->language == lang) {
      if (it->contentId == CONTENT_UNAVAILABLE)
        it->contentId = CONTENT_AVAILABLE;
      it->nexusTranslationModIds = nexusModIds;
      changed = true;
    }
  }

  if (changed) {
    LWizardLog::info(
        QStringLiteral("Nexus: marked '%1' as CONTENT_AVAILABLE (%2 translation mod(s))")
            .arg(modName).arg(nexusModIds.size()));
    emit contentCacheUpdated();
  }
}

QList<int> BG3LocalizationContent::nexusTranslationModIds(const QString& modName) const
{
  QMutexLocker lk(&m_cacheMutex);
  auto it = m_cache.constFind(modName);
  if (it != m_cache.constEnd())
    return it->nexusTranslationModIds;
  return {};
}

QStringList BG3LocalizationContent::unavailableMods() const
{
  const QString lang = currentLanguage();
  QMutexLocker  lk(&m_cacheMutex);
  QStringList   out;
  for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) {
    if (it->language == lang && it->contentId == CONTENT_UNAVAILABLE)
      out.append(it.key());
  }
  return out;
}

QString BG3LocalizationContent::localizationFingerprint(const QString& modAbsPath) const
{
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QStringList        lines;

  auto appendFile = [&](const QString& relUtf8, const QFileInfo& fi) {
    if (!fi.isFile())
      return;
    lines.append(QStringLiteral("%1|%2|%3")
                     .arg(relUtf8)
                     .arg(fi.size())
                     .arg(fi.lastModified().toMSecsSinceEpoch()));
  };

  const QDir modRoot(modAbsPath);
  if (!modRoot.exists()) {
    hash.addData("!");
    return QString::fromLatin1(hash.result().toHex());
  }

  {
    QDir loc(modAbsPath + QStringLiteral("/Localization"));
    if (loc.exists()) {
      QDirIterator it(loc.path(), QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString     rel = loc.relativeFilePath(fi.absoluteFilePath());
        appendFile(QStringLiteral("Localization/") + rel, fi);
      }
    }
  }

  {
    const QFileInfoList rootPaks =
        modRoot.entryInfoList(QStringList{QStringLiteral("*.pak")}, QDir::Files);
    for (const QFileInfo& fi : rootPaks)
      appendFile(fi.fileName(), fi);
  }

  {
    QDir pakDir(modAbsPath + QStringLiteral("/PAK_FILES"));
    if (pakDir.exists()) {
      QDirIterator it(pakDir.path(), QStringList{QStringLiteral("*.pak")}, QDir::Files,
                      QDirIterator::Subdirectories);
      while (it.hasNext()) {
        it.next();
        const QFileInfo fi  = it.fileInfo();
        const QString rel   = pakDir.relativeFilePath(fi.absoluteFilePath());
        appendFile(QStringLiteral("PAK_FILES/") + rel, fi);
      }
    }
  }

  // Also include unpacked localization under PAK_FILES/**/Localization
  {
    QDir pakDir(modAbsPath + QStringLiteral("/PAK_FILES"));
    if (pakDir.exists()) {
      QDirIterator it(pakDir.path(), QStringList{QStringLiteral("Localization")},
                      QDir::Dirs, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        it.next();
        const QDir locRoot(it.filePath());
        QDirIterator files(locRoot.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
          files.next();
          const QFileInfo fi = files.fileInfo();
          const QString rel  = pakDir.relativeFilePath(fi.absoluteFilePath());
          appendFile(QStringLiteral("PAK_FILES/") + rel, fi);
        }
      }
    }
  }

  lines.sort(Qt::CaseInsensitive);
  for (const QString& ln : lines)
    hash.addData(ln.toUtf8());
  return QString::fromLatin1(hash.result().toHex());
}

void BG3LocalizationContent::hydrateMemoryFromPersistent()
{
  const QString lang = currentLanguage();
  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"), kPersistentKey(),
                                                 QVariant()));
  if (raw.isEmpty())
    return;

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return;
  const QJsonObject root = doc.object();
  const int version = root[QStringLiteral("v")].toInt(0);
  if (version != kLegacyCacheJsonVersion && version != kCacheJsonVersion)
    return;

  const QJsonObject mods = root[QStringLiteral("mods")].toObject();
  QHash<QString, CacheEntry> loaded;

  for (auto it = mods.constBegin(); it != mods.constEnd(); ++it) {
    const QString modName = it.key();
    const QJsonObject modObj = it->toObject();
    const QString     storedPath = modObj[QStringLiteral("path")].toString();
    const QJsonObject langs      = modObj[QStringLiteral("langs")].toObject();
    const QJsonObject langEntry  = langs[lang].toObject();
    if (langEntry.isEmpty())
      continue;

    MOBase::IModInterface* mod = m_organizer->modList()->getMod(modName);
    if (!mod)
      continue;
    if (mod->isSeparator())
      continue;
    if (mod->absolutePath().compare(storedPath, Qt::CaseInsensitive) != 0)
      continue;

    const int     id = langEntry[QStringLiteral("id")].toInt(CONTENT_NONE);
    const QString fp = langEntry[QStringLiteral("fp")].toString();
    if (id == CONTENT_NONE || fp.isEmpty())
      continue;
    if (id == CONTENT_UNAVAILABLE &&
        discoverLanguagesInMod(mod->absolutePath()).isEmpty())
      continue;

    const QString liveFp = localizationFingerprint(mod->absolutePath());
    if (liveFp != fp)
      continue;

    CacheEntry entry;
    entry.language    = lang;
    entry.contentId   = id;
    entry.fingerprint = fp;

    if (version >= kCacheJsonVersion) {
      entry.translationTarget =
          langEntry[QStringLiteral("target")].toString().trimmed();
      entry.separateTranslations =
          jsonArrayToStringList(langEntry[QStringLiteral("translations")]);
      entry.relationshipsKnown = langEntry[QStringLiteral("linksKnown")].toBool(false);
    }

    loaded.insert(modName, entry);
  }

  if (version >= kCacheJsonVersion) {
    for (auto it = loaded.begin(); it != loaded.end(); ++it) {
      if (!it->relationshipsKnown) {
        it->translationTarget.clear();
        it->separateTranslations.clear();
        continue;
      }

      if (it->translationTarget == it.key() ||
          !loaded.contains(it->translationTarget)) {
        it->translationTarget.clear();
      }

      QStringList translations;
      for (const QString& translation : it->separateTranslations) {
        if (translation == it.key() || !loaded.contains(translation))
          continue;
        translations.append(translation);
      }
      it->separateTranslations = normalizedLinkedMods(translations);
    }
  }

  auto lock = QMutexLocker(&m_cacheMutex);
  for (auto it = m_cache.begin(); it != m_cache.end();) {
    if (it->language == lang)
      it = m_cache.erase(it);
    else
      ++it;
  }
  for (auto it = loaded.constBegin(); it != loaded.constEnd(); ++it)
    m_cache[it.key()] = it.value();
}

void BG3LocalizationContent::pruneMissingModsFromCache()
{
  const QStringList allMods = m_organizer->modList()->allMods();
  QSet<QString>     valid;
  for (const QString& n : allMods)
    valid.insert(n);

  bool memChanged      = false;
  bool linksTrimmed    = false;
  {
    auto lock = QMutexLocker(&m_cacheMutex);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
      if (!valid.contains(it.key())) {
        it = m_cache.erase(it);
        memChanged = true;
      } else {
        if (!it->translationTarget.isEmpty() &&
            !valid.contains(it->translationTarget)) {
          it->translationTarget.clear();
          linksTrimmed = true;
        }

        QStringList translations;
        for (const QString& translation : it->separateTranslations) {
          if (valid.contains(translation))
            translations.append(translation);
        }
        const QStringList normalized = normalizedLinkedMods(translations);
        if (normalized != it->separateTranslations) {
          it->separateTranslations = normalized;
          linksTrimmed             = true;
        }
        ++it;
      }
    }
  }

  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"), kPersistentKey(),
                                                 QVariant()));
  if (raw.isEmpty())
    return;

  QJsonParseError err;
  QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return;

  QJsonObject root = doc.object();
  QJsonObject mods = root[QStringLiteral("mods")].toObject();
  bool        jsonChanged = false;
  for (auto it = mods.begin(); it != mods.end();) {
    if (!valid.contains(it.key())) {
      it = mods.erase(it);
      jsonChanged = true;
    } else
      ++it;
  }

  for (auto it = mods.begin(); it != mods.end(); ++it) {
    QJsonObject modObj = it->toObject();
    QJsonObject langs  = modObj[QStringLiteral("langs")].toObject();
    bool modChanged    = false;
    for (auto langIt = langs.begin(); langIt != langs.end(); ++langIt) {
      QJsonObject langEntry = langIt->toObject();
      const QString target =
          langEntry[QStringLiteral("target")].toString().trimmed();
      if (!target.isEmpty() && !valid.contains(target)) {
        langEntry.remove(QStringLiteral("target"));
        modChanged  = true;
      }

      const QStringList translations =
          normalizedLinkedMods(jsonArrayToStringList(langEntry[QStringLiteral("translations")]));
      QStringList keptTranslations;
      for (const QString& translation : translations) {
        if (valid.contains(translation))
          keptTranslations.append(translation);
      }
      keptTranslations = normalizedLinkedMods(keptTranslations);
      if (keptTranslations != translations) {
        QJsonArray arr;
        for (const QString& translation : keptTranslations)
          arr.append(translation);
        langEntry[QStringLiteral("translations")] = arr;
        modChanged = true;
      }

      if (modChanged)
        langIt.value() = langEntry;
    }

    if (modChanged) {
      modObj[QStringLiteral("langs")] = langs;
      it.value()                      = modObj;
      jsonChanged                     = true;
    }
  }

  if (!jsonChanged && !linksTrimmed && !memChanged)
    return;

  root[QStringLiteral("mods")]  = mods;
  root[QStringLiteral("v")]     = kCacheJsonVersion;
  m_organizer->setPersistent(QStringLiteral("lwizard"), kPersistentKey(),
                             QVariant(QJsonDocument(root).toJson(QJsonDocument::Compact)), true);
}

void BG3LocalizationContent::filterModsJsonToSingleLanguage(QJsonObject& mods,
                                                            const QString& keepLang) const
{
  QJsonObject out;
  for (auto it = mods.begin(); it != mods.end(); ++it) {
    QJsonObject modObj = it->toObject();
    QJsonObject langs  = modObj[QStringLiteral("langs")].toObject();
    if (!langs.contains(keepLang))
      continue;
    QJsonObject one;
    one[keepLang]                = langs[keepLang];
    modObj[QStringLiteral("langs")] = one;
    out[it.key()]                = modObj;
  }
  mods = out;
}

void BG3LocalizationContent::savePersistentFromMemory()
{
  QHash<QString, CacheEntry> snapshot;
  {
    auto lock = QMutexLocker(&m_cacheMutex);
    snapshot = m_cache;
  }
  savePersistentFromMemory(snapshot);
}

void BG3LocalizationContent::savePersistentFromMemory(
    const QHash<QString, CacheEntry>& entries)
{
  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"), kPersistentKey(),
                                                 QVariant()));

  QJsonObject root;
  if (!raw.isEmpty()) {
    QJsonParseError err;
    const QJsonDocument existing = QJsonDocument::fromJson(raw, &err);
    if (err.error == QJsonParseError::NoError && existing.isObject())
      root = existing.object();
  }

  root[QStringLiteral("v")] = kCacheJsonVersion;
  QJsonObject mods            = root[QStringLiteral("mods")].toObject();

  for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
    if (it->contentId == CONTENT_NONE || it->fingerprint.isEmpty() ||
        !it->relationshipsKnown)
      continue;

    const QString          modName    = it.key();
    const QString          entryLang  = it->language;
    MOBase::IModInterface* mod        = m_organizer->modList()->getMod(modName);
    if (!mod)
      continue;

    QJsonObject modObj = mods[modName].toObject();
    modObj[QStringLiteral("path")] = mod->absolutePath();
    QJsonObject langs              = modObj[QStringLiteral("langs")].toObject();
    QJsonObject le;
    le[QStringLiteral("id")]         = it->contentId;
    le[QStringLiteral("fp")]         = it->fingerprint;
    le[QStringLiteral("linksKnown")] = true;
    if (!it->translationTarget.isEmpty())
      le[QStringLiteral("target")] = it->translationTarget;
    if (!it->separateTranslations.isEmpty()) {
      QJsonArray translations;
      for (const QString& translation : it->separateTranslations)
        translations.append(translation);
      le[QStringLiteral("translations")] = translations;
    }
    langs[entryLang] = le;
    modObj[QStringLiteral("langs")] = langs;
    mods[modName]                     = modObj;
  }

  if (cacheOnlyCurrentLanguage())
    filterModsJsonToSingleLanguage(mods, currentLanguage());

  root[QStringLiteral("mods")] = mods;
  m_organizer->setPersistent(QStringLiteral("lwizard"), kPersistentKey(),
                             QVariant(QJsonDocument(root).toJson(QJsonDocument::Compact)),
                             true);
}

void BG3LocalizationContent::prunePersistentCacheToCurrentLanguageOnly()
{
  const QString lang = currentLanguage();

  auto pruneKey = [&](const QString& key, int version) {
    const QByteArray raw =
        variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"), key, QVariant()));
    if (raw.isEmpty())
      return;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
      return;

    QJsonObject root = doc.object();
    QJsonObject mods = root[QStringLiteral("mods")].toObject();
    filterModsJsonToSingleLanguage(mods, lang);

    root[QStringLiteral("mods")] = mods;
    root[QStringLiteral("v")]    = version;
    m_organizer->setPersistent(QStringLiteral("lwizard"), key,
                               QVariant(QJsonDocument(root).toJson(QJsonDocument::Compact)), true);
  };

  pruneKey(kPersistentKey(), kCacheJsonVersion);
  pruneKey(kStringsPersistentKey(), 1);

  LWizardLog::info(
      QStringLiteral("Disk localization cache trimmed to \"%1\" only (other languages "
                     "removed).")
          .arg(lang));
}

bool BG3LocalizationContent::cacheOnlyCurrentLanguage() const
{
  const QVariant v = m_organizer->pluginSetting(
      QStringLiteral("lwizard"), QStringLiteral("cache_only_current_language"));
  return v.isValid() ? v.toBool() : false;
}

QStringList BG3LocalizationContent::validModNames() const
{
  const QStringList allMods = m_organizer->modList()->allMods();
  QStringList       mods;
  for (const QString& name : allMods) {
    if (!(m_organizer->modList()->state(name) & MOBase::IModList::STATE_VALID))
      continue;
    auto* mod = m_organizer->modList()->getMod(name);
    if (mod && mod->isSeparator())
      continue;
    mods.append(name);
  }
  return mods;
}

void BG3LocalizationContent::scanModAsync(const QString& modName)
{
  const QString name = modName.trimmed();
  if (name.isEmpty())
    return;

  int queuedCount = 0;
  {
    auto lock = QMutexLocker(&m_autoScanMutex);
    if (m_autoScanPending.contains(name)) {
      LWizardLog::debug(QStringLiteral("Automatic scan already queued: %1").arg(name));
      return;
    }

    m_autoScanPending.insert(name);
    m_autoScanQueue.append(name);
    queuedCount = m_autoScanQueue.size();
  }

  LWizardLog::info(QStringLiteral("Queued automatic scan for newly installed mod: %1 "
                                  "(waiting: %2)")
                       .arg(name)
                       .arg(queuedCount));

  QMetaObject::invokeMethod(
      this, [this]() { startNextQueuedAutoScan(); }, Qt::QueuedConnection);
}

bool BG3LocalizationContent::scanAll()
{
  int clearedQueuedScans = 0;
  {
    auto lock = QMutexLocker(&m_autoScanMutex);
    if (m_scanning.load() || m_autoScanRunning)
      return false;

    m_scanning.store(true);
    clearedQueuedScans = m_autoScanQueue.size();
    if (clearedQueuedScans > 0) {
      m_autoScanQueue.clear();
      m_autoScanPending.clear();
    }
  }

  const QString language = currentLanguage();

  const QStringList mods = validModNames();

  if (clearedQueuedScans > 0) {
    LWizardLog::info(
        QStringLiteral("Manual full scan superseded %1 queued automatic scan(s).")
            .arg(clearedQueuedScans));
  }

  LWizardLog::info(
      QStringLiteral("Scan started — language: %1, mods: %2").arg(language).arg(mods.size()));

  auto* thread = QThread::create([this, language, mods]() {
    QElapsedTimer totalTimer;
    totalTimer.start();
    ScanMetrics      metrics;
    PakManifestCache pakManifestCache;

    int foundEmbedded    = 0;
    int foundTranslation = 0;
    int skipped          = 0;

    QHash<QString, int>            scanBaseId;
    QHash<QString, QSet<QString>> scanUuids;
    QHash<QString, QString>        scanPath;
    QHash<QString, QString>        scanFp;

    int done = 0;
    for (const QString& modName : mods) {
      auto emitProgress = [&]() {
        QMetaObject::invokeMethod(
            this, [this, done, total = mods.size(), modName]() {
              emit scanProgress(done, total, modName);
            }, Qt::QueuedConnection);
      };

      auto* mod = m_organizer->modList()->getMod(modName);
      if (!mod) {
        ++done;
        emitProgress();
        continue;
      }

      const QString modPath = mod->absolutePath();
      QString fp;
      {
        ScopedScanTimer timer(&metrics.fingerprintMs);
        fp = localizationFingerprint(modPath);
      }

      {
        auto lock = QMutexLocker(&m_cacheMutex);
        auto cit  = m_cache.constFind(modName);
        if (cit != m_cache.constEnd() && cit->language == language &&
            cit->contentId != CONTENT_NONE && !cit->fingerprint.isEmpty() &&
            cit->fingerprint == fp && cit->relationshipsKnown) {
          if (cit->contentId == CONTENT_EMBEDDED)
            ++foundEmbedded;
          else if (cit->contentId == CONTENT_TRANSLATION_MOD)
            ++foundTranslation;
          ++skipped;
          LWizardLog::debug(QStringLiteral("  [cache hit] ") + modName);
          ++done;
          emitProgress();
          continue;
        }
      }

      auto tree = mod->fileTree();
      if (!tree) {
        ++done;
        emitProgress();
        continue;
      }

      QStringList topEntries;
      for (const auto& e : *tree)
        topEntries.append(e->name() + (e->isDir() ? QStringLiteral("/") : QStringLiteral("")));
      LWizardLog::debug(
          QStringLiteral("  [%1] top entries: ").arg(modName) +
          topEntries.join(QStringLiteral(", ")));

      int baseId = CONTENT_NONE;
      {
        ScopedScanTimer timer(&metrics.detectMs);
        baseId = detectContentId(tree, language, &pakManifestCache, &metrics);
      }

      scanBaseId[modName] = baseId;
      scanPath[modName]   = modPath;
      scanFp[modName]     = fp;

      // Always union UUIDs from every language present on disk / in paks. The scan
      // language only affects detectContentId (embedded vs unavailable for *this* lang);
      // translation-mod matching needs cross-language handles (e.g. English base vs RUS).
      {
        ScopedScanTimer timer(&metrics.uuidMs);
        scanUuids[modName] = uuidKeysUnionAllLanguages(modPath, &pakManifestCache, &metrics);
      }
      if (scanUuids[modName].isEmpty())
        scanBaseId[modName] = CONTENT_NONE;
      LWizardLog::debug(QStringLiteral("  [%1] UUID keys (all languages): %2")
                            .arg(modName)
                            .arg(scanUuids[modName].size()));
      ++done;
      emitProgress();
    }

    const QList<QString> rescanned = scanBaseId.keys();

    QHash<QString, int> finalIds;
    QHash<QString, QString> translationCandToRef;

    {
      ScopedScanTimer timer(&metrics.matchMs);
      const QHash<QString, QSet<QString>> persistentUuidsByMod = preloadPersistentUuidKeys();
      QHash<QString, QStringList> uuidIndexByUuid;
      for (const QString& indexedModName : mods) {
        QSet<QString> uuids = scanUuids.value(indexedModName);
        uuids |= persistentUuidsByMod.value(indexedModName);
        if (uuids.size() < kMinUuidsTranslation)
          continue;
        for (const QString& uuid : uuids)
          uuidIndexByUuid[uuid].append(indexedModName);
      }

      for (const QString& modName : rescanned) {
        const int                     baseId = scanBaseId[modName];
        const QPair<int, QString> pr =
            applyTranslationModClassification(modName, baseId, mods, scanBaseId, scanUuids,
                                              persistentUuidsByMod, uuidIndexByUuid);
        finalIds[modName] = pr.first;
        if (pr.first == CONTENT_TRANSLATION_MOD && !pr.second.isEmpty())
          translationCandToRef[modName] = pr.second;
      }
    }

    QHash<QString, QStringList> baseToTranslations;
    for (auto it = translationCandToRef.constBegin(); it != translationCandToRef.constEnd(); ++it)
      baseToTranslations[it.value()].append(it.key());
    for (auto it = baseToTranslations.begin(); it != baseToTranslations.end(); ++it)
      it.value() = normalizedLinkedMods(it.value());

    QSet<QString> refModsWithSeparateTranslation;
    for (auto it = translationCandToRef.constBegin(); it != translationCandToRef.constEnd(); ++it)
      refModsWithSeparateTranslation.insert(it.value());

    for (const QString& modName : rescanned) {
      if (!refModsWithSeparateTranslation.contains(modName))
        continue;
      if (finalIds[modName] != CONTENT_UNAVAILABLE)
        continue;
      finalIds[modName] = CONTENT_INSTALLED;
      LWizardLog::info(
          QStringLiteral("[translation-installed-for-base] ") + modName);
    }

    int foundInstalled = 0;
    QHash<QString, QByteArray> embeddedStringsByMod;
    QHash<QString, CacheEntry> updatedEntries;
    for (const QString& modName : rescanned) {
      const int finalId = finalIds[modName];
      CacheEntry entry;
      entry.language            = language;
      entry.contentId           = finalId;
      entry.fingerprint         = scanFp[modName];
      entry.translationTarget   = translationCandToRef.value(modName);
      entry.separateTranslations = baseToTranslations.value(modName);
      entry.relationshipsKnown  = true;

      updatedEntries.insert(modName, entry);

      if (finalId == CONTENT_EMBEDDED) {
        ScopedScanTimer timer(&metrics.uuidMs);
        const QByteArray compressed =
            buildEmbeddedStringsForMod(scanPath[modName], language, &pakManifestCache, &metrics);
        if (!compressed.isEmpty())
          embeddedStringsByMod.insert(modName, compressed);
      }

      if (finalId == CONTENT_EMBEDDED) {
        ++foundEmbedded;
        LWizardLog::info(QStringLiteral("[embedded] ") + modName);
      } else if (finalId == CONTENT_TRANSLATION_MOD) {
        ++foundTranslation;
        LWizardLog::info(QStringLiteral("[translation-mod] ") + modName);
      } else if (finalId == CONTENT_INSTALLED) {
        ++foundInstalled;
        if (refModsWithSeparateTranslation.contains(modName) &&
            scanBaseId[modName] == CONTENT_UNAVAILABLE)
          ;  // already logged [translation-installed-for-base]
        else
          LWizardLog::info(QStringLiteral("[translation-redundant] ") + modName);
      } else if (finalId == CONTENT_AVAILABLE) {
        LWizardLog::info(QStringLiteral("[available] ") + modName);
      } else if (finalId == CONTENT_OUTDATED) {
        LWizardLog::info(QStringLiteral("[outdated] ") + modName);
      } else {
        LWizardLog::debug(QStringLiteral("  [-] ") + modName);
      }
    }

    QHash<QString, CacheEntry> persistentSnapshot;
    {
      auto lock = QMutexLocker(&m_cacheMutex);
      for (auto it = updatedEntries.constBegin(); it != updatedEntries.constEnd(); ++it)
        m_cache[it.key()] = it.value();

      for (auto it = baseToTranslations.constBegin(); it != baseToTranslations.constEnd(); ++it) {
        if (scanBaseId.contains(it.key()))
          continue;

        auto cachedBase = m_cache.find(it.key());
        if (cachedBase == m_cache.end() || cachedBase->language != language)
          continue;

        cachedBase->separateTranslations = it.value();
        cachedBase->relationshipsKnown   = true;
        if (cachedBase->contentId == CONTENT_UNAVAILABLE)
          cachedBase->contentId = CONTENT_INSTALLED;
      }
      persistentSnapshot = m_cache;
    }

    LWizardLog::info(
        QStringLiteral("Scan complete — embedded: %1, translation mod: %2, installed/redundant: "
                       "%3, skipped (cache): %4, language: %5")
            .arg(foundEmbedded)
            .arg(foundTranslation)
            .arg(foundInstalled)
            .arg(skipped)
            .arg(language));

    const qint64 totalMs = totalTimer.elapsed();

    QMetaObject::invokeMethod(
        this,
        [this, language, foundEmbedded, foundTranslation, foundInstalled, skipped,
         embeddedStringsByMod, scanPath, scanFp, persistentSnapshot, metrics, totalMs]() mutable {
          QElapsedTimer persistTimer;
          persistTimer.start();
          saveEmbeddedStringsBlobs(embeddedStringsByMod, scanPath, scanFp, language);
          savePersistentFromMemory(persistentSnapshot);
          metrics.persistMs = persistTimer.elapsed();
          LWizardLog::info(
              QStringLiteral("Scan timings — total: %1 ms, fingerprint: %2 ms, detect: %3 ms, "
                             "uuid/string: %4 ms, match: %5 ms, persist: %6 ms")
                  .arg(totalMs)
                  .arg(metrics.fingerprintMs)
                  .arg(metrics.detectMs)
                  .arg(metrics.uuidMs)
                  .arg(metrics.matchMs)
                  .arg(metrics.persistMs));
          LWizardLog::info(
              QStringLiteral("Divine spawns — list-package: %1, extract-package: %2, "
                             "extract-single-file: %3, convert-loca: %4")
                  .arg(metrics.listPackageSpawns)
                  .arg(metrics.extractPackageSpawns)
                  .arg(metrics.extractSingleSpawns)
                  .arg(metrics.convertLocaSpawns));
          m_scanning.store(false);
          emit contentCacheUpdated();
          emit scanFinished();
          startNextQueuedAutoScan();
        },
        Qt::QueuedConnection);
  });

  QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  thread->start();
  return true;
}

BG3LocalizationContent::SingleModScanResult BG3LocalizationContent::computeSingleModScan(
    const QString& modName, const QString& language) const
{
  SingleModScanResult result;
  result.modName  = modName;
  result.language = language;

  auto* mod = m_organizer->modList()->getMod(modName);
  if (!mod) {
    LWizardLog::warn(
        QStringLiteral("Automatic scan skipped; mod disappeared before scanning: %1")
            .arg(modName));
    return result;
  }
  if (mod->isSeparator()) {
    LWizardLog::debug(QStringLiteral("Automatic scan skipped; separator: %1").arg(modName));
    return result;
  }

  if (!(m_organizer->modList()->state(modName) & MOBase::IModList::STATE_VALID)) {
    LWizardLog::warn(
        QStringLiteral("Automatic scan skipped; mod is not in a scannable state: %1")
            .arg(modName));
    return result;
  }

  result.valid   = true;
  result.modPath = mod->absolutePath();
  result.fingerprint = localizationFingerprint(result.modPath);

  {
    auto lock = QMutexLocker(&m_cacheMutex);
    auto cit  = m_cache.constFind(modName);
    if (cit != m_cache.constEnd() && cit->language == language &&
        cit->contentId != CONTENT_NONE && !cit->fingerprint.isEmpty() &&
        cit->fingerprint == result.fingerprint && cit->relationshipsKnown) {
      result.cacheHit          = true;
      result.baseContentId     = cit->contentId;
      result.finalContentId    = cit->contentId;
      result.translationTarget = cit->translationTarget;
      return result;
    }
  }

  auto tree = mod->fileTree();
  if (!tree) {
    result.valid = false;
    LWizardLog::warn(
        QStringLiteral("Automatic scan skipped; could not read file tree for %1").arg(modName));
    return result;
  }

  PakManifestCache pakManifestCache;
  ScanMetrics      metrics;
  result.baseContentId = detectContentId(tree, language, &pakManifestCache, &metrics);
  result.uuidKeys      = uuidKeysUnionAllLanguages(result.modPath, &pakManifestCache, &metrics);
  if (result.uuidKeys.isEmpty()) {
    result.baseContentId  = CONTENT_NONE;
    result.finalContentId = CONTENT_NONE;
    return result;
  }

  QHash<QString, int>            scanBaseId;
  QHash<QString, QSet<QString>> scanUuids;
  scanBaseId.insert(modName, result.baseContentId);
  scanUuids.insert(modName, result.uuidKeys);

  const QPair<int, QString> classification =
      applyTranslationModClassification(modName, result.baseContentId, validModNames(),
                                        scanBaseId, scanUuids, preloadPersistentUuidKeys());
  result.finalContentId    = classification.first;
  result.translationTarget = classification.second;

  if (result.finalContentId == CONTENT_EMBEDDED)
    result.embeddedStrings = buildEmbeddedStringsForMod(result.modPath, language,
                                                        &pakManifestCache, &metrics);

  return result;
}

void BG3LocalizationContent::applySingleModScanResult(const SingleModScanResult& result)
{
  if (!result.valid) {
    finishAutoScan(result.modName);
    return;
  }

  if (result.cacheHit) {
    LWizardLog::info(QStringLiteral("Automatic scan cache hit: %1 [%2]")
                         .arg(result.modName, contentIdLabel(result.finalContentId)));
    emit contentCacheUpdated();
    finishAutoScan(result.modName);
    return;
  }

  CacheEntry  entry;
  QStringList linkedTranslations;
  entry.language          = result.language;
  entry.contentId         = result.finalContentId;
  entry.fingerprint       = result.fingerprint;
  entry.translationTarget = result.translationTarget;
  entry.relationshipsKnown = true;

  {
    auto lock = QMutexLocker(&m_cacheMutex);

    // Remove stale backlinks for this mod before writing its refreshed relationship data.
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
      if (it.key() == result.modName || it->language != result.language)
        continue;

      if (it->separateTranslations.removeAll(result.modName) > 0)
        it->separateTranslations = normalizedLinkedMods(it->separateTranslations);
    }

    m_cache[result.modName] = entry;

    if (!entry.translationTarget.isEmpty()) {
      auto refIt = m_cache.find(entry.translationTarget);
      if (refIt != m_cache.end() && refIt->language == result.language) {
        refIt->separateTranslations =
            normalizedLinkedMods(refIt->separateTranslations + QStringList{result.modName});
        refIt->relationshipsKnown = true;
        if (refIt->contentId == CONTENT_UNAVAILABLE)
          refIt->contentId = CONTENT_INSTALLED;
      }
    }

    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) {
      if (it.key() == result.modName || it->language != result.language)
        continue;
      if (it->translationTarget == result.modName)
        linkedTranslations.append(it.key());
    }

    entry.separateTranslations = normalizedLinkedMods(linkedTranslations);
    if (!entry.separateTranslations.isEmpty() && entry.contentId == CONTENT_UNAVAILABLE)
      entry.contentId = CONTENT_INSTALLED;
    m_cache[result.modName] = entry;
  }

  if (entry.contentId == CONTENT_EMBEDDED && !result.embeddedStrings.isEmpty()) {
    saveEmbeddedStringsBlob(result.modName, result.language, result.modPath,
                            result.fingerprint, result.embeddedStrings);
  }

  savePersistentFromMemory();

  QString suffix;
  if (!entry.translationTarget.isEmpty()) {
    suffix = QStringLiteral(" -> %1").arg(entry.translationTarget);
  } else if (!entry.separateTranslations.isEmpty()) {
    suffix = QStringLiteral(" (%1 linked translation(s))").arg(entry.separateTranslations.size());
  }

  LWizardLog::info(QStringLiteral("Automatic scan finished: %1 [%2]%3")
                       .arg(result.modName, contentIdLabel(entry.contentId), suffix));

  emit contentCacheUpdated();
  finishAutoScan(result.modName);
}

void BG3LocalizationContent::finishAutoScan(const QString& modName)
{
  {
    auto lock = QMutexLocker(&m_autoScanMutex);
    m_autoScanPending.remove(modName);
    m_autoScanRunning = false;
  }

  startNextQueuedAutoScan();
}

void BG3LocalizationContent::startNextQueuedAutoScan()
{
  QString modName;
  {
    auto lock = QMutexLocker(&m_autoScanMutex);
    if (m_scanning.load() || m_autoScanRunning || m_autoScanQueue.isEmpty())
      return;

    modName = m_autoScanQueue.takeFirst();
    m_autoScanRunning = true;
  }

  const QString language = currentLanguage();
  LWizardLog::info(
      QStringLiteral("Automatic scan started: %1 (language: %2)").arg(modName, language));

  auto* thread = QThread::create([this, modName, language]() {
    const SingleModScanResult result = computeSingleModScan(modName, language);
    QMetaObject::invokeMethod(
        this, [this, result]() { applySingleModScanResult(result); }, Qt::QueuedConnection);
  });

  QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  thread->start();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString BG3LocalizationContent::currentLanguage() const
{
  QVariant v = m_organizer->pluginSetting(QStringLiteral("lwizard"),
                                          QStringLiteral("language"));
  if (!v.isValid())
    return QStringLiteral("English");

  // MO2 stores a QStringList when the user hasn't changed the setting yet,
  // and a QString after the user has selected a value from the combo box.
  if (v.typeId() == QMetaType::QStringList) {
    const QStringList list = v.toStringList();
    return list.isEmpty() ? QStringLiteral("English") : list.first();
  }
  const QString s = v.toString();
  return s.isEmpty() ? QStringLiteral("English") : s;
}

QString BG3LocalizationContent::resolvedDivinePath() const
{
  auto lock = QMutexLocker(&m_divineMutex);
  if (m_divinePathResolved)
    return m_divinePath;

  m_divinePathResolved = true;
  m_divinePath         = LWizardDivine::existingExecutable(m_organizer);
  if (!m_divinePath.isEmpty())
    LWizardLog::debug(QStringLiteral("Divine.exe found at ") + m_divinePath);
  else
    LWizardLog::warn(
        QStringLiteral("Divine.exe not found — pak localization scanning disabled"));
  return m_divinePath;
}

bool BG3LocalizationContent::pakHasLocalization(const QString& pakPath,
                                                 const QString& language,
                                                 PakManifestCache* pakManifestCache,
                                                 ScanMetrics* metrics) const
{
  LWizardLog::debug(QStringLiteral("  divine scanning: ") + pakPath);

  const QString needle = QStringLiteral("Localization/") + language + QStringLiteral("/");
  for (const QString& entry : listPakFileEntries(pakPath, pakManifestCache, metrics)) {
    if (entry.contains(needle, Qt::CaseInsensitive))
      return true;
  }
  return false;
}

int BG3LocalizationContent::detectContentId(
    std::shared_ptr<const MOBase::IFileTree> tree, const QString& language,
    PakManifestCache* pakManifestCache, ScanMetrics* metrics) const
{
  auto* mod = m_organizer->modList()->getMod(tree->name());
  if (mod && mod->isSeparator())
    return CONTENT_NONE;

  // 1. Direct check: unpacked mod has Localization/{language}/ at its root
  if (tree->exists(QStringLiteral("Localization/") + language,
                   MOBase::FileTreeEntry::DIRECTORY))
    return CONTENT_EMBEDDED;

  // 1b. Unpacked mod may keep localization under PAK_FILES/<something>/Localization/{language}/
  if (auto pakFiles = tree->findDirectory(QStringLiteral("PAK_FILES"))) {
    for (const auto& entry : *pakFiles) {
      if (!entry->isDir())
        continue;
      auto sub = entry->astree();
      if (!sub)
        continue;
      if (sub->exists(QStringLiteral("Localization/") + language,
                      MOBase::FileTreeEntry::DIRECTORY))
        return CONTENT_EMBEDDED;
    }
  }

  // 2. Scan .pak files for embedded localization
  if (mod) {
    const QString modPath = mod->absolutePath();
    QStringList paks;
    collectPakPathsUnderMod(modPath, &paks);
    for (const QString& pakPath : paks) {
      if (pakHasLocalization(pakPath, language, pakManifestCache, metrics))
        return CONTENT_EMBEDDED;
    }

    if (discoverLanguagesInMod(modPath, pakManifestCache, metrics).isEmpty())
      return CONTENT_NONE;
  }

  // States 1 (INSTALLED), 2 (AVAILABLE), 3 (OUTDATED) require
  // cross-mod analysis or Nexus API — not yet implemented.

  return CONTENT_UNAVAILABLE;
}

QSet<QString> BG3LocalizationContent::uuidKeysFromCompressed(const QByteArray& compressedJson) const
{
  QSet<QString> out;
  if (compressedJson.isEmpty())
    return out;
  const QByteArray json = qUncompress(compressedJson);
  if (json.isEmpty())
    return out;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return out;
  const QJsonObject obj = doc.object();
  for (auto it = obj.begin(); it != obj.end(); ++it)
    out.insert(it.key());
  return out;
}

QSet<QString> BG3LocalizationContent::discoverLanguagesInMod(
    const QString& modAbsPath, PakManifestCache* pakManifestCache, ScanMetrics* metrics) const
{
  QSet<QString> langs;

  const QDir locRoot(modAbsPath + QStringLiteral("/Localization"));
  if (locRoot.exists()) {
    const QFileInfoList entries =
        locRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries)
      langs.insert(fi.fileName());
  }

  const QDir pakFiles(modAbsPath + QStringLiteral("/PAK_FILES"));
  if (pakFiles.exists()) {
    QDirIterator locIt(pakFiles.path(), QStringList{QStringLiteral("Localization")},
                       QDir::Dirs, QDirIterator::Subdirectories);
    while (locIt.hasNext()) {
      locIt.next();
      const QFileInfoList langDirs =
          QDir(locIt.filePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
      for (const QFileInfo& fi : langDirs)
        langs.insert(fi.fileName());
    }
  }

  QStringList paks;
  collectPakPathsUnderMod(modAbsPath, &paks);
  const QRegularExpression re(
      QStringLiteral(R"(Localization[/\\]([^/\\]+)[/\\])"),
      QRegularExpression::CaseInsensitiveOption);
  for (const QString& pakPath : paks) {
    const QStringList pakLines = listPakFileEntries(pakPath, pakManifestCache, metrics);
    for (const QString& line : pakLines) {
      const QString n = normalizedPakEntry(line);
      const QRegularExpressionMatch m = re.match(n);
      if (m.hasMatch())
        langs.insert(m.captured(1));
    }
  }

  return langs;
}

QSet<QString> BG3LocalizationContent::uuidKeysUnionAllLanguages(
    const QString& modAbsPath, PakManifestCache* pakManifestCache, ScanMetrics* metrics) const
{
  const QSet<QString> langs = discoverLanguagesInMod(modAbsPath, pakManifestCache, metrics);
  QSet<QString>       out;
  for (const QString& lang : langs) {
    const QByteArray c = buildEmbeddedStringsForMod(modAbsPath, lang, pakManifestCache, metrics);
    out |= uuidKeysFromCompressed(c);
  }
  return out;
}

QSet<QString> BG3LocalizationContent::embeddedUuidKeysUnionFromPersistent(
    const QString& modName) const
{
  QSet<QString> out;
  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"),
                                                 kStringsPersistentKey(), QVariant()));
  if (raw.isEmpty())
    return out;

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return out;

  const QJsonObject mods = doc.object()[QStringLiteral("mods")].toObject();
  const QJsonObject modO = mods[modName].toObject();
  const QJsonObject langs = modO[QStringLiteral("langs")].toObject();
  for (const QString& langKey : langs.keys()) {
    const QJsonObject le = langs[langKey].toObject();
    const QString     b64 = le[QStringLiteral("data")].toString();
    if (b64.isEmpty())
      continue;
    out |= uuidKeysFromCompressed(QByteArray::fromBase64(b64.toUtf8()));
  }
  return out;
}

QHash<QString, QSet<QString>> BG3LocalizationContent::preloadPersistentUuidKeys() const
{
  QHash<QString, QSet<QString>> out;
  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"),
                                                 kStringsPersistentKey(), QVariant()));
  if (raw.isEmpty())
    return out;

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return out;

  const QJsonObject mods = doc.object()[QStringLiteral("mods")].toObject();
  for (auto modIt = mods.constBegin(); modIt != mods.constEnd(); ++modIt) {
    QSet<QString> modUuids;
    const QJsonObject langs = modIt->toObject()[QStringLiteral("langs")].toObject();
    for (auto langIt = langs.constBegin(); langIt != langs.constEnd(); ++langIt) {
      const QString b64 = langIt->toObject()[QStringLiteral("data")].toString();
      if (b64.isEmpty())
        continue;
      modUuids |= uuidKeysFromCompressed(QByteArray::fromBase64(b64.toUtf8()));
    }
    if (!modUuids.isEmpty())
      out.insert(modIt.key(), modUuids);
  }

  return out;
}

QPair<int, QString> BG3LocalizationContent::applyTranslationModClassification(
    const QString& modName,
    int             baseContentId,
    const QStringList& allModNames,
    const QHash<QString, int>&            baseIdThisScan,
    const QHash<QString, QSet<QString>>& uuidsThisScan,
    const QHash<QString, QSet<QString>>& persistentUuidsByMod,
    const QHash<QString, QStringList>& uuidIndexByUuid) const
{
  const QSet<QString> candUuids = uuidsThisScan.value(modName);

  auto resolveRefBase = [&](const QString& refName) -> int {
    int b = baseIdThisScan.value(refName, CONTENT_NONE);
    if (b == CONTENT_NONE) {
      auto lock = QMutexLocker(&m_cacheMutex);
      auto cit  = m_cache.constFind(refName);
      b           = cit == m_cache.constEnd() ? CONTENT_NONE : cit->contentId;
    }
    return b;
  };

  auto mergedRefUuids = [&](const QString& refName) -> QSet<QString> {
    QSet<QString> u = uuidsThisScan.value(refName);
    u |= persistentUuidsByMod.value(refName);
    return u;
  };

  if (candUuids.size() >= kMinUuidsTranslation) {
    QHash<QString, int> overlapByRef;
    if (!uuidIndexByUuid.isEmpty()) {
      for (const QString& uuid : candUuids) {
        for (const QString& refName : uuidIndexByUuid.value(uuid)) {
          if (refName != modName)
            ++overlapByRef[refName];
        }
      }
    } else {
      for (const QString& refName : allModNames) {
        if (refName == modName)
          continue;
        const QSet<QString> refUuids = mergedRefUuids(refName);
        for (const QString& uuid : candUuids) {
          if (refUuids.contains(uuid))
            ++overlapByRef[refName];
        }
      }
    }

    for (const QString& refName : allModNames) {
      if (refName == modName)
        continue;
      const int inter = overlapByRef.value(refName, 0);
      if (inter <= 0)
        continue;

      const int refBase = resolveRefBase(refName);
      // Do not use another translation-only mod as the reference pool.
      if (refBase == CONTENT_TRANSLATION_MOD)
        continue;

      QSet<QString> refUuids = mergedRefUuids(refName);
      if (refUuids.size() < kMinUuidsTranslation)
        continue;
      // Candidate pool should not be larger than the reference (translation ⊂ original).
      if (candUuids.size() > refUuids.size())
        continue;
      // Equal-sized pools: pick the longer mod display name as the likely translation
      // package (e.g. "… RUS" vs base), so the shorter name stays embedded/unavailable.
      if (candUuids.size() == refUuids.size() && modName.length() <= refName.length())
        continue;

      if (inter < kMinOverlapAbs)
        continue;

      const double recall = double(inter) / double(candUuids.size());
      if (recall >= kOverlapRecall) {
        // Translation mod only when this mod actually ships the *current scan* language
        // (embedded). Otherwise it is a pack for another language → redundant vs scan.
        if (baseContentId == CONTENT_EMBEDDED)
          return qMakePair(CONTENT_TRANSLATION_MOD, refName);
        return qMakePair(CONTENT_INSTALLED, refName);
      }
    }
  }

  // XML-only / non-loca packs often have no UUID overlap; pair by mod name when the base
  // mod has no localization for the scan language.
  const QString nameRef = guessTranslationBaseModName(modName, allModNames);
  if (!nameRef.isEmpty() && nameRef != modName && baseContentId == CONTENT_EMBEDDED) {
    const int refBase = resolveRefBase(nameRef);
    if (refBase == CONTENT_UNAVAILABLE) {
      LWizardLog::debug(QStringLiteral("  [translation-mod name match] ") + modName +
                        QStringLiteral(" → ") + nameRef);
      return qMakePair(CONTENT_TRANSLATION_MOD, nameRef);
    }
  }

  return qMakePair(baseContentId, QString());
}

QByteArray BG3LocalizationContent::loadEmbeddedStringsBlob(const QString& modName,
                                                           const QString& lang) const
{
  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"),
                                                 kStringsPersistentKey(), QVariant()));
  if (raw.isEmpty())
    return {};

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return {};

  const QJsonObject root = doc.object();
  const QJsonObject mods = root[QStringLiteral("mods")].toObject();
  const QJsonObject modO = mods[modName].toObject();
  const QJsonObject langs = modO[QStringLiteral("langs")].toObject();
  const QJsonObject le    = langs[lang].toObject();
  const QString b64       = le[QStringLiteral("data")].toString();
  if (b64.isEmpty())
    return {};
  return QByteArray::fromBase64(b64.toUtf8());
}

void BG3LocalizationContent::saveEmbeddedStringsBlob(const QString& modName, const QString& lang,
                                                     const QString& modPath,
                                                     const QString& fingerprint,
                                                     const QByteArray& compressedJson)
{
  if (compressedJson.isEmpty())
    return;

  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"),
                                                 kStringsPersistentKey(), QVariant()));
  QJsonObject root;
  if (!raw.isEmpty()) {
    QJsonParseError err;
    const QJsonDocument existing = QJsonDocument::fromJson(raw, &err);
    if (err.error == QJsonParseError::NoError && existing.isObject())
      root = existing.object();
  }

  root[QStringLiteral("v")] = 1;
  QJsonObject mods          = root[QStringLiteral("mods")].toObject();
  QJsonObject modObj        = mods[modName].toObject();
  modObj[QStringLiteral("path")] = modPath;
  QJsonObject langs              = modObj[QStringLiteral("langs")].toObject();

  QJsonObject le;
  le[QStringLiteral("fp")]   = fingerprint;
  le[QStringLiteral("data")] = QString::fromUtf8(compressedJson.toBase64());
  langs[lang]                = le;
  modObj[QStringLiteral("langs")] = langs;
  mods[modName]                    = modObj;

  if (cacheOnlyCurrentLanguage())
    filterModsJsonToSingleLanguage(mods, currentLanguage());

  root[QStringLiteral("mods")] = mods;
  m_organizer->setPersistent(QStringLiteral("lwizard"), kStringsPersistentKey(),
                             QVariant(QJsonDocument(root).toJson(QJsonDocument::Compact)), true);
}

void BG3LocalizationContent::saveEmbeddedStringsBlobs(
    const QHash<QString, QByteArray>& compressedJsonByMod,
    const QHash<QString, QString>& modPathByMod,
    const QHash<QString, QString>& fingerprintByMod,
    const QString& lang)
{
  if (compressedJsonByMod.isEmpty())
    return;

  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"),
                                                 kStringsPersistentKey(), QVariant()));
  QJsonObject root;
  if (!raw.isEmpty()) {
    QJsonParseError err;
    const QJsonDocument existing = QJsonDocument::fromJson(raw, &err);
    if (err.error == QJsonParseError::NoError && existing.isObject())
      root = existing.object();
  }

  root[QStringLiteral("v")] = 1;
  QJsonObject mods          = root[QStringLiteral("mods")].toObject();

  for (auto it = compressedJsonByMod.constBegin(); it != compressedJsonByMod.constEnd(); ++it) {
    if (it.value().isEmpty())
      continue;

    const QString modName     = it.key();
    const QString modPath     = modPathByMod.value(modName);
    const QString fingerprint = fingerprintByMod.value(modName);
    if (modPath.isEmpty() || fingerprint.isEmpty())
      continue;

    QJsonObject modObj        = mods[modName].toObject();
    modObj[QStringLiteral("path")] = modPath;
    QJsonObject langs              = modObj[QStringLiteral("langs")].toObject();

    QJsonObject le;
    le[QStringLiteral("fp")]   = fingerprint;
    le[QStringLiteral("data")] = QString::fromUtf8(it.value().toBase64());
    langs[lang]                = le;
    modObj[QStringLiteral("langs")] = langs;
    mods[modName]                    = modObj;
  }

  if (cacheOnlyCurrentLanguage())
    filterModsJsonToSingleLanguage(mods, lang);

  root[QStringLiteral("mods")] = mods;
  m_organizer->setPersistent(QStringLiteral("lwizard"), kStringsPersistentKey(),
                             QVariant(QJsonDocument(root).toJson(QJsonDocument::Compact)), true);
}

QMap<QString, QString>
BG3LocalizationContent::parseEmbeddedStringsCompressedJson(const QByteArray& compressedJson) const
{
  const QByteArray json = qUncompress(compressedJson);
  if (json.isEmpty())
    return {};

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return {};

  QMap<QString, QString> out;
  const QJsonObject obj = doc.object();
  for (auto it = obj.begin(); it != obj.end(); ++it)
    out[it.key()] = it->toString();
  return out;
}

QMap<QString, QString> BG3LocalizationContent::loadStringsSync(const QString& modName,
                                                               const QString& language) const
{
  MOBase::IModInterface* mod = m_organizer->modList()->getMod(modName);
  if (!mod)
    return {};
  if (mod->isSeparator())
    return {};
  const QByteArray compressed = buildEmbeddedStringsForMod(mod->absolutePath(), language);
  if (compressed.isEmpty())
    return {};
  return parseEmbeddedStringsCompressedJson(compressed);
}

QMap<QString, QString> BG3LocalizationContent::embeddedStringsFor(const QString& modName) const
{
  const QString lang = currentLanguage();
  const QByteArray blob = loadEmbeddedStringsBlob(modName, lang);
  if (blob.isEmpty())
    return {};

  // Validate fingerprint before returning.
  MOBase::IModInterface* mod = m_organizer->modList()->getMod(modName);
  if (!mod)
    return {};
  const QString liveFp = localizationFingerprint(mod->absolutePath());

  const QByteArray raw =
      variantToJsonBytes(m_organizer->persistent(QStringLiteral("lwizard"),
                                                 kStringsPersistentKey(), QVariant()));
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return {};
  const QJsonObject mods = doc.object()[QStringLiteral("mods")].toObject();
  const QJsonObject modO = mods[modName].toObject();
  const QJsonObject le =
      modO[QStringLiteral("langs")].toObject()[lang].toObject();
  const QString storedFp = le[QStringLiteral("fp")].toString();
  if (storedFp.isEmpty() || storedFp != liveFp)
    return {};

  return parseEmbeddedStringsCompressedJson(blob);
}

QByteArray BG3LocalizationContent::locaFileToJsonMapCompressed(const QString& locaAbsPath,
                                                               ScanMetrics* metrics) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return {};

  QTemporaryDir tmp;
  if (!tmp.isValid())
    return {};

  const QString xmlPath = tmp.path() + QStringLiteral("/out.xml");
  if (metrics)
    ++metrics->convertLocaSpawns;

  QProcess proc;
  proc.start(divine,
             {QStringLiteral("-g"), QStringLiteral("bg3"),
              QStringLiteral("-a"), QStringLiteral("convert-loca"),
              QStringLiteral("-s"), locaAbsPath,
              QStringLiteral("-d"), xmlPath});
  if (!proc.waitForStarted(5000) || !proc.waitForFinished(60000)) {
    proc.kill();
    return {};
  }
  if (proc.exitCode() != 0)
    return {};

  QFile f(xmlPath);
  if (!f.open(QIODevice::ReadOnly))
    return {};

  const QJsonObject out = parseBg3LocalizationXmlContent(&f);
  if (out.isEmpty())
    return {};

  const QByteArray json = QJsonDocument(out).toJson(QJsonDocument::Compact);
  return qCompress(json, 9);
}

QByteArray BG3LocalizationContent::mergeJsonMapsCompressed(
    const QList<QByteArray>& compressedJsonMaps) const
{
  QJsonObject merged;
  for (const QByteArray& c : compressedJsonMaps) {
    if (c.isEmpty())
      continue;
    const QByteArray json = qUncompress(c);
    if (json.isEmpty())
      continue;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
      continue;
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
      merged[it.key()] = it.value();
  }

  if (merged.isEmpty())
    return {};
  return qCompress(QJsonDocument(merged).toJson(QJsonDocument::Compact), 9);
}

void BG3LocalizationContent::collectPakPathsUnderMod(const QString& modAbsPath,
                                                     QStringList* outPaks) const
{
  if (!outPaks)
    return;

  QSet<QString> seen;
  auto add = [&](const QString& p) {
    if (seen.contains(p))
      return;
    seen.insert(p);
    outPaks->append(p);
  };

  const QDir root(modAbsPath);
  if (root.exists()) {
    const QFileInfoList rootPaks =
        root.entryInfoList(QStringList{QStringLiteral("*.pak")}, QDir::Files);
    for (const QFileInfo& fi : rootPaks)
      add(fi.absoluteFilePath());
  }

  const QDir pakRoot(modAbsPath + QStringLiteral("/PAK_FILES"));
  if (pakRoot.exists()) {
    QDirIterator it(pakRoot.path(), QStringList{QStringLiteral("*.pak")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      add(it.filePath());
    }
  }
}

QStringList BG3LocalizationContent::listPakFileEntries(const QString& pakPath,
                                                       PakManifestCache* pakManifestCache,
                                                       ScanMetrics* metrics) const
{
  if (pakManifestCache) {
    auto it = pakManifestCache->entriesByPakPath.constFind(pakPath);
    if (it != pakManifestCache->entriesByPakPath.constEnd())
      return it.value();
  }

  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return {};

  if (metrics)
    ++metrics->listPackageSpawns;

  QProcess proc;
  proc.start(divine,
             {QStringLiteral("-g"), QStringLiteral("bg3"),
              QStringLiteral("-a"), QStringLiteral("list-package"),
              QStringLiteral("-s"), pakPath});

  if (!proc.waitForStarted(5000) || !proc.waitForFinished(120000)) {
    proc.kill();
    LWizardLog::warn(QStringLiteral("Divine.exe timed out listing package ") + pakPath);
    return {};
  }

  if (proc.exitCode() != 0)
    return {};

  QStringList out;
  const QString output = QString::fromUtf8(proc.readAllStandardOutput());
  const QStringList lines = output.split(QChar('\n'), Qt::SkipEmptyParts);
  for (const QString& line : lines) {
    const QString name = normalizedPakEntry(line);
    if (!name.isEmpty())
      out.append(name);
  }
  if (pakManifestCache)
    pakManifestCache->entriesByPakPath.insert(pakPath, out);
  return out;
}

bool BG3LocalizationContent::extractPakLocalizationsToDir(const QString& pakPath,
                                                          const QString& destDir,
                                                          ScanMetrics* metrics) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return false;

  if (metrics)
    ++metrics->extractPackageSpawns;

  QProcess proc;
  proc.start(divine,
             {QStringLiteral("-g"), QStringLiteral("bg3"),
              QStringLiteral("-a"), QStringLiteral("extract-package"),
              QStringLiteral("-s"), pakPath,
              QStringLiteral("-d"), destDir,
              QStringLiteral("-x"), QStringLiteral(R"(Localization[/\\].*\.(loca|xml)$)"),
              QStringLiteral("--use-regex")});

  if (!proc.waitForStarted(5000) || !proc.waitForFinished(120000)) {
    proc.kill();
    LWizardLog::debug(QStringLiteral("  extract-package timed out: ") + pakPath);
    return false;
  }

  if (proc.exitCode() != 0) {
    LWizardLog::debug(QStringLiteral("  extract-package failed (code %1): %2")
                          .arg(proc.exitCode())
                          .arg(pakPath));
    return false;
  }

  return true;
}

bool BG3LocalizationContent::extractPakEntryToFile(const QString& pakPath,
                                                   const QString& packagedPath,
                                                   const QString& destAbsFile,
                                                   ScanMetrics* metrics) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return false;

  if (metrics)
    ++metrics->extractSingleSpawns;

  QProcess proc;
  proc.start(divine,
             {QStringLiteral("-g"), QStringLiteral("bg3"),
              QStringLiteral("-a"), QStringLiteral("extract-single-file"),
              QStringLiteral("-s"), pakPath,
              QStringLiteral("-d"), destAbsFile,
              QStringLiteral("-f"), packagedPath});

  if (!proc.waitForStarted(5000) || !proc.waitForFinished(120000)) {
    proc.kill();
    LWizardLog::debug(QStringLiteral("  extract-single-file timed out: ") + packagedPath);
    return false;
  }

  if (proc.exitCode() != 0) {
    LWizardLog::debug(
        QStringLiteral("  extract-single-file failed (code %1): %2")
            .arg(proc.exitCode())
            .arg(packagedPath));
    return false;
  }

  return QFile::exists(destAbsFile);
}

QByteArray BG3LocalizationContent::buildEmbeddedStringsForMod(const QString& modAbsPath,
                                                              const QString& lang,
                                                              PakManifestCache* pakManifestCache,
                                                              ScanMetrics* metrics) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return {}; // can't convert .loca

  QList<QByteArray> parts;

  auto scanLocaUnder = [&](const QString& baseDir) {
    QDir d(baseDir);
    if (!d.exists())
      return;
    QDirIterator it(d.path(), QStringList{QStringLiteral("*.loca")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      const QByteArray one = locaFileToJsonMapCompressed(it.filePath(), metrics);
      if (!one.isEmpty())
        parts.append(one);
    }
  };

  auto scanXmlUnder = [&](const QString& baseDir) {
    QDir d(baseDir);
    if (!d.exists())
      return;
    QDirIterator it(d.path(), QStringList{QStringLiteral("*.xml")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      const QByteArray one = localizationXmlFileToJsonMapCompressed(it.filePath());
      if (!one.isEmpty())
        parts.append(one);
    }
  };

  {
    const QString rootLoc = modAbsPath + QStringLiteral("/Localization/") + lang;
    scanLocaUnder(rootLoc);
    scanXmlUnder(rootLoc);
  }

  // Also scan unpacked localization under PAK_FILES/**/Localization/{lang}
  {
    QDir pakDir(modAbsPath + QStringLiteral("/PAK_FILES"));
    if (pakDir.exists()) {
      QDirIterator it(pakDir.path(), QStringList{QStringLiteral("Localization")},
                      QDir::Dirs, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        it.next();
        const QString locRoot = it.filePath() + QStringLiteral("/") + lang;
        scanLocaUnder(locRoot);
        scanXmlUnder(locRoot);
      }
    }
  }

  // .loca/.xml inside .pak: extract localization files once per pak, then filter locally.
  QStringList paks;
  collectPakPathsUnderMod(modAbsPath, &paks);
  const QString locNeedle = QStringLiteral("Localization/") + lang + QStringLiteral("/");

  QTemporaryDir pakTmp;
  if (pakTmp.isValid()) {
    for (const QString& pakPath : paks) {
      const QStringList entries = listPakFileEntries(pakPath, pakManifestCache, metrics);
      bool hasWantedEntry = false;
      for (const QString& entry : entries) {
        const QString norm = normalizedPakEntry(entry);
        if (!norm.contains(locNeedle, Qt::CaseInsensitive))
          continue;
        const bool isLoca = norm.endsWith(QStringLiteral(".loca"), Qt::CaseInsensitive);
        const bool isXml  = norm.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive);
        if (isLoca || isXml) {
          hasWantedEntry = true;
          break;
        }
      }
      if (!hasWantedEntry)
        continue;

      QString pakDest;
      bool    extracted = false;
      if (pakManifestCache) {
        auto extractedIt =
            pakManifestCache->localizationExtractDirsByPakPath.constFind(pakPath);
        if (extractedIt != pakManifestCache->localizationExtractDirsByPakPath.constEnd() &&
            extractedIt.value() && extractedIt.value()->isValid()) {
          pakDest   = extractedIt.value()->path();
          extracted = true;
        } else {
          auto tmp = std::make_shared<QTemporaryDir>();
          if (tmp->isValid() && extractPakLocalizationsToDir(pakPath, tmp->path(), metrics)) {
            pakDest   = tmp->path();
            extracted = true;
            pakManifestCache->localizationExtractDirsByPakPath.insert(pakPath, tmp);
          }
        }
      } else {
        pakDest =
            pakTmp.path() + QChar('/') +
            QString::fromLatin1(
                QCryptographicHash::hash(pakPath.toUtf8(), QCryptographicHash::Sha256).toHex());
        QDir().mkpath(pakDest);
        extracted = extractPakLocalizationsToDir(pakPath, pakDest, metrics);
      }

      if (extracted) {
        scanLocaUnder(pakDest + QStringLiteral("/Localization/") + lang);
        scanXmlUnder(pakDest + QStringLiteral("/Localization/") + lang);
        continue;
      }

      for (const QString& entry : entries) {
        const QString norm = normalizedPakEntry(entry);
        if (!norm.contains(locNeedle, Qt::CaseInsensitive))
          continue;

        const bool isLoca = norm.endsWith(QStringLiteral(".loca"), Qt::CaseInsensitive);
        const bool isXml  = norm.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive);
        if (!isLoca && !isXml)
          continue;

        const QString dest =
            pakTmp.path() + QChar('/') +
            QString::fromLatin1(
                QCryptographicHash::hash(entry.toUtf8(), QCryptographicHash::Sha256).toHex()) +
            (isLoca ? QStringLiteral(".loca") : QStringLiteral(".xml"));
        if (!extractPakEntryToFile(pakPath, entry, dest, metrics))
          continue;
        const QByteArray one =
            isLoca ? locaFileToJsonMapCompressed(dest, metrics) :
                     localizationXmlFileToJsonMapCompressed(dest);
        if (!one.isEmpty())
          parts.append(one);
        QFile::remove(dest);
      }
    }
  }

  return mergeJsonMapsCompressed(parts);
}
