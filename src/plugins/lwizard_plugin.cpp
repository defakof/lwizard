#include "plugins/lwizard_plugin.h"

#include "core/bg3_localization_content.h"
#include "core/lwizard_divine.h"
#include "core/lwizard_log.h"
#include "services/lwizard_nexus_api.h"
#include "ui/lwizard_modlist_ui_patch.h"
#include "ui/lwizard_window.h"

#include <QMainWindow>

#include <map>

#include <uibase/game_features/igamefeatures.h>
#include <uibase/iplugingame.h>
#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool LWizardPlugin::init(MOBase::IOrganizer* organizer)
{
  m_organizer = organizer;

  m_localizationContent = std::make_shared<BG3LocalizationContent>(organizer);

  // Nexus API — persistent, outlives any window instance.
  m_nexusApi = new LWizardNexusApi(this);
  // Connect results back to content column (updates CONTENT_UNAVAILABLE → CONTENT_AVAILABLE)
  QObject::connect(m_nexusApi,
                   &LWizardNexusApi::translationsReady,
                   [this](const QString& modName, const QList<NexusTranslationFile>& files) {
                     if (files.isEmpty())
                       return;
                     QList<int> modIds;
                     for (const auto& f : files)
                       if (!modIds.contains(f.modId))
                         modIds.append(f.modId);
                     m_localizationContent->markNexusAvailable(modName, modIds);
                   });

  // After any cache update, trigger a soft MO2 refresh so the Content
  // column re-queries getContentsFor() and picks up the newly cached results.
  QObject::connect(
      m_localizationContent.get(), &BG3LocalizationContent::contentCacheUpdated, [this]() {
        if (m_modListUiPatch) {
          m_modListUiPatch->refreshOrganizerPreservingState();
        } else {
          m_organizer->refresh(false);
        }
      });

  organizer->modList()->onModInstalled([this](MOBase::IModInterface* mod) {
    if (!mod || !m_localizationContent)
      return;

    const QString modName = mod->name().trimmed();
    if (modName.isEmpty())
      return;

    const QVariant autoScanSetting =
        m_organizer->pluginSetting(name(), QStringLiteral("auto_scan_on_install"));
    if (autoScanSetting.isValid() && !autoScanSetting.toBool()) {
      LWizardLog::info(
          QStringLiteral("Automatic scan skipped for newly installed mod: %1").arg(modName));
      return;
    }

    LWizardLog::info(QStringLiteral("Detected newly installed mod: %1").arg(modName));

    // 1. Local PAK/loca scan (existing)
    m_localizationContent->scanModAsync(modName);

    // 2. Nexus translation availability scan (new)
    //    Only if the mod has a Nexus ID — no-op otherwise.
    const int nexusId = mod->nexusId();
    if (nexusId > 0 && m_nexusApi) {
      // Resolve target language the same way as the rest of the plugin
      QVariant v = m_organizer->pluginSetting(name(), QStringLiteral("language"));
      QString  lang;
      if (v.typeId() == QMetaType::QStringList) {
        const QStringList l = v.toStringList();
        lang                = l.isEmpty() ? QStringLiteral("Russian") : l.first();
      } else {
        lang = v.toString();
        if (lang.isEmpty())
          lang = QStringLiteral("Russian");
      }
      m_nexusApi->scanModAsync(modName, nexusId, lang);
    }
  });

  organizer->modList()->onModStateChanged(
      [this](const std::map<QString, MOBase::IModList::ModStates>&) {
        m_localizationContent->invalidateDerivedCaches();
        if (m_modListUiPatch) {
          m_modListUiPatch->refreshOrganizerPreservingState();
        } else {
          m_organizer->refresh(false);
        }
      });

  // Register ModDataContent after the UI is up (same pattern as mo2-bg3-translation-
  // checker’s plugin_content.py). Early init() registration can leave the Content
  // column disabled in the header menu until modDataContents() is populated.

  // Drop cache entries for removed mods; disk cache survives restarts (invalidated by
  // per-mod fingerprint when files change).
  organizer->onNextRefresh(
      [this]() {
        m_localizationContent->pruneMissingModsFromCache();
      },
      /*immediateIfPossible=*/false);

  // Reload cache when language or single-language disk policy changes.
  organizer->onPluginSettingChanged(
      [this](const QString& plugin, const QString& key, const QVariant&, const QVariant& newValue) {
        if (plugin != name())
          return;
        if (key == QStringLiteral("language")) {
          {
            const QVariant v =
                m_organizer->pluginSetting(name(), QStringLiteral("cache_only_current_language"));
            if (v.isValid() && v.toBool())
              m_localizationContent->prunePersistentCacheToCurrentLanguageOnly();
          }
          m_localizationContent->hydrateMemoryFromPersistent();
          if (m_modListUiPatch) {
            m_modListUiPatch->refreshOrganizerPreservingState();
          } else {
            m_organizer->refresh(false);
          }
        } else if (key == QStringLiteral("cache_only_current_language")) {
          if (newValue.toBool())
            m_localizationContent->prunePersistentCacheToCurrentLanguageOnly();
          m_localizationContent->hydrateMemoryFromPersistent();
          if (m_modListUiPatch) {
            m_modListUiPatch->refreshOrganizerPreservingState();
          } else {
            m_organizer->refresh(false);
          }
        } else if (key == QStringLiteral("show_translation_status") ||
                   key == QStringLiteral("show_extra_content_statuses")) {
          if (m_modListUiPatch) {
            m_modListUiPatch->refreshContentColumn();
          } else {
            m_organizer->refresh(false);
          }
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
  auto* game = const_cast<MOBase::IPluginGame*>(m_organizer->managedGame());
  bool  ok   = false;
  if (game != nullptr) {
    ok = features->registerFeature(game,
                                   m_localizationContent,
                                   /*priority=*/10,
                                   /*replace=*/true);
  }
  if (!ok) {
    ok = features->registerFeature(QStringList{QStringLiteral("baldursgate3")},
                                   m_localizationContent,
                                   /*priority=*/10,
                                   /*replace=*/true);
  }
  m_contentFeatureRegistered = ok;
}

// ---------------------------------------------------------------------------
// IPluginTool interface
// ---------------------------------------------------------------------------

QString LWizardPlugin::displayName() const
{
  return tr("LWizard/Settings");
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
  auto* win = new LWizardWindow(
      m_organizer, m_localizationContent, m_nexusApi, m_modListUiPatch.get(), parentWidget());
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
  return MOBase::VersionInfo(0, 2, 10, MOBase::VersionInfo::RELEASE_FINAL);
}

QList<MOBase::PluginSetting> LWizardPlugin::settings() const
{
  return {
      MOBase::PluginSetting(QStringLiteral("show_translation_status"),
                            tr("Show LWizard translation status icons in the MO2 Content column"),
                            QVariant(true)),
      MOBase::PluginSetting(QStringLiteral("show_extra_content_statuses"),
                            tr("Show BG3 Mod Manager-style metadata and Script Extender status "
                               "icons in the MO2 Content column"),
                            QVariant(true)),
      MOBase::PluginSetting(QStringLiteral("auto_download_patches"),
                            tr("Automatically look for and download patches (placeholder)."),
                            QVariant(false)),
      MOBase::PluginSetting(QStringLiteral("auto_scan_on_install"),
                            tr("Automatically scan newly installed mods"),
                            QVariant(true)),
      MOBase::PluginSetting(QStringLiteral("language"),
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
