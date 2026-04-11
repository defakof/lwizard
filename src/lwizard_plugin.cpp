#include "lwizard_plugin.h"
#include "bg3_localization_content.h"
#include "lwizard_divine.h"
#include "lwizard_log.h"
#include "lwizard_modlist_ui_patch.h"
#include "lwizard_window.h"

#include <QMainWindow>

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
                   [this]() {
                     m_organizer->refresh(false);
                     if (m_modListUiPatch)
                       m_modListUiPatch->refreshFromSelection();
                   });

  // Register ModDataContent after the UI is up (same pattern as mo2-bg3-translation-
  // checker’s plugin_content.py). Early init() registration can leave the Content
  // column disabled in the header menu until modDataContents() is populated.

  // Drop cache entries for removed mods; disk cache survives restarts (invalidated by
  // per-mod fingerprint when files change).
  organizer->onNextRefresh(
      [this]() { m_localizationContent->pruneMissingModsFromCache(); },
      /*immediateIfPossible=*/false);

  // Reload cache when language or single-language disk policy changes.
  organizer->onPluginSettingChanged(
      [this](const QString& plugin, const QString& key, const QVariant&,
             const QVariant& newValue) {
        if (plugin != name())
          return;
        if (key == QStringLiteral("language")) {
          {
            const QVariant v = m_organizer->pluginSetting(
                name(), QStringLiteral("cache_only_current_language"));
            if (v.isValid() && v.toBool())
              m_localizationContent->prunePersistentCacheToCurrentLanguageOnly();
          }
          m_localizationContent->hydrateMemoryFromPersistent();
          m_organizer->refresh(false);
          if (m_modListUiPatch)
            m_modListUiPatch->refreshFromSelection();
        } else if (key == QStringLiteral("cache_only_current_language")) {
          if (newValue.toBool())
            m_localizationContent->prunePersistentCacheToCurrentLanguageOnly();
          m_localizationContent->hydrateMemoryFromPersistent();
          m_organizer->refresh(false);
          if (m_modListUiPatch)
            m_modListUiPatch->refreshFromSelection();
        }
      });

  organizer->onUserInterfaceInitialized([this](QMainWindow* mainWindow) {
    registerLocalizationContentFeature();
    m_localizationContent->hydrateMemoryFromPersistent();
    if (!m_modListUiPatch) {
      m_modListUiPatch =
          std::make_unique<LWizardModListUiPatch>(m_organizer, m_localizationContent);
    }
    m_modListUiPatch->attach(mainWindow);
    m_organizer->refresh(false);
    m_modListUiPatch->refreshFromSelection();
    LWizardDivine::ensureDownloadedIfMissing(m_organizer);
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
// IPluginTool interface
// ---------------------------------------------------------------------------

QString LWizardPlugin::displayName() const
{
  return tr("LWizard/Menu");
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
  return MOBase::VersionInfo(0, 2, 7, MOBase::VersionInfo::RELEASE_FINAL);
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
          }),
      MOBase::PluginSetting(
          QStringLiteral("cache_only_current_language"),
          tr("Persist scan cache only for the selected language (other languages are "
             "removed from disk when the cache is saved or when this is enabled)."),
          QVariant(false)),
  };
}
