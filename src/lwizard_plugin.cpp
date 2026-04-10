#include "lwizard_plugin.h"
#include "bg3_localization_content.h"
#include "lwizard_log.h"
#include "lwizard_window.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QThread>

#include <uibase/game_features/igamefeatures.h>
#include <uibase/iplugingame.h>
#include <uibase/imoinfo.h>

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool LWizardPlugin::init(MOBase::IOrganizer* organizer)
{
  m_organizer = organizer;

  m_localizationContent = std::make_shared<BG3LocalizationContent>(organizer);

  // After a manual scan finishes, trigger a soft MO2 refresh so the Content
  // column re-queries getContentsFor() and picks up the newly cached results.
  QObject::connect(m_localizationContent.get(), &BG3LocalizationContent::scanFinished,
                   [this]() { m_organizer->refresh(false); });

  // Register ModDataContent after the UI is up (same pattern as mo2-bg3-translation-
  // checker’s plugin_content.py). Early init() registration can leave the Content
  // column disabled in the header menu until modDataContents() is populated.

  // Clear the scan cache after each MO2 refresh so stale results are dropped
  organizer->onNextRefresh(
      [this]() {
        m_localizationContent->clearCache();
      },
      /*immediateIfPossible=*/false);

  // Clear cache when the user changes the language setting
  organizer->onPluginSettingChanged(
      [this](const QString& plugin, const QString& key, const QVariant&,
             const QVariant&) {
        if (plugin == name() && key == QStringLiteral("language"))
          m_localizationContent->clearCache();
      });

  organizer->onUserInterfaceInitialized([this](QMainWindow*) {
    registerLocalizationContentFeature();
    findOrDownloadDivine();
  });

  return true;
}

void LWizardPlugin::registerLocalizationContentFeature()
{
  if (m_contentFeatureRegistered || !m_organizer || !m_localizationContent)
    return;

  auto* features = m_organizer->gameFeatures();
  // registerFeature(IPluginGame*, …) matches the Python reference (managedGame()).
  auto* game =
      const_cast<MOBase::IPluginGame*>(m_organizer->managedGame());
  bool ok = false;
  if (game != nullptr) {
    ok = features->registerFeature(game, m_localizationContent,
                                   /*priority=*/10, /*replace=*/true);
  }
  if (!ok) {
    ok = features->registerFeature(
        QStringList{QStringLiteral("baldursgate3")}, m_localizationContent,
        /*priority=*/10, /*replace=*/true);
  }
  m_contentFeatureRegistered = ok;
}

// ---------------------------------------------------------------------------
// Divine.exe discovery / download
// ---------------------------------------------------------------------------

void LWizardPlugin::findOrDownloadDivine()
{
  const QString base = m_organizer->basePath();

  // 1. Known location from the unofficial BG3 plugin
  if (QFileInfo::exists(
          base + "/plugins/basic_games/games/baldursgate3/tools/Divine.exe"))
    return;

  // 2. Search MO2 plugins folder
  QDirIterator it(base + "/plugins", QStringList{QStringLiteral("Divine.exe")},
                  QDir::Files, QDirIterator::Subdirectories);
  if (it.hasNext())
    return;

  // 3. Already downloaded by a previous run
  if (QFileInfo::exists(base + "/plugins/lwizard/Divine.exe"))
    return;

  // Not found anywhere — download on a background thread
  const QString targetDir =
      QDir::toNativeSeparators(base + "/plugins/lwizard");

  LWizardLog::info(QStringLiteral("Divine.exe not found; downloading LSLib to ") + targetDir);

  auto* thread = QThread::create([targetDir]() {
    static constexpr const char* k_zipUrl =
        "https://github.com/Norbyte/lslib/releases/download/v1.19.5/"
        "ExportTool-v1.19.5.zip";

    const QString tempZip =
        QDir::temp().filePath(QStringLiteral("ExportTool-v1.19.5.zip"));

    // --- Download ---
    const int dlRet = QProcess::execute(
        QStringLiteral("powershell"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QString::fromLatin1(
             "[Net.ServicePointManager]::SecurityProtocol = "
             "[Net.SecurityProtocolType]::Tls12; "
             "Invoke-WebRequest -Uri '%1' -OutFile '%2'")
             .arg(QString::fromLatin1(k_zipUrl), tempZip)});

    if (dlRet != 0) {
      LWizardLog::warn(QStringLiteral("Failed to download LSLib (exit code %1)").arg(dlRet));
      return;
    }

    // --- Extract Tools/* entries from the zip ---
    QDir().mkpath(targetDir);

    const QString psExtract = QString::fromLatin1(
        "Add-Type -Assembly 'System.IO.Compression.FileSystem';"
        "$zip = [IO.Compression.ZipFile]::OpenRead('%1');"
        "foreach ($e in $zip.Entries) {"
        "  if ($e.FullName -like 'Tools/*' -and $e.Name -ne '') {"
        "    $dest = '%2\\' + $e.Name;"
        "    [IO.Compression.ZipFileExtensions]::ExtractToFile($e,$dest,$true)"
        "  }"
        "};"
        "$zip.Dispose()")
                                  .arg(tempZip, targetDir);

    const int exRet = QProcess::execute(
        QStringLiteral("powershell"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"), psExtract});

    if (exRet != 0) {
      LWizardLog::warn(QStringLiteral("Failed to extract LSLib (exit code %1)").arg(exRet));
      return;
    }

    LWizardLog::info(QStringLiteral("LSLib downloaded and extracted to ") + targetDir);
  });

  QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  thread->start();
}

// ---------------------------------------------------------------------------
// IPluginTool interface
// ---------------------------------------------------------------------------

QString LWizardPlugin::displayName() const
{
  return tr("LWizard");
}

QString LWizardPlugin::tooltip() const
{
  return tr("BG3-focused management enhancements");
}

QIcon LWizardPlugin::icon() const
{
  return QIcon();
}

void LWizardPlugin::display() const
{
  auto* win = new LWizardWindow(m_organizer, m_localizationContent, parentWidget());
  win->exec();
}

// ---------------------------------------------------------------------------
// IPlugin interface
// ---------------------------------------------------------------------------

QString LWizardPlugin::name() const
{
  return QStringLiteral("lwizard");
}

QString LWizardPlugin::author() const
{
  return QStringLiteral("lwizard");
}

QString LWizardPlugin::description() const
{
  return tr("BG3-focused management enhancements for Mod Organizer 2.");
}

MOBase::VersionInfo LWizardPlugin::version() const
{
  return MOBase::VersionInfo(0, 2, 0, MOBase::VersionInfo::RELEASE_FINAL);
}

QList<MOBase::PluginSetting> LWizardPlugin::settings() const
{
  return {
      MOBase::PluginSetting(
          QStringLiteral("language"),
          tr("Localization language to scan for in BG3 mods"),
          QStringList{
              QStringLiteral("English"),
              QStringLiteral("French"),
              QStringLiteral("German"),
              QStringLiteral("Italian"),
              QStringLiteral("Spanish"),
              QStringLiteral("Polish"),
              QStringLiteral("Russian"),
              QStringLiteral("ChineseSimplified"),
              QStringLiteral("PortugueseBrazil"),
              QStringLiteral("Turkish"),
              QStringLiteral("Czech"),
              QStringLiteral("Ukrainian"),
              QStringLiteral("Korean"),
              QStringLiteral("Japanese"),
          })};
}
