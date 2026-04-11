#include "lwizard_window.h"
#include "bg3_localization_content.h"
#include "lwizard_log.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>

#include <uibase/imoinfo.h>

// ---------------------------------------------------------------------------
// Language list — must match lwizard_plugin.cpp settings() order
// ---------------------------------------------------------------------------
static const QStringList k_languages = {
    QStringLiteral("English"),          QStringLiteral("French"),
    QStringLiteral("German"),           QStringLiteral("Italian"),
    QStringLiteral("Spanish"),          QStringLiteral("Polish"),
    QStringLiteral("Russian"),          QStringLiteral("ChineseSimplified"),
    QStringLiteral("PortugueseBrazil"), QStringLiteral("Turkish"),
    QStringLiteral("Czech"),            QStringLiteral("Ukrainian"),
    QStringLiteral("Korean"),           QStringLiteral("Japanese"),
};

// ---------------------------------------------------------------------------

LWizardWindow::LWizardWindow(MOBase::IOrganizer* organizer,
                             std::shared_ptr<BG3LocalizationContent> content,
                             QWidget* parent)
    : QDialog(parent), m_organizer(organizer), m_content(std::move(content))
{
  setWindowTitle(tr("LWizard"));
  setMinimumSize(520, 400);
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
  buildLogsTab(m_tabs);
  root->addWidget(m_tabs);

  // Close button at bottom
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  root->addWidget(buttons);
}

void LWizardWindow::buildSettingsTab(QTabWidget* tabs)
{
  auto* page   = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);

  // Language group
  auto* grp     = new QGroupBox(tr("Localization scanning"), page);
  auto* form    = new QFormLayout(grp);
  m_languageCombo = new QComboBox(grp);
  m_languageCombo->addItems(k_languages);

  {
    QSignalBlocker block(m_languageCombo);
    const QString saved = currentSavedLanguage();
    const int idx       = k_languages.indexOf(saved);
    m_languageCombo->setCurrentIndex(idx >= 0 ? idx : 0);
  }

  form->addRow(tr("Language to scan for:"), m_languageCombo);

  m_cacheOnlyCurrentLang = new QCheckBox(
      tr("Persist scan cache only for that language (removes other languages from disk "
         "when the cache is saved or when you enable this)."),
      grp);
  {
    QSignalBlocker block(m_cacheOnlyCurrentLang);
    const QVariant cacheOnly =
        m_organizer->pluginSetting(QStringLiteral("lwizard"),
                                   QStringLiteral("cache_only_current_language"));
    m_cacheOnlyCurrentLang->setChecked(cacheOnly.isValid() ? cacheOnly.toBool() : false);
  }
  form->addRow(tr("Disk cache:"), m_cacheOnlyCurrentLang);

  layout->addWidget(grp);

  connect(m_languageCombo, &QComboBox::currentIndexChanged, this,
          &LWizardWindow::saveSettings);
  connect(m_cacheOnlyCurrentLang, &QCheckBox::toggled, this,
          &LWizardWindow::onCacheOnlyCurrentLangToggled);

  // Scan button row
  m_scanBtn = new QPushButton(tr("Scan mods"), page);

  auto* btnRow = new QHBoxLayout;
  btnRow->addStretch();
  btnRow->addWidget(m_scanBtn);
  layout->addLayout(btnRow);
  layout->addStretch();

  connect(m_scanBtn, &QPushButton::clicked, this, &LWizardWindow::startScan);
  connect(m_content.get(), &BG3LocalizationContent::scanFinished,
          this, &LWizardWindow::onScanFinished);

  tabs->addTab(page, tr("Settings"));
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
  connect(&LWizardLog::instance(), &LWizardLog::entryAdded, this,
          &LWizardWindow::onLogEntry);

  tabs->addTab(page, tr("Logs"));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString LWizardWindow::currentSavedLanguage() const
{
  QVariant v = m_organizer->pluginSetting(QStringLiteral("lwizard"),
                                          QStringLiteral("language"));
  if (!v.isValid())
    return QStringLiteral("English");
  if (v.typeId() == QMetaType::QStringList) {
    const QStringList list = v.toStringList();
    return list.isEmpty() ? QStringLiteral("English") : list.first();
  }
  const QString s = v.toString();
  return s.isEmpty() ? QStringLiteral("English") : s;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void LWizardWindow::saveSettings()
{
  const QString lang = m_languageCombo->currentText();
  m_organizer->setPluginSetting(QStringLiteral("lwizard"), QStringLiteral("language"),
                                lang);
  LWizardLog::info(QStringLiteral("Language set to: ") + lang);
}

void LWizardWindow::onCacheOnlyCurrentLangToggled(bool checked)
{
  m_organizer->setPluginSetting(QStringLiteral("lwizard"),
                                QStringLiteral("cache_only_current_language"),
                                QVariant(checked));
}

void LWizardWindow::startScan()
{
  if (!m_content->scanAll()) {
    LWizardLog::warn(QStringLiteral("Scan already in progress"));
    return;
  }
  m_scanBtn->setEnabled(false);
  m_scanBtn->setText(tr("Scanning…"));
  // Switch to the Logs tab so the user can see progress
  if (m_tabs)
    m_tabs->setCurrentIndex(1);
}

void LWizardWindow::onScanFinished()
{
  m_scanBtn->setEnabled(true);
  m_scanBtn->setText(tr("Scan mods"));
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
