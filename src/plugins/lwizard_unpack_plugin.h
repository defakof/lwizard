#pragma once

#include <uibase/iplugintool.h>
#include <uibase/pluginsetting.h>
#include <uibase/versioninfo.h>

class LWizardUnpackPlugin : public MOBase::IPluginTool
{
  Q_OBJECT
  Q_INTERFACES(MOBase::IPlugin MOBase::IPluginTool)
  Q_PLUGIN_METADATA(IID "org.modorganizer.lwizard.unpack")

public:
  LWizardUnpackPlugin() = default;

  bool                         init(MOBase::IOrganizer* organizer) override;
  QString                      name() const override;
  QString                      author() const override;
  QString                      description() const override;
  MOBase::VersionInfo          version() const override;
  QList<MOBase::PluginSetting> settings() const override;

  QString displayName() const override;
  QString tooltip() const override;
  QIcon   icon() const override;

public Q_SLOTS:
  void display() const override;

private:
  MOBase::IOrganizer* m_organizer = nullptr;
};
