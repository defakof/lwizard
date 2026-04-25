#include "ui/lwizard_translation_tab.h"

#include "core/bg3_localization_content.h"
#include "core/lwizard_divine.h"
#include "core/lwizard_log.h"
#include "services/lwizard_ai_translator.h"
#include "ui/lwizard_html_delegate.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QPointer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>
#include <QXmlStreamWriter>

#include <uibase/imodinterface.h>
#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TranslationTab::TranslationTab(MOBase::IOrganizer*                     organizer,
                               std::shared_ptr<BG3LocalizationContent> content,
                               QWidget*                                parent)
    : QWidget(parent), m_organizer(organizer), m_content(std::move(content))
{
  m_aiTranslator = new LWizardAiTranslator(this);
  m_aiTranslator->setApiKey(loadApiKey()); // load persisted key immediately
  connect(m_aiTranslator, &LWizardAiTranslator::batchDone, this, &TranslationTab::onAiBatchDone);
  connect(m_aiTranslator, &LWizardAiTranslator::progress, this, &TranslationTab::onAiProgress);
  connect(m_aiTranslator, &LWizardAiTranslator::finished, this, &TranslationTab::onAiFinished);
  connect(m_aiTranslator, &LWizardAiTranslator::error, this, &TranslationTab::onAiError);

  setupUi();
  populateModList();
}

