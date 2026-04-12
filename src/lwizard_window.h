#pragma once

#include <memory>

#include <QDialog>
#include <QPointer>

class BG3LocalizationContent;
class QCheckBox;
class QComboBox;
class QPushButton;
class QTextEdit;
class TranslationTab;

namespace MOBase {
class IOrganizer;
}

/**
 * Main lwizard tool window, opened from the MO2 Tools menu.
 * Tabs:
 *  - Settings    : language selector (auto-saved) + scan button
 *  - Translation : mod string translator + pak exporter
 *  - Logs        : live plugin log output
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

  QComboBox*      m_languageCombo         = nullptr;
  QCheckBox*      m_cacheOnlyCurrentLang  = nullptr;
  QPushButton*    m_scanBtn               = nullptr;
  QPointer<QTextEdit> m_logView;
  TranslationTab* m_translationTab        = nullptr;
  class QTabWidget* m_tabs                = nullptr;

  void setupUi();
  void buildSettingsTab(class QTabWidget* tabs);
  void buildTranslationTab(class QTabWidget* tabs);
  void buildLogsTab(class QTabWidget* tabs);

  QString currentSavedLanguage() const;

private slots:
  void saveSettings();
  void onCacheOnlyCurrentLangToggled(bool checked);
  void startScan();
  void onScanFinished();
  void onLogEntry(const QString& entry);
};
