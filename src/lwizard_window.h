#pragma once

#include <memory>

#include <QDialog>

class BG3LocalizationContent;
class QComboBox;
class QPushButton;
class QTextEdit;

namespace MOBase {
class IOrganizer;
}

/**
 * Main lwizard tool window, opened from the MO2 Tools menu.
 * Tabs:
 *  - Settings : language selector + scan button
 *  - Logs     : live plugin log output
 */
class LWizardWindow : public QDialog
{
  Q_OBJECT

public:
  explicit LWizardWindow(MOBase::IOrganizer* organizer,
                         std::shared_ptr<BG3LocalizationContent> content,
                         QWidget* parent = nullptr);

private:
  MOBase::IOrganizer* m_organizer;
  std::shared_ptr<BG3LocalizationContent> m_content;

  QComboBox*   m_languageCombo = nullptr;
  QPushButton* m_scanBtn       = nullptr;
  QTextEdit*   m_logView       = nullptr;
  class QTabWidget* m_tabs     = nullptr;

  void setupUi();
  void buildSettingsTab(class QTabWidget* tabs);
  void buildLogsTab(class QTabWidget* tabs);

  QString currentSavedLanguage() const;

private slots:
  void saveSettings();
  void startScan();
  void onScanFinished();
  void onLogEntry(const QString& entry);
};
