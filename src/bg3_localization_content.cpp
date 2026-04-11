#include "bg3_localization_content.h"
#include "lwizard_divine.h"
#include "lwizard_log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
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
      const QFileInfoList paks =
          pakDir.entryInfoList(QStringList{QStringLiteral("*.pak")}, QDir::Files);
      for (const QFileInfo& fi : paks)
        appendFile(QStringLiteral("PAK_FILES/") + fi.fileName(), fi);
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
    if (mod->absolutePath().compare(storedPath, Qt::CaseInsensitive) != 0)
      continue;

    const int     id = langEntry[QStringLiteral("id")].toInt(CONTENT_NONE);
    const QString fp = langEntry[QStringLiteral("fp")].toString();
    if (id == CONTENT_NONE || fp.isEmpty())
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

  {
    auto lock = QMutexLocker(&m_cacheMutex);
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) {
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

bool BG3LocalizationContent::scanAll()
{
  if (m_scanning.exchange(true))
    return false;  // already running

  const QString language = currentLanguage();

  // Pre-filter to valid (real) mods only, discarding Overwrite/DLC stubs.
  const QStringList allMods = m_organizer->modList()->allMods();
  QStringList       mods;
  for (const QString& name : allMods)
    if (m_organizer->modList()->state(name) & MOBase::IModList::STATE_VALID)
      mods.append(name);

  LWizardLog::info(
      QStringLiteral("Scan started — language: %1, mods: %2").arg(language).arg(mods.size()));

  auto* thread = QThread::create([this, language, mods]() {
    int foundEmbedded    = 0;
    int foundTranslation = 0;
    int skipped          = 0;

    QHash<QString, int>            scanBaseId;
    QHash<QString, QSet<QString>> scanUuids;
    QHash<QString, QString>        scanPath;
    QHash<QString, QString>        scanFp;

    for (const QString& modName : mods) {
      auto* mod = m_organizer->modList()->getMod(modName);
      if (!mod)
        continue;

      const QString modPath = mod->absolutePath();
      const QString fp      = localizationFingerprint(modPath);

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
          continue;
        }
      }

      auto tree = mod->fileTree();
      if (!tree)
        continue;

      QStringList topEntries;
      for (const auto& e : *tree)
        topEntries.append(e->name() + (e->isDir() ? QStringLiteral("/") : QStringLiteral("")));
      LWizardLog::debug(
          QStringLiteral("  [%1] top entries: ").arg(modName) +
          topEntries.join(QStringLiteral(", ")));

      const int baseId = detectContentId(tree, language);

      scanBaseId[modName] = baseId;
      scanPath[modName]   = modPath;
      scanFp[modName]     = fp;

      // Always union UUIDs from every language present on disk / in paks. The scan
      // language only affects detectContentId (embedded vs unavailable for *this* lang);
      // translation-mod matching needs cross-language handles (e.g. English base vs RUS).
      scanUuids[modName] = uuidKeysUnionAllLanguages(modPath);
      LWizardLog::debug(QStringLiteral("  [%1] UUID keys (all languages): %2")
                            .arg(modName)
                            .arg(scanUuids[modName].size()));
    }

    const QList<QString> rescanned = scanBaseId.keys();

    QHash<QString, int> finalIds;
    QHash<QString, QString> translationCandToRef;

    for (const QString& modName : rescanned) {
      const int                     baseId = scanBaseId[modName];
      const QPair<int, QString> pr =
          applyTranslationModClassification(modName, baseId, mods, scanBaseId, scanUuids);
      finalIds[modName] = pr.first;
      if (pr.first == CONTENT_TRANSLATION_MOD && !pr.second.isEmpty())
        translationCandToRef[modName] = pr.second;
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
    for (const QString& modName : rescanned) {
      const int finalId = finalIds[modName];
      CacheEntry entry;
      entry.language            = language;
      entry.contentId           = finalId;
      entry.fingerprint         = scanFp[modName];
      entry.translationTarget   = translationCandToRef.value(modName);
      entry.separateTranslations = baseToTranslations.value(modName);
      entry.relationshipsKnown  = true;

      {
        auto lock        = QMutexLocker(&m_cacheMutex);
        m_cache[modName] = entry;
      }

      if (finalId == CONTENT_EMBEDDED) {
        const QByteArray compressed =
            buildEmbeddedStringsForMod(scanPath[modName], language);
        if (!compressed.isEmpty()) {
          const QString modNameCopy = modName;
          const QString langCopy    = language;
          const QString fpCopy      = scanFp[modName];
          const QString pathCopy    = scanPath[modName];
          QMetaObject::invokeMethod(
              this,
              [this, modNameCopy, langCopy, pathCopy, fpCopy, compressed]() {
                saveEmbeddedStringsBlob(modNameCopy, langCopy, pathCopy, fpCopy, compressed);
              },
              Qt::QueuedConnection);
        }
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

    {
      auto lock = QMutexLocker(&m_cacheMutex);
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
    }

    LWizardLog::info(
        QStringLiteral("Scan complete — embedded: %1, translation mod: %2, installed/redundant: "
                       "%3, skipped (cache): %4, language: %5")
            .arg(foundEmbedded)
            .arg(foundTranslation)
            .arg(foundInstalled)
            .arg(skipped)
            .arg(language));

    QMetaObject::invokeMethod(
        this,
        [this]() {
          savePersistentFromMemory();
          m_scanning.store(false);
          emit scanFinished();
        },
        Qt::QueuedConnection);
  });

  QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  thread->start();
  return true;
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
                                                 const QString& language) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return false;

  QProcess proc;
  proc.start(divine,
             {QStringLiteral("-g"), QStringLiteral("bg3"),
              QStringLiteral("-a"), QStringLiteral("list-package"),
              QStringLiteral("-s"), pakPath});

  LWizardLog::debug(QStringLiteral("  divine scanning: ") + pakPath);

  if (!proc.waitForStarted(5000) || !proc.waitForFinished(30000)) {
    proc.kill();
    LWizardLog::warn(QStringLiteral("Divine.exe timed out scanning ") + pakPath);
    return false;
  }

  const int     code   = proc.exitCode();
  const QString output = QString::fromUtf8(proc.readAllStandardOutput());
  const QString errout = QString::fromUtf8(proc.readAllStandardError());

  LWizardLog::debug(QStringLiteral("  exit=%1 stdout=%2 stderr=%3")
                        .arg(code)
                        .arg(output.left(200))
                        .arg(errout.left(200)));

  if (code != 0)
    return false;

  const QString needle = QStringLiteral("Localization/") + language + QStringLiteral("/");
  return output.contains(needle, Qt::CaseInsensitive);
}

int BG3LocalizationContent::detectContentId(
    std::shared_ptr<const MOBase::IFileTree> tree, const QString& language) const
{
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
  auto* mod = m_organizer->modList()->getMod(tree->name());
  if (mod) {
    const QString modPath = mod->absolutePath();

    auto scanPaksInDir = [&](std::shared_ptr<const MOBase::IFileTree> dir,
                             const QString& relDir) -> bool {
      if (!dir)
        return false;
      for (const auto& entry : *dir) {
        if (entry->isFile() && entry->hasSuffix(QStringLiteral("pak"))) {
          const QString pakPath = modPath + QStringLiteral("/") + relDir + entry->name();
          if (pakHasLocalization(pakPath, language))
            return true;
        }
      }
      return false;
    };

    if (scanPaksInDir(tree->findDirectory(QStringLiteral("PAK_FILES")), QStringLiteral("PAK_FILES/")))
      return CONTENT_EMBEDDED;

    if (scanPaksInDir(tree, QStringLiteral("")))
      return CONTENT_EMBEDDED;
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

QSet<QString> BG3LocalizationContent::discoverLanguagesInMod(const QString& modAbsPath) const
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
    const QStringList pakLines = listPakFileEntries(pakPath);
    for (const QString& line : pakLines) {
      QString n = line;
      n.replace(QChar('\\'), QChar('/'));
      const QRegularExpressionMatch m = re.match(n);
      if (m.hasMatch())
        langs.insert(m.captured(1));
    }
  }

  return langs;
}

QSet<QString> BG3LocalizationContent::uuidKeysUnionAllLanguages(const QString& modAbsPath) const
{
  const QSet<QString> langs = discoverLanguagesInMod(modAbsPath);
  QSet<QString>       out;
  for (const QString& lang : langs) {
    const QByteArray c = buildEmbeddedStringsForMod(modAbsPath, lang);
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

QPair<int, QString> BG3LocalizationContent::applyTranslationModClassification(
    const QString& modName,
    int             baseContentId,
    const QStringList& allModNames,
    const QHash<QString, int>&            baseIdThisScan,
    const QHash<QString, QSet<QString>>& uuidsThisScan) const
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
    u |= embeddedUuidKeysUnionFromPersistent(refName);
    return u;
  };

  if (candUuids.size() >= kMinUuidsTranslation) {
    for (const QString& refName : allModNames) {
      if (refName == modName)
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

      int inter = 0;
      for (const QString& u : candUuids) {
        if (refUuids.contains(u))
          ++inter;
      }
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

QByteArray BG3LocalizationContent::locaFileToJsonMapCompressed(const QString& locaAbsPath) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return {};

  QTemporaryDir tmp;
  if (!tmp.isValid())
    return {};

  const QString xmlPath = tmp.path() + QStringLiteral("/out.xml");
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

QStringList BG3LocalizationContent::listPakFileEntries(const QString& pakPath) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return {};

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
    const int tab = line.indexOf(QChar('\t'));
    const QString name =
        (tab < 0 ? line.trimmed() : line.left(tab).trimmed());
    if (!name.isEmpty())
      out.append(name);
  }
  return out;
}

bool BG3LocalizationContent::extractPakEntryToFile(const QString& pakPath,
                                                   const QString& packagedPath,
                                                   const QString& destAbsFile) const
{
  const QString divine = resolvedDivinePath();
  if (divine.isEmpty())
    return false;

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
                                                              const QString& lang) const
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
      const QByteArray one = locaFileToJsonMapCompressed(it.filePath());
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

  // .loca inside .pak: list entries, extract matching Localization/<lang>/*.loca, convert.
  QStringList paks;
  collectPakPathsUnderMod(modAbsPath, &paks);
  const QString locNeedle = QStringLiteral("Localization/") + lang + QStringLiteral("/");

  QTemporaryDir pakTmp;
  if (pakTmp.isValid()) {
    for (const QString& pakPath : paks) {
      const QStringList entries = listPakFileEntries(pakPath);
      for (const QString& entry : entries) {
        QString norm = entry;
        norm.replace(QChar('\\'), QChar('/'));
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
        if (!extractPakEntryToFile(pakPath, entry, dest))
          continue;
        const QByteArray one =
            isLoca ? locaFileToJsonMapCompressed(dest) : localizationXmlFileToJsonMapCompressed(dest);
        if (!one.isEmpty())
          parts.append(one);
        QFile::remove(dest);
      }
    }
  }

  return mergeJsonMapsCompressed(parts);
}