TranslationTab::~TranslationTab()
{
  // Stop AI — cancels any pending QTimer retries and disconnects reply signals
  if (m_aiTranslator)
    m_aiTranslator->cancel();

  // The load thread has no event loop (QThread::create), so quit() is a no-op.
  // We wait briefly so it can finish naturally; the QPointer guard in the
  // invokeMethod lambda makes it safe even if the thread outlives us.
  if (m_loadThread && m_loadThread->isRunning())
    m_loadThread->wait(500);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void TranslationTab::setupUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(6);

  // ── Top area: mod picker (left) + language controls (right) ──────────────
  auto* topSplit = new QSplitter(Qt::Horizontal, this);
  topSplit->setChildrenCollapsible(false);

  // Left: mod list
  auto* modPanel  = new QWidget(topSplit);
  auto* modLayout = new QVBoxLayout(modPanel);
  modLayout->setContentsMargins(0, 0, 0, 0);
  modLayout->setSpacing(4);

  auto* modLabel = new QLabel(tr("Source mod:"), modPanel);
  modLayout->addWidget(modLabel);

  m_modSearch = new QLineEdit(modPanel);
  m_modSearch->setPlaceholderText(tr("Search mods…"));
  m_modSearch->setClearButtonEnabled(true);
  modLayout->addWidget(m_modSearch);

  m_modList = new QListWidget(modPanel);
  m_modList->setAlternatingRowColors(true);
  modLayout->addWidget(m_modList);

  topSplit->addWidget(modPanel);

  // Right: language selectors + load button
  auto* langPanel  = new QWidget(topSplit);
  auto* langLayout = new QVBoxLayout(langPanel);
  langLayout->setContentsMargins(0, 0, 0, 0);
  langLayout->setSpacing(6);

  auto* srcRow = new QHBoxLayout;
  srcRow->addWidget(new QLabel(tr("Source language:"), langPanel));
  m_srcLangCombo = new QComboBox(langPanel);
  m_srcLangCombo->addItems(k_languages);
  m_srcLangCombo->setCurrentIndex(0); // English
  srcRow->addWidget(m_srcLangCombo);
  langLayout->addLayout(srcRow);

  auto* dstRow = new QHBoxLayout;
  dstRow->addWidget(new QLabel(tr("Target language:"), langPanel));
  m_dstLangCombo = new QComboBox(langPanel);
  m_dstLangCombo->addItems(k_languages);
  // Default to the plugin "scan for" language
  {
    QVariant v = m_organizer->pluginSetting(QStringLiteral("lwizard"), QStringLiteral("language"));
    QString  saved;
    if (v.typeId() == QMetaType::QStringList) {
      const QStringList l = v.toStringList();
      saved               = l.isEmpty() ? QStringLiteral("Russian") : l.first();
    } else {
      saved = v.toString();
    }
    const int idx = k_languages.indexOf(saved);
    m_dstLangCombo->setCurrentIndex(idx >= 0 ? idx : 6); // fallback Russian
  }
  dstRow->addWidget(m_dstLangCombo);
  langLayout->addLayout(dstRow);

  m_loadBtn = new QPushButton(tr("Load Strings"), langPanel);
  m_loadBtn->setEnabled(false);
  langLayout->addWidget(m_loadBtn);

  m_statusLabel = new QLabel(langPanel);
  m_statusLabel->setWordWrap(true);
  langLayout->addWidget(m_statusLabel);

  langLayout->addStretch();
  topSplit->addWidget(langPanel);
  topSplit->setStretchFactor(0, 2);
  topSplit->setStretchFactor(1, 1);

  root->addWidget(topSplit, 1);

  // ── String filter + action buttons ───────────────────────────────────────
  auto* ctrlRow = new QHBoxLayout;

  auto* filterLabel = new QLabel(tr("Filter strings:"), this);
  ctrlRow->addWidget(filterLabel);

  m_strSearch = new QLineEdit(this);
  m_strSearch->setPlaceholderText(tr("Search UUID or text…"));
  m_strSearch->setClearButtonEnabled(true);
  ctrlRow->addWidget(m_strSearch, 1);

  m_copyOrigBtn = new QPushButton(tr("Original = Translated"), this);
  m_copyOrigBtn->setToolTip(tr("Pre-fill empty translation cells with the original text.\n"
                               "Useful as a starting point when editing existing markup."));
  m_copyOrigBtn->setEnabled(false);
  ctrlRow->addWidget(m_copyOrigBtn);

  root->addLayout(ctrlRow);

  // ── String table ─────────────────────────────────────────────────────────
  m_table = new QTableWidget(0, 3, this);
  m_table->setHorizontalHeaderLabels({tr("UUID"), tr("Original"), tr("Translation")});
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_table->verticalHeader()->setDefaultSectionSize(52);
  m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_table->setWordWrap(true);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
                           QAbstractItemView::EditKeyPressed);
  m_table->setAlternatingRowColors(true);

  auto* delegate = new HtmlItemDelegate(m_table);
  m_table->setItemDelegate(delegate);

  root->addWidget(m_table, 4);

  // ── AI translation group ──────────────────────────────────────────────────
  auto* aiGroup  = new QGroupBox(tr("AI Translation (Google Gemini)"), this);
  auto* aiLayout = new QVBoxLayout(aiGroup);
  aiLayout->setSpacing(6);

  // API key row
  auto* keyRow = new QHBoxLayout;
  keyRow->addWidget(new QLabel(tr("API Key:"), aiGroup));
  m_apiKeyEdit = new QLineEdit(aiGroup);
  m_apiKeyEdit->setEchoMode(QLineEdit::Password);
  m_apiKeyEdit->setPlaceholderText(tr("Paste Google AI Studio API key…"));
  m_apiKeyEdit->setText(loadApiKey());
  keyRow->addWidget(m_apiKeyEdit, 1);
  auto* saveKeyBtn = new QPushButton(tr("Save"), aiGroup);
  saveKeyBtn->setMaximumWidth(60);
  keyRow->addWidget(saveKeyBtn);
  aiLayout->addLayout(keyRow);

  // Model row
  auto* modelRow = new QHBoxLayout;
  modelRow->addWidget(new QLabel(tr("Model:"), aiGroup));
  m_modelCombo = new QComboBox(aiGroup);
  for (const auto& m : LWizardAiTranslator::availableModels())
    m_modelCombo->addItem(m.display, m.apiName);
  modelRow->addWidget(m_modelCombo, 1);
  aiLayout->addLayout(modelRow);

  // Action row
  auto* aiActionRow = new QHBoxLayout;
  m_aiTransBtn      = new QPushButton(tr("Translate Selected Rows"), aiGroup);
  m_aiTransBtn->setToolTip(tr("Translate the selected table rows using Gemini.\n"
                              "Select rows first, then click. All existing translations\n"
                              "are sent as context to ensure consistent terminology."));
  m_aiTransBtn->setEnabled(false);
  aiActionRow->addWidget(m_aiTransBtn);

  m_clipboardCopyBtn = new QPushButton(tr("Copy Prompt to Clipboard"), aiGroup);
  m_clipboardCopyBtn->setToolTip(
      tr("Build a complete translation prompt for the selected rows and copy it\n"
         "to the clipboard. Paste into any AI chat (ChatGPT, Claude, Gemini web…).\n"
         "The AI's reply can be imported back with \"Import from Clipboard\"."));
  m_clipboardCopyBtn->setEnabled(false);
  aiActionRow->addWidget(m_clipboardCopyBtn);

  m_clipboardImportBtn = new QPushButton(tr("Import from Clipboard"), aiGroup);
  m_clipboardImportBtn->setToolTip(
      tr("Parse the AI's reply from the clipboard and apply the translations\n"
         "to the table. Accepts raw JSON or the full chat reply."));
  m_clipboardImportBtn->setEnabled(false);
  aiActionRow->addWidget(m_clipboardImportBtn);

  aiActionRow->addStretch();
  aiLayout->addLayout(aiActionRow);

  // Progress row
  m_aiProgress = new QProgressBar(aiGroup);
  m_aiProgress->setVisible(false);
  m_aiProgress->setRange(0, 100);
  aiLayout->addWidget(m_aiProgress);

  m_aiStatusLabel = new QLabel(aiGroup);
  m_aiStatusLabel->setWordWrap(true);
  aiLayout->addWidget(m_aiStatusLabel);

  root->addWidget(aiGroup);

  // ── Export buttons ────────────────────────────────────────────────────────
  auto* exportRow = new QHBoxLayout;
  exportRow->addStretch();

  m_exportPakBtn = new QPushButton(tr("Export .pak…"), this);
  m_exportPakBtn->setToolTip(tr("Pack the translation into a .pak file and save it."));
  m_exportPakBtn->setEnabled(false);
  exportRow->addWidget(m_exportPakBtn);

  m_exportModBtn = new QPushButton(tr("Export as Mod"), this);
  m_exportModBtn->setToolTip(tr("Create a mod in Mod Organizer with the packed translation."));
  m_exportModBtn->setEnabled(false);
  exportRow->addWidget(m_exportModBtn);

  root->addLayout(exportRow);

  // ── Connections ───────────────────────────────────────────────────────────
  connect(m_modSearch, &QLineEdit::textChanged, this, &TranslationTab::onModSearchChanged);
  connect(m_modList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem*) {
    onModSelected();
  });
  connect(m_loadBtn, &QPushButton::clicked, this, &TranslationTab::onLoadClicked);
  connect(m_strSearch, &QLineEdit::textChanged, this, &TranslationTab::onStringSearchChanged);
  connect(m_copyOrigBtn, &QPushButton::clicked, this, &TranslationTab::onCopyOriginalClicked);
  connect(m_table, &QTableWidget::cellChanged, this, &TranslationTab::onTableCellChanged);
  connect(m_exportPakBtn, &QPushButton::clicked, this, &TranslationTab::onExportPak);
  connect(m_exportModBtn, &QPushButton::clicked, this, &TranslationTab::onExportAsMod);

  connect(saveKeyBtn, &QPushButton::clicked, this, [this]() {
    const QString key = m_apiKeyEdit->text().trimmed();
    saveApiKey(key);
    m_aiTranslator->setApiKey(key);
    m_aiStatusLabel->setText(key.isEmpty() ? tr("API key cleared.") : tr("API key saved."));
    // Re-evaluate button states regardless of whether strings were loaded first
    m_aiTransBtn->setEnabled(!m_originalStrings.isEmpty() && m_aiTranslator->hasApiKey());
    m_clipboardCopyBtn->setEnabled(!m_originalStrings.isEmpty());
    m_clipboardImportBtn->setEnabled(!m_originalStrings.isEmpty());
  });
  connect(m_apiKeyEdit, &QLineEdit::returnPressed, saveKeyBtn, &QPushButton::click);
  connect(m_aiTransBtn, &QPushButton::clicked, this, &TranslationTab::onAiTranslateClicked);
  connect(
      m_clipboardCopyBtn, &QPushButton::clicked, this, &TranslationTab::onCopyPromptToClipboard);
  connect(
      m_clipboardImportBtn, &QPushButton::clicked, this, &TranslationTab::onImportFromClipboard);

  // Model selection — update translator immediately
  connect(m_modelCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
    if (idx >= 0)
      m_aiTranslator->setModel(m_modelCombo->itemData(idx).toString());
  });
  // Init translator model from combo default
  m_aiTranslator->setModel(m_modelCombo->itemData(0).toString());

  // Rate-limit countdown in status label
  connect(m_aiTranslator, &LWizardAiTranslator::rateLimited, this, [this](int secs) {
    m_aiStatusLabel->setText(tr("Rate limited — retrying in %1 s…").arg(secs));
  });
}

