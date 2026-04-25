#include "plugins/lwizard_unpack_plugin.h"

#include "core/lwizard_divine.h"
#include "ui/lwizard_unpack_dialog.h"

#include <QMainWindow>

#include <uibase/imoinfo.h>

bool LWizardUnpackPlugin::init(MOBase::IOrganizer* organizer)
{
  m_organizer = organizer;
  organizer->onUserInterfaceInitialized([this](QMainWindow*) {
    LWizardDivine::ensureDownloadedIfMissing(m_organizer);
  });
  return true;
}

QString LWizardUnpackPlugin::displayName() const
{
  return tr("LWizard/Utilities/Unpack mod");
}

QString LWizardUnpackPlugin::tooltip() const
{
  return tr("Extract every .pak in a mod next to each archive (Divine / LSLib).");
}

QIcon LWizardUnpackPlugin::icon() const
{
  return QIcon();
}

void LWizardUnpackPlugin::display() const
{
  auto* dlg = new LWizardUnpackDialog(m_organizer, parentWidget());
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->exec();
}

QString LWizardUnpackPlugin::name() const
{
  return QStringLiteral("lwizard_unpack");
}

QString LWizardUnpackPlugin::author() const
{
  return QStringLiteral("lwizard");
}

QString LWizardUnpackPlugin::description() const
{
  return tr("Unpack BG3 .pak files for the selected mod using Divine (LSLib).");
}

MOBase::VersionInfo LWizardUnpackPlugin::version() const
{
  return MOBase::VersionInfo(0, 2, 1, MOBase::VersionInfo::RELEASE_FINAL);
}

QList<MOBase::PluginSetting> LWizardUnpackPlugin::settings() const
{
  return {};
}
