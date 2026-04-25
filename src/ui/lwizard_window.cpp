#include "ui/lwizard_window.h"

#include "core/bg3_localization_content.h"
#include "core/lwizard_log.h"
#include "ui/lwizard_modlist_ui_patch.h"
#include "ui/lwizard_nexus_tab.h"
#include "ui/lwizard_translation_tab.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>

#include <functional>

#include <uibase/imoinfo.h>

// ---------------------------------------------------------------------------
// Language list — must match lwizard_plugin.cpp settings() order
// ---------------------------------------------------------------------------
static const QStringList k_languages = {
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
};

static const QString kShowTranslationStatusKey()
{
  return QStringLiteral("show_translation_status");
}

static const QString kShowExtraContentStatusesKey()
{
  return QStringLiteral("show_extra_content_statuses");
}

static const QString kAutoDownloadPatchesKey()
{
  return QStringLiteral("auto_download_patches");
}

static const QString kAutoScanOnInstallKey()
{
  return QStringLiteral("auto_scan_on_install");
}

void configureSettingsForm(QFormLayout* form)
{
  form->setContentsMargins(10, 18, 10, 10);
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(10);
}

class CheckableComboBox : public QComboBox
{
public:
  explicit CheckableComboBox(QWidget* parent = nullptr) : QComboBox(parent)
  {
    auto* items = new QStandardItemModel(this);
    setModel(items);
    setEditable(true);
    if (lineEdit())
      lineEdit()->setReadOnly(true);
    setInsertPolicy(QComboBox::NoInsert);
    view()->viewport()->installEventFilter(this);
    connect(items, &QStandardItemModel::itemChanged, this, [this](QStandardItem*) {
      refreshSummaryText();
      if (!m_signalsBlocked && m_onSelectionChanged)
        m_onSelectionChanged();
    });
  }

