#pragma once

#include <memory>

#include <QDialog>
#include <QPointer>

class BG3LocalizationContent;
class CheckableComboBox;
class LWizardModListUiPatch;
class LWizardNexusApi;
class NexusTab;
class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextEdit;
class TranslationTab;

namespace MOBase {
class IOrganizer;
}

/** Main lwizard tool window, opened from the MO2 Tools menu. */
class LWizardWindow : public QDialog
{
  Q_OBJECT

public:
  explicit LWizardWindow(MOBase::IOrganizer*                     organizer,
                         std::shared_ptr<BG3LocalizationContent> content,
                         LWizardNexusApi*                        nexusApi,
                         LWizardModListUiPatch*                  modListUiPatch,
                         QWidget*                                parent = nullptr);

private:
  MOBase::IOrganizer*                     m_organizer;
  std::shared_ptr<BG3LocalizationContent> m_content;

  QComboBox*             m_languageCombo         = nullptr;
  QCheckBox*             m_cacheOnlyCurrentLang  = nullptr;
  QCheckBox*             m_autoScanOnInstall     = nullptr;
  CheckableComboBox*     m_contentStatusesCombo  = nullptr;
  QCheckBox*             m_autoDownloadPatches   = nullptr;
  QPushButton*           m_scanBtn                  = nullptr;
  QPushButton*           m_clearCacheBtn            = nullptr;
  QLabel*                m_scanStatus               = nullptr;
  QProgressBar*          m_scanProgress             = nullptr;
  QPointer<QTextEdit>    m_logView;
  TranslationTab*        m_translationTab = nullptr;
  NexusTab*              m_nexusTab       = nullptr;
  LWizardNexusApi*       m_nexusApi       = nullptr;
  LWizardModListUiPatch* m_modListUiPatch = nullptr;
  class QTabWidget*      m_tabs           = nullptr;

  void setupUi();
  void buildSettingsTab(class QTabWidget* tabs);
  void buildTranslationTab(class QTabWidget* tabs);
  void buildNexusTab(class QTabWidget* tabs);
  void buildLogsTab(class QTabWidget* tabs);

  QString currentSavedLanguage() const;
  bool    savedBoolSetting(const QString& key, bool defaultValue) const;

private slots:
  void saveSettings();
  void onCacheOnlyCurrentLangToggled(bool checked);
  void startScan();
  void clearAllCaches();
  void onScanProgress(int done, int total, const QString& currentMod);
  void onScanFinished();
  void onLogEntry(const QString& entry);
};
