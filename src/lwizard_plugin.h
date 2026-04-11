#pragma once

#include <memory>

#include <uibase/iplugintool.h>
#include <uibase/pluginsetting.h>
#include <uibase/versioninfo.h>

class BG3LocalizationContent;
class LWizardModListUiPatch;

class LWizardPlugin : public MOBase::IPluginTool
{
  Q_OBJECT
  Q_INTERFACES(MOBase::IPlugin MOBase::IPluginTool)
  Q_PLUGIN_METADATA(IID "org.modorganizer.lwizard")

public:
  LWizardPlugin() = default;

  bool init(MOBase::IOrganizer* organizer) override;
  QString name() const override;
  QString author() const override;
  QString description() const override;
  MOBase::VersionInfo version() const override;
  QList<MOBase::PluginSetting> settings() const override;

  // IPluginTool
  QString displayName() const override;
  QString tooltip() const override;
  QIcon icon() const override;

public Q_SLOTS:
  void display() const override;

private:
  MOBase::IOrganizer* m_organizer = nullptr;
  std::shared_ptr<BG3LocalizationContent> m_localizationContent;
  std::unique_ptr<LWizardModListUiPatch> m_modListUiPatch;
  bool m_contentFeatureRegistered = false;

  void registerLocalizationContentFeature();
};