  void addCheckItem(const QString& text, const QVariant& value, bool checked)
  {
    auto* item = new QStandardItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setData(value, Qt::UserRole);
    item->setData(checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
    static_cast<QStandardItemModel*>(model())->appendRow(item);
    refreshSummaryText();
  }

  bool isChecked(const QVariant& value) const
  {
    auto* items = static_cast<QStandardItemModel*>(model());
    for (int row = 0; row < items->rowCount(); ++row) {
      const QStandardItem* item = items->item(row);
      if (item && item->data(Qt::UserRole) == value)
        return item->checkState() == Qt::Checked;
    }
    return false;
  }

  void setChecked(const QVariant& value, bool checked)
  {
    auto* items = static_cast<QStandardItemModel*>(model());
    for (int row = 0; row < items->rowCount(); ++row) {
      QStandardItem* item = items->item(row);
      if (item && item->data(Qt::UserRole) == value) {
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        return;
      }
    }
  }

  void setSummaryTextProvider(std::function<QString(const CheckableComboBox&)> provider)
  {
    m_summaryTextProvider = std::move(provider);
    refreshSummaryText();
  }

  void setSelectionChangedHandler(std::function<void()> handler)
  {
    m_onSelectionChanged = std::move(handler);
  }

  void setSignalsBlocked(bool blocked)
  {
    m_signalsBlocked = blocked;
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (watched == view()->viewport() && event->type() == QEvent::MouseButtonRelease) {
      auto* mouseEvent = static_cast<QMouseEvent*>(event);
      const QModelIndex index = view()->indexAt(mouseEvent->pos());
      if (!index.isValid())
        return false;

      auto* item = static_cast<QStandardItemModel*>(model())->itemFromIndex(index);
      if (!item)
        return false;

      const bool checked = item->checkState() == Qt::Checked;
      item->setCheckState(checked ? Qt::Unchecked : Qt::Checked);
      return true;
    }

    return QComboBox::eventFilter(watched, event);
  }

private:
  void refreshSummaryText()
  {
    if (!lineEdit())
      return;

    if (m_summaryTextProvider) {
      lineEdit()->setText(m_summaryTextProvider(*this));
      return;
    }

    lineEdit()->setText(currentText());
  }

  std::function<QString(const CheckableComboBox&)> m_summaryTextProvider;
  std::function<void()>                            m_onSelectionChanged;
  bool                                             m_signalsBlocked = false;
};

enum class ContentStatusOption
{
  TranslationStatus,
  Bg3mmStatuses,
};

QString contentStatusSummary(const CheckableComboBox& combo)
{
  const bool translation = combo.isChecked(static_cast<int>(ContentStatusOption::TranslationStatus));
  const bool bg3mm = combo.isChecked(static_cast<int>(ContentStatusOption::Bg3mmStatuses));

  if (translation && bg3mm)
    return QCoreApplication::translate("LWizardWindow", "Both");
  if (translation)
    return QCoreApplication::translate("LWizardWindow", "Translation only");
  if (bg3mm)
    return QCoreApplication::translate("LWizardWindow", "BG3MM-style only");
  return QCoreApplication::translate("LWizardWindow", "None");
}

// ---------------------------------------------------------------------------

LWizardWindow::LWizardWindow(MOBase::IOrganizer*                     organizer,
                             std::shared_ptr<BG3LocalizationContent> content,
                             LWizardNexusApi*                        nexusApi,
                             LWizardModListUiPatch*                  modListUiPatch,
                             QWidget*                                parent)
    : QDialog(parent),
      m_organizer(organizer),
      m_content(std::move(content)),
      m_nexusApi(nexusApi),
      m_modListUiPatch(modListUiPatch)
{
  setWindowTitle(tr("LWizard"));
  setMinimumSize(900, 600);
  setAttribute(Qt::WA_DeleteOnClose);
  setupUi();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void LWizardWindow::setupUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(6);

  m_tabs = new QTabWidget(this);
  buildSettingsTab(m_tabs);
  buildTranslationTab(m_tabs);
  buildNexusTab(m_tabs);
  buildLogsTab(m_tabs);
  root->addWidget(m_tabs);

  // Close button at bottom
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  root->addWidget(buttons);
}

void LWizardWindow::buildSettingsTab(QTabWidget* tabs)
{
  auto* page   = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(14);

  auto* contentGroup = new QGroupBox(tr("Content column"), page);
  auto* contentForm  = new QFormLayout(contentGroup);
  configureSettingsForm(contentForm);

  m_contentStatusesCombo = new CheckableComboBox(contentGroup);
  m_contentStatusesCombo->addCheckItem(tr("Translation status"),
                                       static_cast<int>(ContentStatusOption::TranslationStatus),
                                       savedBoolSetting(kShowTranslationStatusKey(), true));
  m_contentStatusesCombo->addCheckItem(tr("BG3MM-style statuses"),
                                       static_cast<int>(ContentStatusOption::Bg3mmStatuses),
                                       savedBoolSetting(kShowExtraContentStatusesKey(), true));
  m_contentStatusesCombo->setSummaryTextProvider(contentStatusSummary);
  m_contentStatusesCombo->setToolTip(
      tr("Choose which LWizard icon groups appear in the MO2 Content column."));
  m_contentStatusesCombo->setSelectionChangedHandler([this]() {
    m_organizer->setPluginSetting(
        QStringLiteral("lwizard"),
        kShowTranslationStatusKey(),
        QVariant(m_contentStatusesCombo->isChecked(
            static_cast<int>(ContentStatusOption::TranslationStatus))));
    m_organizer->setPluginSetting(
        QStringLiteral("lwizard"),
        kShowExtraContentStatusesKey(),
        QVariant(m_contentStatusesCombo->isChecked(
            static_cast<int>(ContentStatusOption::Bg3mmStatuses))));
  });
  contentForm->addRow(tr("Visible statuses:"), m_contentStatusesCombo);

  layout->addWidget(contentGroup);

  auto* localizationGroup = new QGroupBox(tr("Localization scanning"), page);
  auto* localizationForm  = new QFormLayout(localizationGroup);
  configureSettingsForm(localizationForm);
  m_languageCombo = new QComboBox(localizationGroup);
  m_languageCombo->addItems(k_languages);

  {
    QSignalBlocker block(m_languageCombo);
    const QString  saved = currentSavedLanguage();
    const int      idx   = k_languages.indexOf(saved);
    m_languageCombo->setCurrentIndex(idx >= 0 ? idx : 0);
  }

  localizationForm->addRow(tr("Language to scan for:"), m_languageCombo);

  m_cacheOnlyCurrentLang = new QCheckBox(
      tr("Persist scan cache only for that language (removes other languages from disk "
         "when the cache is saved or when you enable this)."),
      localizationGroup);
  {
    QSignalBlocker block(m_cacheOnlyCurrentLang);
    const QVariant cacheOnly = m_organizer->pluginSetting(
        QStringLiteral("lwizard"), QStringLiteral("cache_only_current_language"));
    m_cacheOnlyCurrentLang->setChecked(cacheOnly.isValid() ? cacheOnly.toBool() : false);
  }
  localizationForm->addRow(tr("Disk cache:"), m_cacheOnlyCurrentLang);

  m_autoScanOnInstall =
      new QCheckBox(tr("Automatically scan newly installed mods"), localizationGroup);
  {
    QSignalBlocker block(m_autoScanOnInstall);
    m_autoScanOnInstall->setChecked(savedBoolSetting(kAutoScanOnInstallKey(), true));
  }
  localizationForm->addRow(tr("New installs:"), m_autoScanOnInstall);

  layout->addWidget(localizationGroup);

  auto* patchGroup = new QGroupBox(tr("Patches"), page);
  auto* patchForm  = new QFormLayout(patchGroup);
  configureSettingsForm(patchForm);
  m_autoDownloadPatches =
      new QCheckBox(tr("Automatically look for and download patches (placeholder)"), patchGroup);
  {
    QSignalBlocker block(m_autoDownloadPatches);
    m_autoDownloadPatches->setChecked(savedBoolSetting(kAutoDownloadPatchesKey(), false));
  }
  patchForm->addRow(tr("Patch automation:"), m_autoDownloadPatches);
  layout->addWidget(patchGroup);

  connect(m_languageCombo, &QComboBox::currentIndexChanged, this, &LWizardWindow::saveSettings);
  connect(m_cacheOnlyCurrentLang,
          &QCheckBox::toggled,
          this,
          &LWizardWindow::onCacheOnlyCurrentLangToggled);
  connect(m_autoScanOnInstall, &QCheckBox::toggled, this, [this](bool checked) {
    m_organizer->setPluginSetting(
        QStringLiteral("lwizard"), kAutoScanOnInstallKey(), QVariant(checked));
  });
  connect(m_autoDownloadPatches, &QCheckBox::toggled, this, [this](bool checked) {
    m_organizer->setPluginSetting(
        QStringLiteral("lwizard"), kAutoDownloadPatchesKey(), QVariant(checked));
  });

  auto* maintenanceGroup  = new QGroupBox(tr("Maintenance"), page);
  auto* maintenanceLayout = new QVBoxLayout(maintenanceGroup);
  maintenanceLayout->setSpacing(8);

  m_scanBtn       = new QPushButton(tr("Scan mods"), maintenanceGroup);
  m_clearCacheBtn = new QPushButton(tr("Clear all caches"), maintenanceGroup);
  m_scanStatus    = new QLabel(tr("Idle"), maintenanceGroup);
  m_scanStatus->setVisible(false);
  m_scanProgress = new QProgressBar(maintenanceGroup);
  m_scanProgress->setVisible(false);
  m_scanProgress->setRange(0, 100);

  auto* btnRow = new QHBoxLayout;
  btnRow->addStretch();
  btnRow->addWidget(m_scanBtn);
  btnRow->addWidget(m_clearCacheBtn);
  maintenanceLayout->addLayout(btnRow);
  maintenanceLayout->addWidget(m_scanStatus);
  maintenanceLayout->addWidget(m_scanProgress);
  layout->addWidget(maintenanceGroup);
  layout->addStretch();

  connect(m_scanBtn, &QPushButton::clicked, this, &LWizardWindow::startScan);
  connect(m_clearCacheBtn, &QPushButton::clicked, this, &LWizardWindow::clearAllCaches);
  connect(
      m_content.get(), &BG3LocalizationContent::scanProgress, this, &LWizardWindow::onScanProgress);
  connect(
      m_content.get(), &BG3LocalizationContent::scanFinished, this, &LWizardWindow::onScanFinished);

  tabs->addTab(page, tr("Settings"));
}

void LWizardWindow::buildTranslationTab(QTabWidget* tabs)
{
  m_translationTab = new TranslationTab(m_organizer, m_content, tabs);
  tabs->addTab(m_translationTab, tr("Translation"));
}

void LWizardWindow::buildNexusTab(QTabWidget* tabs)
{
  m_nexusTab = new NexusTab(m_organizer, m_content, m_nexusApi, tabs);
  tabs->addTab(m_nexusTab, tr("Nexus Downloads"));
}

void LWizardWindow::buildLogsTab(QTabWidget* tabs)
{
  auto* page   = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);