void TranslationTab::populateModList()
{
  m_modList->clear();

  auto*             modList = m_organizer->modList();
  const QStringList names   = modList->allModsByProfilePriority();
  for (const QString& name : names) {
    if (name.isEmpty())
      continue;
    auto* mod = modList->getMod(name);
    if (mod && mod->isSeparator())
      continue;
    m_modList->addItem(name);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString TranslationTab::currentSrcLang() const
{
  return m_srcLangCombo ? m_srcLangCombo->currentText() : QStringLiteral("English");
}

QString TranslationTab::currentDstLang() const
{
  return m_dstLangCombo ? m_dstLangCombo->currentText() : QStringLiteral("Russian");
}

QString TranslationTab::modFolderName() const
{
  return m_currentMod + QStringLiteral(" - ") + currentDstLang();
}

QString TranslationTab::translationsFilePath() const
{
  if (m_currentMod.isEmpty())
    return {};
  const QString dir = m_organizer->basePath() + QStringLiteral("/plugins/lwizard/translations");
  // Sanitize mod name for use in a file name
  QString safe = m_currentMod;
  for (QChar& c : safe) {
    if (c == QChar('/') || c == QChar('\\') || c == QChar(':') || c == QChar('*') ||
        c == QChar('?') || c == QChar('"') || c == QChar('<') || c == QChar('>') || c == QChar('|'))
      c = QChar('_');
  }
  return dir + QChar('/') + safe + QChar('_') + currentDstLang() + QStringLiteral(".json");
}

void TranslationTab::loadTranslationsFromDisk()
{
  m_translations.clear();
  const QString path = translationsFilePath();
  if (path.isEmpty())
    return;

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return;

  QJsonParseError     err;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return;

  const QJsonObject root  = doc.object();
  const QJsonObject trans = root[QStringLiteral("translations")].toObject();
  for (auto it = trans.begin(); it != trans.end(); ++it)
    m_translations[it.key()] = it.value().toString();
}

void TranslationTab::saveTranslationsToDisk() const
{
  const QString path = translationsFilePath();
  if (path.isEmpty())
    return;

  QDir().mkpath(QFileInfo(path).absolutePath());

  // Collect non-empty translations from the table (authoritative in-memory source)
  QJsonObject trans;
  for (auto it = m_translations.constBegin(); it != m_translations.constEnd(); ++it) {
    if (!it.value().isEmpty())
      trans[it.key()] = it.value();
  }

  QJsonObject root;
  root[QStringLiteral("modName")]        = m_currentMod;
  root[QStringLiteral("targetLanguage")] = currentDstLang();
  root[QStringLiteral("translations")]   = trans;
  // Preserve UUID if already stored
  QFile fr(path);
  if (fr.open(QIODevice::ReadOnly)) {
    QJsonParseError     err;
    const QJsonDocument existing = QJsonDocument::fromJson(fr.readAll(), &err);
    fr.close();
    if (err.error == QJsonParseError::NoError && existing.isObject()) {
      const QString storedUuid = existing.object()[QStringLiteral("uuid")].toString();
      if (!storedUuid.isEmpty())
        root[QStringLiteral("uuid")] = storedUuid;
    }
  }

  QFile fw(path);
  if (fw.open(QIODevice::WriteOnly | QIODevice::Truncate))
    fw.write(QJsonDocument(root).toJson());
}

QString TranslationTab::storedOrNewUuid() const
{
  const QString path = translationsFilePath();
  if (!path.isEmpty()) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
      QJsonParseError     err;
      const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
      if (err.error == QJsonParseError::NoError && doc.isObject()) {
        const QString stored = doc.object()[QStringLiteral("uuid")].toString();
        if (!stored.isEmpty())
          return stored;
      }
    }
  }
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void TranslationTab::storeUuid(const QString& uuid) const
{
  const QString path = translationsFilePath();
  if (path.isEmpty())
    return;

  QDir().mkpath(QFileInfo(path).absolutePath());

  QJsonObject root;
  QFile       fr(path);
  if (fr.open(QIODevice::ReadOnly)) {
    QJsonParseError     err;
    const QJsonDocument existing = QJsonDocument::fromJson(fr.readAll(), &err);
    fr.close();
    if (err.error == QJsonParseError::NoError && existing.isObject())
      root = existing.object();
  }
  root[QStringLiteral("uuid")] = uuid;

  QFile fw(path);
  if (fw.open(QIODevice::WriteOnly | QIODevice::Truncate))
    fw.write(QJsonDocument(root).toJson());
}

void TranslationTab::setLoadingState(bool loading)
{
  m_loadBtn->setEnabled(!loading);
  m_loadBtn->setText(loading ? tr("Loading…") : tr("Load Strings"));
  m_modList->setEnabled(!loading);
  m_srcLangCombo->setEnabled(!loading);
  m_dstLangCombo->setEnabled(!loading);
}

void TranslationTab::setExportEnabled(bool enabled)
{
  m_exportPakBtn->setEnabled(enabled);
  m_exportModBtn->setEnabled(enabled);
  m_copyOrigBtn->setEnabled(enabled);
  m_aiTransBtn->setEnabled(enabled && m_aiTranslator->hasApiKey());
  // Clipboard copy needs strings loaded; import is always available
  m_clipboardCopyBtn->setEnabled(enabled);
  m_clipboardImportBtn->setEnabled(enabled);
}

void TranslationTab::setAiBusy(bool busy)
{
  m_aiTransBtn->setEnabled(!busy && !m_originalStrings.isEmpty() && m_aiTranslator->hasApiKey());
  m_clipboardCopyBtn->setEnabled(!busy && !m_originalStrings.isEmpty());
  m_clipboardImportBtn->setEnabled(!busy && !m_originalStrings.isEmpty());
  m_aiProgress->setVisible(busy);
  if (!busy)
    m_aiProgress->setValue(0);
}

// ---------------------------------------------------------------------------
// Table filling / filtering
// ---------------------------------------------------------------------------

void TranslationTab::rebuildAllRows()
{
  m_allRows.clear();
  m_allRows.reserve(m_originalStrings.size());
  for (auto it = m_originalStrings.constBegin(); it != m_originalStrings.constEnd(); ++it) {
    Row r;
    r.uuid       = it.key();
    r.original   = it.value();
    r.translated = m_translations.value(it.key());
    m_allRows.append(r);
  }
}

void TranslationTab::fillTable(const QVector<Row>& rows)
{
  QSignalBlocker blk(m_table);
  m_table->setRowCount(0);
  m_table->setRowCount(rows.size());

  // Update header label for translation column
  QStringList headers = {tr("UUID"), tr("Original"), tr("%1 Translation").arg(currentDstLang())};
  m_table->setHorizontalHeaderLabels(headers);

  for (int r = 0; r < rows.size(); ++r) {
    const Row& row = rows[r];

    auto* uuidItem = new QTableWidgetItem(row.uuid);
    uuidItem->setFlags(uuidItem->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(r, 0, uuidItem);

    auto* origItem = new QTableWidgetItem(row.original);
    origItem->setFlags(origItem->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(r, 1, origItem);

    auto* transItem = new QTableWidgetItem(row.translated);
    m_table->setItem(r, 2, transItem);
  }
}

// ---------------------------------------------------------------------------
// Slots — mod list
// ---------------------------------------------------------------------------

void TranslationTab::onModSearchChanged(const QString& filter)
{
  for (int i = 0; i < m_modList->count(); ++i) {
    auto*      item = m_modList->item(i);
    const bool v    = filter.isEmpty() || item->text().contains(filter, Qt::CaseInsensitive);
    item->setHidden(!v);
  }
}

void TranslationTab::onModSelected()
{
  auto* item = m_modList->currentItem();
  if (!item) {
    m_loadBtn->setEnabled(false);
    return;
  }
  m_loadBtn->setEnabled(true);
  m_statusLabel->clear();
}

// ---------------------------------------------------------------------------
// Slots — loading
// ---------------------------------------------------------------------------

void TranslationTab::onLoadClicked()
{
  auto* item = m_modList->currentItem();
  if (!item)
    return;

  const QString modName = item->text();
  const QString srcLang = currentSrcLang();

  if (m_loadThread && m_loadThread->isRunning())
    return;

  m_currentMod = modName;
  setLoadingState(true);
  setExportEnabled(false);
  m_statusLabel->setText(tr("Extracting strings…"));

  // Load translations saved from a previous session before showing the table
  loadTranslationsFromDisk();

  // QPointer guards against use-after-free if the tab is closed while the
  // thread is still running.  The thread captures the QPointer by value so
  // it never touches a dangling pointer.  qApp is used as the invokeMethod
  // context (it outlives everything); the QPointer check inside the lambda
  // runs on the main thread where it is safe to dereference.
  QPointer<TranslationTab> weakSelf   = this;
  auto                     contentRef = m_content;
  m_loadThread                        = QThread::create([weakSelf, modName, srcLang, contentRef]() {
    const QMap<QString, QString> strings = contentRef->loadStringsSync(modName, srcLang);
    if (strings.isEmpty())
      QMetaObject::invokeMethod(
          qApp,
          [weakSelf]() {
            if (weakSelf)
              weakSelf->onLoadError(
                  TranslationTab::tr("No localization found for this mod / language."));
          },
          Qt::QueuedConnection);
    else
      QMetaObject::invokeMethod(
          qApp,
          [weakSelf, strings]() {
            if (weakSelf)
              weakSelf->onStringsLoaded(strings);
          },
          Qt::QueuedConnection);
  });
  connect(m_loadThread, &QThread::finished, m_loadThread, &QThread::deleteLater);
  m_loadThread->start();
}

void TranslationTab::onStringsLoaded(const QMap<QString, QString>& strings)
{
  m_originalStrings = strings;
  rebuildAllRows();
  fillTable(m_allRows);

  // Ensure translator has the saved key
  if (!m_aiTranslator->hasApiKey())
    m_aiTranslator->setApiKey(loadApiKey());

  setLoadingState(false);
  setExportEnabled(true);
  m_statusLabel->setText(tr("%1 strings loaded.").arg(strings.size()));
  LWizardLog::info(QStringLiteral("Translation tab: loaded %1 strings from '%2'")
                       .arg(strings.size())
                       .arg(m_currentMod));
}

void TranslationTab::onLoadError(const QString& message)
{
  setLoadingState(false);
  setExportEnabled(false);
  m_statusLabel->setText(message);
  LWizardLog::warn(QStringLiteral("Translation tab: %1").arg(message));
}

// ---------------------------------------------------------------------------
// Slots — string filter
// ---------------------------------------------------------------------------

void TranslationTab::onStringSearchChanged(const QString& filter)
{
  if (filter.isEmpty()) {
    fillTable(m_allRows);
    return;
  }
  QVector<Row> filtered;
  for (const Row& r : m_allRows) {
    if (r.uuid.contains(filter, Qt::CaseInsensitive) ||
        r.original.contains(filter, Qt::CaseInsensitive) ||
        r.translated.contains(filter, Qt::CaseInsensitive))
      filtered.append(r);
  }
  fillTable(filtered);
}

// ---------------------------------------------------------------------------
// Slots — table edits
// ---------------------------------------------------------------------------

void TranslationTab::onTableCellChanged(int row, int col)
{
  if (col != 2)
    return;

  auto* item = m_table->item(row, 2);
  auto* uuid = m_table->item(row, 0);
  if (!item || !uuid)
    return;

  m_translations[uuid->text()] = item->text();
  saveTranslationsToDisk();
}

void TranslationTab::onCopyOriginalClicked()
{
  QSignalBlocker blk(m_table);

  for (int r = 0; r < m_table->rowCount(); ++r) {
    auto* transItem = m_table->item(r, 2);
    auto* origItem  = m_table->item(r, 1);
    auto* uuidItem  = m_table->item(r, 0);
    if (!transItem || !origItem || !uuidItem)
      continue;
    // Only fill if the cell is currently empty
    if (transItem->text().isEmpty()) {
      const QString orig = origItem->text();
      transItem->setText(orig);
      m_translations[uuidItem->text()] = orig;
    }
  }

  saveTranslationsToDisk();
  // Also update m_allRows so the filter doesn't lose changes
  for (Row& r : m_allRows) {
    if (m_translations.contains(r.uuid))
      r.translated = m_translations[r.uuid];
  }
}

// ---------------------------------------------------------------------------
// XML / mod structure generation
// ---------------------------------------------------------------------------

QString TranslationTab::translationXml() const
{
  QString          xml;
  QXmlStreamWriter xw(&xml);
  xw.setAutoFormatting(true);
  xw.writeStartDocument(QStringLiteral("1.0"));
  xw.writeStartElement(QStringLiteral("contentList"));
  xw.writeAttribute(QStringLiteral("xmlns:xsd"),
                    QStringLiteral("http://www.w3.org/2001/XMLSchema"));
  xw.writeAttribute(QStringLiteral("xmlns:xsi"),
                    QStringLiteral("http://www.w3.org/2001/XMLSchema-instance"));

  for (auto it = m_translations.constBegin(); it != m_translations.constEnd(); ++it) {
    if (it.value().isEmpty())
      continue;
    xw.writeStartElement(QStringLiteral("content"));
    xw.writeAttribute(QStringLiteral("contentuid"), it.key());
    xw.writeAttribute(QStringLiteral("version"), QStringLiteral("1"));
    xw.writeCharacters(it.value());
    xw.writeEndElement();
  }

  xw.writeEndElement(); // contentList
  xw.writeEndDocument();
  return xml;
}

QString TranslationTab::metaLsx(const QString& folderName, const QString& uuid) const
{
  QString          out;
  QXmlStreamWriter xw(&out);
  xw.setAutoFormatting(true);
  xw.setAutoFormattingIndent(3);
  xw.writeStartDocument(QStringLiteral("1.0"), true);

  xw.writeStartElement(QStringLiteral("save"));
  xw.writeStartElement(QStringLiteral("version"));
  xw.writeAttribute(QStringLiteral("major"), QStringLiteral("4"));
  xw.writeAttribute(QStringLiteral("minor"), QStringLiteral("0"));
  xw.writeAttribute(QStringLiteral("revision"), QStringLiteral("9"));
  xw.writeAttribute(QStringLiteral("build"), QStringLiteral("331"));
  xw.writeEndElement(); // version

  xw.writeStartElement(QStringLiteral("region"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("Config"));

  xw.writeStartElement(QStringLiteral("node"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("root"));
  xw.writeStartElement(QStringLiteral("children"));

  // Dependencies (empty)
  xw.writeStartElement(QStringLiteral("node"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("Dependencies"));
  xw.writeEndElement();

  // ModuleInfo
  xw.writeStartElement(QStringLiteral("node"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("ModuleInfo"));

  auto attr = [&](const QString& id, const QString& type, const QString& value) {
    xw.writeStartElement(QStringLiteral("attribute"));
    xw.writeAttribute(QStringLiteral("id"), id);
    xw.writeAttribute(QStringLiteral("type"), type);
    xw.writeAttribute(QStringLiteral("value"), value);
    xw.writeEndElement();
  };

  attr(QStringLiteral("Author"), QStringLiteral("LSString"), QStringLiteral(""));
  attr(QStringLiteral("CharacterCreationLevelName"),
       QStringLiteral("FixedString"),
       QStringLiteral(""));
  attr(QStringLiteral("Description"),
       QStringLiteral("LSString"),
       QStringLiteral("Translation mod created by lwizard"));
  attr(QStringLiteral("Folder"), QStringLiteral("LSString"), folderName);
  attr(QStringLiteral("LobbyLevelName"), QStringLiteral("FixedString"), QStringLiteral(""));
  attr(QStringLiteral("MD5"), QStringLiteral("LSString"), QStringLiteral(""));
  attr(
      QStringLiteral("MainMenuBackgroundVideo"), QStringLiteral("FixedString"), QStringLiteral(""));
  attr(QStringLiteral("MenuLevelName"), QStringLiteral("FixedString"), QStringLiteral(""));
  attr(QStringLiteral("Name"), QStringLiteral("LSString"), folderName);
  attr(QStringLiteral("NumPlayers"), QStringLiteral("uint8"), QStringLiteral("4"));
  attr(QStringLiteral("PhotoBooth"), QStringLiteral("FixedString"), QStringLiteral(""));
  attr(QStringLiteral("StartupLevelName"), QStringLiteral("FixedString"), QStringLiteral(""));
  attr(QStringLiteral("Tags"), QStringLiteral("LSString"), QStringLiteral(""));
  attr(QStringLiteral("Type"), QStringLiteral("FixedString"), QStringLiteral("Add-on"));
  attr(QStringLiteral("UUID"), QStringLiteral("FixedString"), uuid);
  attr(QStringLiteral("Version64"), QStringLiteral("int64"), QStringLiteral("36028797018963968"));

  // children of ModuleInfo
  xw.writeStartElement(QStringLiteral("children"));

  xw.writeStartElement(QStringLiteral("node"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("PublishVersion"));
  attr(QStringLiteral("Version64"), QStringLiteral("int64"), QStringLiteral("36028797018963968"));
  xw.writeEndElement();

  xw.writeStartElement(QStringLiteral("node"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("TargetModes"));
  xw.writeStartElement(QStringLiteral("children"));
  xw.writeStartElement(QStringLiteral("node"));
  xw.writeAttribute(QStringLiteral("id"), QStringLiteral("Target"));
  attr(QStringLiteral("Object"), QStringLiteral("FixedString"), QStringLiteral("Story"));
  xw.writeEndElement(); // Target node
  xw.writeEndElement(); // TargetModes children
  xw.writeEndElement(); // TargetModes node

  xw.writeEndElement(); // ModuleInfo children
  xw.writeEndElement(); // ModuleInfo node
  xw.writeEndElement(); // root children
  xw.writeEndElement(); // root node
  xw.writeEndElement(); // region
  xw.writeEndElement(); // save
  xw.writeEndDocument();
  return out;
}

bool TranslationTab::buildModStructure(const QString& rootPath,
                                       const QString& folderName,
                                       QString*       outXmlName) const
{
  // sanitize modName for use as XML filename (no path separators)
  QString xmlName = m_currentMod;
  for (QChar& c : xmlName) {
    if (c == QChar('/') || c == QChar('\\') || c == QChar(':') || c == QChar('*') ||
        c == QChar('?') || c == QChar('"') || c == QChar('<') || c == QChar('>') || c == QChar('|'))
      c = QChar('_');
  }

  if (outXmlName)
    *outXmlName = xmlName;

  const QString uuid = storedOrNewUuid();
  storeUuid(uuid);

  // Mods/{folderName}/meta.lsx
  const QString modsDir = rootPath + QStringLiteral("/Mods/") + folderName;
  if (!QDir().mkpath(modsDir))
    return false;

  QFile meta(modsDir + QStringLiteral("/meta.lsx"));
  if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  meta.write(metaLsx(folderName, uuid).toUtf8());
  meta.close();

  // Localization/{dstLang}/{xmlName}.xml
  const QString locDir = rootPath + QStringLiteral("/Localization/") + currentDstLang();
  if (!QDir().mkpath(locDir))
    return false;

  QFile xml(locDir + QChar('/') + xmlName + QStringLiteral(".xml"));
  if (!xml.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  xml.write(translationXml().toUtf8());
  xml.close();

  return true;
}

bool TranslationTab::packWithDivine(const QString& sourcePath, const QString& outputPak) const
{
  const QString divine = LWizardDivine::existingExecutable(m_organizer);
  if (divine.isEmpty()) {
    LWizardLog::warn(QStringLiteral("Translation export: Divine.exe not found"));
    return false;
  }

  QProcess proc;
  proc.start(divine,
             {QStringLiteral("-g"),
              QStringLiteral("bg3"),
              QStringLiteral("-a"),
              QStringLiteral("create-package"),
              QStringLiteral("-s"),
              sourcePath,
              QStringLiteral("-d"),
              outputPak});

  if (!proc.waitForStarted(5000) || !proc.waitForFinished(120000)) {
    proc.kill();
    LWizardLog::warn(QStringLiteral("Translation export: Divine.exe timed out"));
    return false;
  }
  if (proc.exitCode() != 0) {
    LWizardLog::warn(QStringLiteral("Translation export: Divine.exe exit %1\n%2")
                         .arg(proc.exitCode())
                         .arg(QString::fromLocal8Bit(proc.readAllStandardError())));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Slots — export
// ---------------------------------------------------------------------------

void TranslationTab::onExportPak()
{
  if (m_currentMod.isEmpty() || m_translations.isEmpty()) {
    QMessageBox::warning(this, tr("Export"), tr("No translations to export."));
    return;
  }

  const QString defaultName = modFolderName() + QStringLiteral(".pak");
  const QString dest        = QFileDialog::getSaveFileName(
      this, tr("Save .pak"), QDir::homePath() + QChar('/') + defaultName, tr("BG3 Pack (*.pak)"));
  if (dest.isEmpty())
    return;

  QTemporaryDir tmp;
  if (!tmp.isValid()) {
    QMessageBox::critical(this, tr("Export"), tr("Failed to create temp directory."));
    return;
  }

  const QString folder = modFolderName();
  if (!buildModStructure(tmp.path(), folder)) {
    QMessageBox::critical(this, tr("Export"), tr("Failed to build mod structure."));
    return;
  }

  if (!packWithDivine(tmp.path(), dest)) {
    QMessageBox::critical(this,
                          tr("Export"),
                          tr("Divine.exe failed to pack the mod.\n"
                             "Check the Logs tab for details."));
    return;
  }

  QMessageBox::information(this, tr("Export"), tr("Exported successfully:\n%1").arg(dest));
  LWizardLog::info(QStringLiteral("Translation export: .pak saved to %1").arg(dest));
}

void TranslationTab::onExportAsMod()
{
  if (m_currentMod.isEmpty() || m_translations.isEmpty()) {
    QMessageBox::warning(this, tr("Export"), tr("No translations to export."));
    return;
  }

  const QString folder   = modFolderName();
  const QString modsPath = m_organizer->modsPath();
  const QString modDir   = modsPath + QChar('/') + folder;
  const QString pakDir   = modDir + QStringLiteral("/PAK_FILES");
  const QString pakPath  = pakDir + QChar('/') + folder + QStringLiteral(".pak");

  // Confirm if already exists
  if (QDir(modDir).exists()) {
    const auto btn =
        QMessageBox::question(this,
                              tr("Export as Mod"),
                              tr("Mod '%1' already exists.\nOverwrite it?").arg(folder),
                              QMessageBox::Yes | QMessageBox::No);
    if (btn != QMessageBox::Yes)
      return;
  }

  if (!QDir().mkpath(pakDir)) {
    QMessageBox::critical(
        this, tr("Export"), tr("Failed to create mod directory:\n%1").arg(pakDir));
    return;
  }

  QTemporaryDir tmp;
  if (!tmp.isValid()) {
    QMessageBox::critical(this, tr("Export"), tr("Failed to create temp directory."));
    return;
  }

  if (!buildModStructure(tmp.path(), folder)) {
    QMessageBox::critical(this, tr("Export"), tr("Failed to build mod structure."));
    return;
  }

  if (!packWithDivine(tmp.path(), pakPath)) {
    QMessageBox::critical(this,
                          tr("Export"),
                          tr("Divine.exe failed to pack the mod.\n"
                             "Check the Logs tab for details."));
    return;
  }

  // Trigger MO2 to pick up the new mod
  m_organizer->refresh(false);

  QMessageBox::information(this,
                           tr("Export as Mod"),
                           tr("Mod created:\n%1\n\nThe mod list has been refreshed.").arg(modDir));
  LWizardLog::info(QStringLiteral("Translation export: mod created at %1").arg(modDir));
}

// ---------------------------------------------------------------------------
// AI helpers — API key persistence
// ---------------------------------------------------------------------------

QString TranslationTab::aiConfigPath() const
{
  return m_organizer->basePath() + QStringLiteral("/plugins/lwizard/ai_config.json");
}

QString TranslationTab::loadApiKey() const
{
  QFile f(aiConfigPath());
  if (!f.open(QIODevice::ReadOnly))
    return {};
  QJsonParseError     err;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return {};
  return doc.object()[QStringLiteral("gemini_api_key")].toString();
}

void TranslationTab::saveApiKey(const QString& key) const
{
  const QString path = aiConfigPath();
  QDir().mkpath(QFileInfo(path).absolutePath());

  QJsonObject root;
  root[QStringLiteral("gemini_api_key")] = key;
  QFile f(path);
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    f.write(QJsonDocument(root).toJson());
}

// ---------------------------------------------------------------------------
// AI slots
// ---------------------------------------------------------------------------

void TranslationTab::onAiTranslateClicked()
{
  if (!m_aiTranslator->hasApiKey()) {
    m_aiStatusLabel->setText(tr("Enter and save a Google AI Studio API key first."));
    return;
  }

  // Collect selected rows
  const QList<QTableWidgetItem*> sel = m_table->selectedItems();
  QSet<int>                      rows;
  for (auto* item : sel)
    rows.insert(item->row());

  if (rows.isEmpty()) {
    m_aiStatusLabel->setText(tr("Select rows to translate first."));
    return;
  }

  // Build ordered list: (UUID, originalText) — preserve table order
  QList<int> sortedRows = rows.values();
  std::sort(sortedRows.begin(), sortedRows.end());

  QList<QPair<QString, QString>> toTranslate;
  for (int r : sortedRows) {
    auto* uuidItem = m_table->item(r, 0);
    auto* origItem = m_table->item(r, 1);
    if (!uuidItem || !origItem)
      continue;
    const QString uuid = uuidItem->text();
    const QString orig = origItem->text();
    if (!uuid.isEmpty() && !orig.isEmpty())
      toTranslate.append({uuid, orig});
  }

  if (toTranslate.isEmpty()) {
    m_aiStatusLabel->setText(tr("Nothing to translate in selection."));
    return;
  }

  setAiBusy(true);
  m_aiStatusLabel->setText(tr("Translating %1 strings in %2 batches…")
                               .arg(toTranslate.size())
                               .arg((toTranslate.size() + LWizardAiTranslator::kBatchSize - 1) /
                                    LWizardAiTranslator::kBatchSize));

  m_aiTranslator->translate(
      toTranslate, m_originalStrings, m_translations, currentDstLang(), currentSrcLang());
}

void TranslationTab::applyAiBatchToTable(const QMap<QString, QString>& results)
{
  QSignalBlocker blk(m_table);
  for (int r = 0; r < m_table->rowCount(); ++r) {
    auto* uuidItem  = m_table->item(r, 0);
    auto* transItem = m_table->item(r, 2);
    if (!uuidItem || !transItem)
      continue;

    const QString uuid = uuidItem->text();
    if (results.contains(uuid)) {
      transItem->setText(results.value(uuid));
      m_translations[uuid] = results.value(uuid);
      // Also update allRows cache
      for (Row& row : m_allRows) {
        if (row.uuid == uuid) {
          row.translated = results.value(uuid);
          break;
        }
      }
    }
  }
  saveTranslationsToDisk();
}

void TranslationTab::onAiBatchDone(const QMap<QString, QString>& results)
{
  applyAiBatchToTable(results);
}

void TranslationTab::onAiProgress(int done, int total)
{
  if (total > 0) {
    m_aiProgress->setValue(static_cast<int>(100.0 * done / total));
    m_aiStatusLabel->setText(tr("Translated %1 / %2 strings…").arg(done).arg(total));
  }
}

void TranslationTab::onAiFinished()
{
  setAiBusy(false);
  m_aiStatusLabel->setText(tr("Translation complete. Review and edit as needed."));
  LWizardLog::info(QStringLiteral("AI translation finished for '%1'").arg(m_currentMod));
}

void TranslationTab::onAiError(const QString& message)
{
  m_aiStatusLabel->setText(tr("Error: %1").arg(message));
  LWizardLog::warn(QStringLiteral("AI translate error: ") + message);
}

// ---------------------------------------------------------------------------
// Slots — clipboard prompt / import
// ---------------------------------------------------------------------------

void TranslationTab::onCopyPromptToClipboard()
{
  // Collect selected rows (fall back to all rows if nothing selected)
  QList<QPair<QString, QString>> selected;
  const QList<QTableWidgetItem*> selItems = m_table->selectedItems();

  QSet<int> seenRows;
  for (auto* item : selItems) {
    const int r = item->row();
    if (seenRows.contains(r))
      continue;
    seenRows.insert(r);
    auto* uuidItem = m_table->item(r, 0);
    auto* origItem = m_table->item(r, 1);
    if (uuidItem && origItem)
      selected.append({uuidItem->text(), origItem->text()});
  }

  if (selected.isEmpty()) {
    m_aiStatusLabel->setText(tr("Select rows in the table first."));
    return;
  }

  const QString prompt = m_aiTranslator->buildClipboardPrompt(
      selected, m_originalStrings, m_translations, currentDstLang(), currentSrcLang());

  QApplication::clipboard()->setText(prompt);
  m_aiStatusLabel->setText(tr("Prompt for %1 string(s) copied to clipboard. "
                              "Paste into any AI chat, then click \"Import from Clipboard\".")
                               .arg(selected.size()));
  LWizardLog::info(QStringLiteral("Clipboard prompt built for %1 strings.").arg(selected.size()));
}

void TranslationTab::onImportFromClipboard()
{
  const QString text = QApplication::clipboard()->text();
  if (text.isEmpty()) {
    m_aiStatusLabel->setText(tr("Clipboard is empty."));
    return;
  }

  const QMap<QString, QString> results = LWizardAiTranslator::importFromClipboardText(text);

  if (results.isEmpty()) {
    m_aiStatusLabel->setText(tr("Could not parse translations from clipboard. "
                                "Make sure the AI replied with the JSON block as instructed."));
    LWizardLog::warn(QStringLiteral("Clipboard import: no translations found in text."));
    return;
  }

  applyAiBatchToTable(results);
  m_aiStatusLabel->setText(tr("Imported %1 translation(s) from clipboard.").arg(results.size()));
  LWizardLog::info(
      QStringLiteral("Clipboard import: applied %1 translations.").arg(results.size()));
}
