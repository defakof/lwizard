#include "bg3_localization_content.h"
#include "lwizard_log.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>
#include <QThread>
#include <QVariant>

#include <uibase/ifiletree.h>
#include <uibase/imodinterface.h>
#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

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
      Content(CONTENT_INSTALLED,   "Translation: Installed (separate)", ":/lwizard/2_installed.ico"),
      Content(CONTENT_AVAILABLE,   "Translation: Available on Nexus",   ":/lwizard/3_available.ico"),
      Content(CONTENT_OUTDATED,    "Translation: Installed, outdated",  ":/lwizard/4_outdated.ico"),
      Content(CONTENT_UNAVAILABLE, "Translation: Not available",        ":/lwizard/5_unavailable.ico"),
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

bool BG3LocalizationContent::scanAll()
{
  if (m_scanning.exchange(true))
    return false;  // already running

  const QString language = currentLanguage();

  // Pre-filter to valid (real) mods only, discarding Overwrite/DLC stubs.
  const QStringList allMods = m_organizer->modList()->allMods();
  QStringList mods;
  for (const QString& name : allMods)
    if (m_organizer->modList()->state(name) & MOBase::IModList::STATE_VALID)
      mods.append(name);

  LWizardLog::info(
      QStringLiteral("Scan started — language: %1, mods: %2").arg(language).arg(mods.size()));

  auto* thread = QThread::create([this, language, mods]() {
    int found = 0;
    for (const QString& modName : mods) {
      auto* mod = m_organizer->modList()->getMod(modName);
      if (!mod)
        continue;

      auto tree = mod->fileTree();
      if (!tree)
        continue;

      // Diagnostic: list top-level entries
      QStringList topEntries;
      for (const auto& e : *tree)
        topEntries.append(e->name() + (e->isDir() ? QStringLiteral("/") : QStringLiteral("")));
      LWizardLog::debug(
          QStringLiteral("  [%1] top entries: ").arg(modName) +
          topEntries.join(QStringLiteral(", ")));

      const int contentId = detectContentId(tree, language);

      {
        auto lock        = QMutexLocker(&m_cacheMutex);
        m_cache[modName] = {language, contentId};
      }

      if (contentId == CONTENT_EMBEDDED) {
        ++found;
        LWizardLog::info(QStringLiteral("[embedded] ") + modName);
      } else if (contentId == CONTENT_INSTALLED) {
        LWizardLog::info(QStringLiteral("[installed] ") + modName);
      } else if (contentId == CONTENT_AVAILABLE) {
        LWizardLog::info(QStringLiteral("[available] ") + modName);
      } else if (contentId == CONTENT_OUTDATED) {
        LWizardLog::info(QStringLiteral("[outdated] ") + modName);
      } else {
        LWizardLog::debug(QStringLiteral("  [-] ") + modName);
      }
    }

    LWizardLog::info(
        QStringLiteral("Scan complete — %1/%2 mods have embedded %3 localization")
            .arg(found)
            .arg(mods.size())
            .arg(language));

    m_scanning.store(false);
    emit scanFinished();
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
  const QString base   = m_organizer->basePath();

  // 1. Known location shipped with the unofficial BG3 plugin
  const QString known =
      base + "/plugins/basic_games/games/baldursgate3/tools/Divine.exe";
  if (QFileInfo::exists(known)) {
    m_divinePath = QDir::toNativeSeparators(known);
    LWizardLog::debug(QStringLiteral("Divine.exe found at ") + m_divinePath);
    return m_divinePath;
  }

  // 2. Search the entire plugins folder recursively
  QDirIterator it(base + "/plugins", QStringList{QStringLiteral("Divine.exe")},
                  QDir::Files, QDirIterator::Subdirectories);
  if (it.hasNext()) {
    m_divinePath = QDir::toNativeSeparators(it.next());
    LWizardLog::debug(QStringLiteral("Divine.exe found at ") + m_divinePath);
    return m_divinePath;
  }

  // 3. Download target (set by LWizardPlugin::findOrDownloadDivine)
  const QString downloaded = base + "/plugins/lwizard/Divine.exe";
  if (QFileInfo::exists(downloaded)) {
    m_divinePath = QDir::toNativeSeparators(downloaded);
    LWizardLog::debug(QStringLiteral("Divine.exe found at ") + m_divinePath);
    return m_divinePath;
  }

  LWizardLog::warn(QStringLiteral("Divine.exe not found — pak localization scanning disabled"));
  return {};
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