  m_logView = new QTextEdit(page);
  m_logView->setReadOnly(true);
  m_logView->setLineWrapMode(QTextEdit::NoWrap);
  {
    QFont mono(QStringLiteral("Consolas"), 9);
    mono.setStyleHint(QFont::Monospace);
    m_logView->setFont(mono);
  }

  // Populate existing entries
  for (const QString& entry : LWizardLog::instance().entries())
    m_logView->append(entry);

  // Scroll to bottom
  auto* sb = m_logView->verticalScrollBar();
  if (sb)
    sb->setValue(sb->maximum());

  layout->addWidget(m_logView);

  // Toolbar row: Clear button
  auto* clearBtn = new QPushButton(tr("Clear"), page);
  clearBtn->setMaximumWidth(80);
  auto* btnRow = new QHBoxLayout;
  btnRow->addStretch();
  btnRow->addWidget(clearBtn);
  layout->addLayout(btnRow);

  connect(clearBtn, &QPushButton::clicked, this, [this]() {
    LWizardLog::instance().clear();
    m_logView->clear();
  });

  // Live updates
  connect(&LWizardLog::instance(), &LWizardLog::entryAdded, this, &LWizardWindow::onLogEntry);

  tabs->addTab(page, tr("Logs"));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString LWizardWindow::currentSavedLanguage() const
{
  QVariant v = m_organizer->pluginSetting(QStringLiteral("lwizard"), QStringLiteral("language"));
  if (!v.isValid())
    return QStringLiteral("English");
  if (v.typeId() == QMetaType::QStringList) {
    const QStringList list = v.toStringList();
    return list.isEmpty() ? QStringLiteral("English") : list.first();
  }
  const QString s = v.toString();
  return s.isEmpty() ? QStringLiteral("English") : s;
}

bool LWizardWindow::savedBoolSetting(const QString& key, bool defaultValue) const
{
  const QVariant value = m_organizer->pluginSetting(QStringLiteral("lwizard"), key);
  return value.isValid() ? value.toBool() : defaultValue;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void LWizardWindow::saveSettings()
{
  const QString lang = m_languageCombo->currentText();
  m_organizer->setPluginSetting(QStringLiteral("lwizard"), QStringLiteral("language"), lang);
  LWizardLog::info(QStringLiteral("Language set to: ") + lang);
}

void LWizardWindow::onCacheOnlyCurrentLangToggled(bool checked)
{
  m_organizer->setPluginSetting(
      QStringLiteral("lwizard"), QStringLiteral("cache_only_current_language"), QVariant(checked));
}

void LWizardWindow::startScan()
{
  if (!m_content->scanAll()) {
    LWizardLog::warn(QStringLiteral("Scan already in progress"));
    return;
  }
  m_scanBtn->setEnabled(false);
  if (m_clearCacheBtn)
    m_clearCacheBtn->setEnabled(false);
  if (m_scanProgress) {
    m_scanProgress->setValue(0);
    m_scanProgress->setVisible(true);
  }
  if (m_scanStatus) {
    m_scanStatus->setText(tr("Scanning mods..."));
    m_scanStatus->setVisible(true);
  }
  m_scanBtn->setText(tr("Scanning…"));
  // Switch to the Logs tab so the user can see progress
  if (m_tabs)
    m_tabs->setCurrentIndex(3); // Logs tab
}

void LWizardWindow::clearAllCaches()
{
  const int answer = QMessageBox::warning(
      this,
      tr("Clear LWizard caches"),
      tr("Clear all LWizard scan caches? This removes Content-column scan results and "
         "embedded string caches. Saved translations and Nexus settings are kept."),
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);
  if (answer != QMessageBox::Yes)
    return;

  if (!m_content->clearAllCaches()) {
    QMessageBox::information(this,
                             tr("Clear LWizard caches"),
                             tr("A scan is running. Wait for it to finish, then clear caches."));
    return;
  }

  if (m_scanStatus) {
    m_scanStatus->setText(tr("Caches cleared."));
    m_scanStatus->setVisible(true);
  }
}

void LWizardWindow::onScanProgress(int done, int total, const QString& currentMod)
{
  if (m_scanProgress) {
    m_scanProgress->setMaximum(total > 0 ? total : 1);
    m_scanProgress->setValue(done);
  }
  if (m_scanStatus)
    m_scanStatus->setText(tr("Scanning %1 / %2: %3").arg(done).arg(total).arg(currentMod));
}

void LWizardWindow::onScanFinished()
{
  m_scanBtn->setEnabled(true);
  if (m_clearCacheBtn)
    m_clearCacheBtn->setEnabled(true);
  m_scanBtn->setText(tr("Scan mods"));
  if (m_scanStatus)
    m_scanStatus->setText(tr("Scan complete."));
  if (m_scanProgress)
    m_scanProgress->setVisible(false);
}

void LWizardWindow::onLogEntry(const QString& entry)
{
  if (!m_logView)
    return;
  m_logView->append(entry);
  auto* sb = m_logView->verticalScrollBar();
  if (sb)
    sb->setValue(sb->maximum());
}
