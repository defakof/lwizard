#include "lwizard_nexus_tab.h"
#include "bg3_localization_content.h"
#include "lwizard_log.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <uibase/idownloadmanager.h>
#include <uibase/imodinterface.h>
#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NexusTab::NexusTab(MOBase::IOrganizer*                     organizer,
                   std::shared_ptr<BG3LocalizationContent> content,
                   LWizardNexusApi*                        nexusApi,
                   QWidget*                                parent)
    : QWidget(parent)
    , m_organizer(organizer)
    , m_content(std::move(content))
{
  if (nexusApi) {
    // Use the plugin's shared API — reload key from disk in case it changed
    m_api     = nexusApi;
    m_ownApi  = false;
    const QString key = loadApiKey();
    if (!key.isEmpty())
      m_api->setApiKey(key);
  } else {
    // Fallback (tests / standalone): own a private instance
    m_api    = new LWizardNexusApi(this);
    m_ownApi = true;
    m_api->setApiKey(loadApiKey());
  }

  // Connect tab-local slots — these are additive (plugin already has its own connections)
  connect(m_api, &LWizardNexusApi::translationsReady,
          this, &NexusTab::onTranslationsReady);
  connect(m_api, &LWizardNexusApi::searchError,
          this, &NexusTab::onSearchError);
  connect(m_api, &LWizardNexusApi::searchProgress,
          this, &NexusTab::onSearchProgress);

  setupUi();
  populateModList();
}

NexusTab::~NexusTab()
{
  // Disconnect our slots so the plugin's API doesn't call us after destruction
  if (m_api)
    m_api->disconnect(this);
  // Only cancel/delete if we own the instance
  if (m_ownApi && m_api) {
    m_api->cancelAll();
    // m_api parented to this widget → will be deleted by Qt
  }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

QString NexusTab::loadApiKey() const
{
  const QString path = m_organizer->basePath() +
                       QStringLiteral("/plugins/lwizard/nexus_config.json");
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  return doc.object()[QStringLiteral("api_key")].toString();
}

void NexusTab::saveApiKey(const QString& key) const
{
  const QString dir = m_organizer->basePath() +
                      QStringLiteral("/plugins/lwizard");
  QDir().mkpath(dir);
  QFile f(dir + QStringLiteral("/nexus_config.json"));
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  QJsonObject obj;
  obj[QStringLiteral("api_key")] = key;
  f.write(QJsonDocument(obj).toJson());
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void NexusTab::setupUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(6);

  // ── API key row ───────────────────────────────────────────────────────────
  auto* keyGroup  = new QGroupBox(tr("Nexus Mods API Key"), this);
  auto* keyLayout = new QHBoxLayout(keyGroup);
  keyLayout->addWidget(new QLabel(tr("API Key:"), keyGroup));
  m_apiKeyEdit = new QLineEdit(keyGroup);
  m_apiKeyEdit->setEchoMode(QLineEdit::Password);
  m_apiKeyEdit->setPlaceholderText(
      tr("Get a personal API key at www.nexusmods.com/users/myaccount?tab=api (free)"));
  m_apiKeyEdit->setText(loadApiKey());
  keyLayout->addWidget(m_apiKeyEdit, 1);
  auto* saveBtn = new QPushButton(tr("Save"), keyGroup);
  saveBtn->setMaximumWidth(60);
  keyLayout->addWidget(saveBtn);
  root->addWidget(keyGroup);

  connect(saveBtn, &QPushButton::clicked, this, &NexusTab::onSaveApiKeyClicked);
  connect(m_apiKeyEdit, &QLineEdit::returnPressed, saveBtn, &QPushButton::click);

  // ── Splitter: mod list (left) | results (right) ───────────────────────────
  auto* splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setChildrenCollapsible(false);

  // Left panel: mod list
  auto* leftPanel  = new QWidget(splitter);
  auto* leftLayout = new QVBoxLayout(leftPanel);
  leftLayout->setContentsMargins(0, 0, 0, 0);
  leftLayout->setSpacing(4);

  leftLayout->addWidget(new QLabel(tr("Mods (must have a Nexus ID):"), leftPanel));

  m_modFilter = new QLineEdit(leftPanel);
  m_modFilter->setPlaceholderText(tr("Filter mods…"));
  m_modFilter->setClearButtonEnabled(true);
  leftLayout->addWidget(m_modFilter);

  m_modTable = new QTableWidget(0, 2, leftPanel);
  m_modTable->setHorizontalHeaderLabels({tr("Mod"), tr("Nexus ID")});
  m_modTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_modTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_modTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_modTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_modTable->setAlternatingRowColors(true);
  m_modTable->setSortingEnabled(true);
  leftLayout->addWidget(m_modTable, 1);

  auto* leftBtnRow = new QHBoxLayout;
  m_scanAllBtn = new QPushButton(tr("Scan All"), leftPanel);
  m_scanAllBtn->setToolTip(
      tr("Search Nexus for translations of all mods with a Nexus ID."));
  leftBtnRow->addWidget(m_scanAllBtn);

  auto* scanSelBtn = new QPushButton(tr("Scan Selected"), leftPanel);
  scanSelBtn->setToolTip(tr("Search Nexus for translations of the selected mod only."));
  leftBtnRow->addWidget(scanSelBtn);

  m_clearBtn = new QPushButton(tr("Clear Results"), leftPanel);
  leftBtnRow->addWidget(m_clearBtn);
  leftBtnRow->addStretch();
  leftLayout->addLayout(leftBtnRow);

  splitter->addWidget(leftPanel);

  // Right panel: results
  auto* rightPanel  = new QWidget(splitter);
  auto* rightLayout = new QVBoxLayout(rightPanel);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  rightLayout->setSpacing(4);

  rightLayout->addWidget(new QLabel(tr("Available translations:"), rightPanel));

  m_resultTable = new QTableWidget(0, 7, rightPanel);
  m_resultTable->setHorizontalHeaderLabels({
      tr("Original Mod"), tr("File"), tr("Version"),
      tr("Updated"), tr("Size"), tr("Cat."), tr("Actions")});
  m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_resultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_resultTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_resultTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_resultTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  m_resultTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  m_resultTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
  m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_resultTable->setAlternatingRowColors(true);
  m_resultTable->setSortingEnabled(false); // buttons in column 6
  rightLayout->addWidget(m_resultTable, 1);

  // ── Download All row ──────────────────────────────────────────────────────
  auto* dlAllRow = new QHBoxLayout;
  m_dlAllBtn = new QPushButton(tr("Download All Latest"), rightPanel);
  m_dlAllBtn->setToolTip(
      tr("Queue the newest file from every translation found above into MO2 Downloads.\n"
         "Only the first (newest) file per unique translation mod is downloaded.\n"
         "Requires a Nexus API key."));
  m_dlAllBtn->setEnabled(false);
  dlAllRow->addWidget(m_dlAllBtn);
  dlAllRow->addStretch();
  rightLayout->addLayout(dlAllRow);

  m_progress = new QProgressBar(rightPanel);
  m_progress->setVisible(false);
  m_progress->setRange(0, 100);
  rightLayout->addWidget(m_progress);

  m_statusLabel = new QLabel(rightPanel);
  m_statusLabel->setWordWrap(true);
  rightLayout->addWidget(m_statusLabel);

  splitter->addWidget(rightPanel);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 3);
  root->addWidget(splitter, 1);

  // ── Connections ───────────────────────────────────────────────────────────
  connect(m_modFilter, &QLineEdit::textChanged, this, &NexusTab::onModFilterChanged);
  connect(m_scanAllBtn, &QPushButton::clicked, this, &NexusTab::onScanAllClicked);
  connect(scanSelBtn,   &QPushButton::clicked, this, &NexusTab::onScanModClicked);
  connect(m_clearBtn,   &QPushButton::clicked, this, &NexusTab::onClearResults);
  connect(m_dlAllBtn,   &QPushButton::clicked, this, &NexusTab::onDownloadAllClicked);
}

void NexusTab::populateModList()
{
  m_modTable->setSortingEnabled(false);
  m_modTable->setRowCount(0);
  m_modNexusIds.clear();

  auto* modList = m_organizer->modList();
  const QStringList names = modList->allModsByProfilePriority();

  for (const QString& name : names) {
    if (name.isEmpty())
      continue;

    auto* mod = modList->getMod(name);
    if (!mod)
      continue;

    const int nexusId = mod->nexusId();
    if (nexusId <= 0)
      continue;  // no Nexus ID — skip

    m_modNexusIds[name] = nexusId;

    const int row = m_modTable->rowCount();
    m_modTable->insertRow(row);

    auto* nameItem = new QTableWidgetItem(name);
    nameItem->setToolTip(name);
    m_modTable->setItem(row, 0, nameItem);

    auto* idItem = new QTableWidgetItem(QString::number(nexusId));
    idItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_modTable->setItem(row, 1, idItem);
  }

  m_modTable->setSortingEnabled(true);
  m_statusLabel->setText(
      tr("%1 mods with Nexus IDs found.").arg(m_modNexusIds.size()));
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void NexusTab::onSaveApiKeyClicked()
{
  const QString key = m_apiKeyEdit->text().trimmed();
  saveApiKey(key);
  if (m_api)
    m_api->setApiKey(key);
  m_statusLabel->setText(key.isEmpty() ? tr("API key cleared.") : tr("API key saved."));
}

void NexusTab::onModFilterChanged(const QString& filter)
{
  for (int row = 0; row < m_modTable->rowCount(); ++row) {
    auto* item = m_modTable->item(row, 0);
    const bool visible = filter.isEmpty() ||
                         (item && item->text().contains(filter, Qt::CaseInsensitive));
    m_modTable->setRowHidden(row, !visible);
  }
}

void NexusTab::onScanAllClicked()
{
  // Collect visible mods
  QStringList toScan;
  for (const QString& name : m_modNexusIds.keys())
    toScan.append(name);

  if (toScan.isEmpty()) {
    m_statusLabel->setText(tr("No mods with Nexus IDs. Run a scan from the Settings tab first."));
    return;
  }

  m_resultTable->setRowCount(0);
  m_totalScans   = toScan.size();
  m_pendingScans = toScan.size();
  setScanning(true);
  m_progress->setMaximum(m_totalScans);
  m_progress->setValue(0);

  for (const QString& name : toScan)
    startSearch(name);
}

void NexusTab::onScanModClicked()
{
  const QList<QTableWidgetItem*> sel = m_modTable->selectedItems();
  if (sel.isEmpty()) {
    m_statusLabel->setText(tr("Select a mod in the list first."));
    return;
  }

  QSet<int> rows;
  QStringList toScan;
  for (auto* item : sel) {
    if (rows.contains(item->row())) continue;
    rows.insert(item->row());
    const QString name = m_modTable->item(item->row(), 0)->text();
    toScan.append(name);
  }

  m_totalScans   = toScan.size();
  m_pendingScans = toScan.size();
  setScanning(true);
  m_progress->setMaximum(m_totalScans);
  m_progress->setValue(0);

  for (const QString& name : toScan)
    startSearch(name);
}

void NexusTab::onClearResults()
{
  m_resultTable->setRowCount(0);
  m_statusLabel->clear();
  m_dlAllBtn->setEnabled(false);
}

void NexusTab::startSearch(const QString& modName)
{
  const int nexusId = m_modNexusIds.value(modName, 0);
  if (nexusId <= 0) {
    --m_pendingScans;
    return;
  }

  // Use the plugin's target language
  QVariant v = m_organizer->pluginSetting(QStringLiteral("lwizard"),
                                          QStringLiteral("language"));
  QString lang;
  if (v.typeId() == QMetaType::QStringList) {
    const QStringList l = v.toStringList();
    lang = l.isEmpty() ? QStringLiteral("Russian") : l.first();
  } else {
    lang = v.toString();
    if (lang.isEmpty()) lang = QStringLiteral("Russian");
  }

  m_api->searchTranslations(modName, nexusId, lang);
}

void NexusTab::onTranslationsReady(const QString& requestId,
                                   const QList<NexusTranslationFile>& files)
{
  const QString modName = requestId;  // requestId == modName

  if (!files.isEmpty()) {
    addResultRows(modName, files);

    // Update content column cache
    QList<int> modIds;
    for (const auto& f : files) {
      if (!modIds.contains(f.modId))
        modIds.append(f.modId);
    }
    m_content->markNexusAvailable(modName, modIds);
  }

  --m_pendingScans;
  m_progress->setValue(m_totalScans - m_pendingScans);

  if (m_pendingScans <= 0) {
    setScanning(false);
    const int resultRows = m_resultTable->rowCount();
    m_statusLabel->setText(
        tr("Scan complete. %1 translation file(s) found.").arg(resultRows));
    LWizardLog::info(QStringLiteral("Nexus scan complete: %1 result row(s)").arg(resultRows));
    m_dlAllBtn->setEnabled(resultRows > 0 && m_organizer->downloadManager() != nullptr);
  }
}

void NexusTab::onSearchError(const QString& requestId, const QString& error)
{
  LWizardLog::warn(QStringLiteral("Nexus search error for '%1': %2").arg(requestId, error));

  --m_pendingScans;
  m_progress->setValue(m_totalScans - m_pendingScans);

  if (m_pendingScans <= 0) {
    setScanning(false);
    m_statusLabel->setText(
        tr("Scan finished with errors. Last error: %1").arg(error));
  }
}

void NexusTab::onSearchProgress(const QString& requestId, const QString& status)
{
  m_statusLabel->setText(QStringLiteral("[%1] %2").arg(requestId, status));
}

// ---------------------------------------------------------------------------
// Add rows to result table
// ---------------------------------------------------------------------------

void NexusTab::addResultRows(const QString& modName,
                             const QList<NexusTranslationFile>& files)
{
  // If API key was absent, we only have mod-level stubs (no fileId)
  if (files.isEmpty())
    return;

  const bool hasFileDetail = files.first().fileId > 0;

  if (!hasFileDetail) {
    // No API key — just show a clickable link row
    for (const auto& f : files) {
      const int row = m_resultTable->rowCount();
      m_resultTable->insertRow(row);

      m_resultTable->setItem(row, 0, new QTableWidgetItem(modName));
      m_resultTable->setItem(row, 1, new QTableWidgetItem(
          QStringLiteral("Mod #%1 (no API key — file list unavailable)").arg(f.modId)));
      m_resultTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("—")));
      m_resultTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("—")));
      m_resultTable->setItem(row, 4, new QTableWidgetItem(QStringLiteral("—")));
      m_resultTable->setItem(row, 5, new QTableWidgetItem(QStringLiteral("—")));

      auto* actWidget = new QWidget(m_resultTable);
      auto* actLayout = new QHBoxLayout(actWidget);
      actLayout->setContentsMargins(2, 2, 2, 2);
      actLayout->setSpacing(4);

      const QString url = f.modPageUrl;
      auto* openBtn = new QPushButton(tr("Open Page"), actWidget);
      openBtn->setToolTip(url);
      connect(openBtn, &QPushButton::clicked, this, [url]() {
        QDesktopServices::openUrl(QUrl(url));
      });
      actLayout->addWidget(openBtn);
      actLayout->addStretch();
      m_resultTable->setCellWidget(row, 6, actWidget);
    }
    return;
  }

  for (const auto& f : files) {
    const int row = m_resultTable->rowCount();
    m_resultTable->insertRow(row);

    // Store modId / fileId in UserRole on col 0 so "Download All" can read them
    auto* modItem = new QTableWidgetItem(modName);
    modItem->setData(Qt::UserRole,     f.modId);
    modItem->setData(Qt::UserRole + 1, f.fileId);
    m_resultTable->setItem(row, 0, modItem);

    const QString displayName = f.fileDisplayName.isEmpty()
                                    ? f.fileName
                                    : f.fileDisplayName;
    auto* fileItem = new QTableWidgetItem(displayName);
    fileItem->setToolTip(f.fileName);
    m_resultTable->setItem(row, 1, fileItem);

    m_resultTable->setItem(row, 2, new QTableWidgetItem(f.version));

    const QString dateStr = f.updatedTimestamp > 0
                                ? QDateTime::fromSecsSinceEpoch(f.updatedTimestamp)
                                      .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                : QStringLiteral("—");
    m_resultTable->setItem(row, 3, new QTableWidgetItem(dateStr));

    const QString sizeStr = f.sizeKb > 0
                                ? QStringLiteral("%1 KB").arg(f.sizeKb)
                                : QStringLiteral("—");
    m_resultTable->setItem(row, 4, new QTableWidgetItem(sizeStr));

    m_resultTable->setItem(row, 5, new QTableWidgetItem(f.category));

    // Action buttons
    auto* actWidget = new QWidget(m_resultTable);
    auto* actLayout = new QHBoxLayout(actWidget);
    actLayout->setContentsMargins(2, 2, 2, 2);
    actLayout->setSpacing(4);

    const int capturedModId  = f.modId;
    const int capturedFileId = f.fileId;
    const QString capturedUrl = f.modPageUrl;

    auto* dlBtn = new QPushButton(tr("Download"), actWidget);
    dlBtn->setToolTip(tr("Queue in MO2 Downloads via Nexus Mod Manager download."));
    connect(dlBtn, &QPushButton::clicked, this, [this, capturedModId, capturedFileId]() {
      if (!m_organizer->downloadManager()) {
        QMessageBox::warning(this, tr("Download"),
                             tr("MO2 download manager is not available."));
        return;
      }
      const int id = m_organizer->downloadManager()->startDownloadNexusFile(
          capturedModId, capturedFileId);
      if (id < 0)
        m_statusLabel->setText(tr("Download failed to start (mod %1, file %2).")
                                   .arg(capturedModId).arg(capturedFileId));
      else
        m_statusLabel->setText(tr("Download queued (id %1).").arg(id));
    });
    actLayout->addWidget(dlBtn);

    auto* openBtn = new QPushButton(tr("Page"), actWidget);
    openBtn->setToolTip(capturedUrl);
    connect(openBtn, &QPushButton::clicked, this, [capturedUrl]() {
      QDesktopServices::openUrl(QUrl(capturedUrl));
    });
    actLayout->addWidget(openBtn);

    actLayout->addStretch();
    m_resultTable->setCellWidget(row, 6, actWidget);
  }

  m_resultTable->resizeRowsToContents();
}

void NexusTab::onDownloadAllClicked()
{
  auto* dm = m_organizer->downloadManager();
  if (!dm) {
    QMessageBox::warning(this, tr("Download All"),
                         tr("MO2 download manager is not available."));
    return;
  }

  // Walk result rows, pick the FIRST (newest) file for each unique translation
  // mod ID — results are already sorted newest-first by addResultRows.
  QSet<int>  seenModIds;
  int        queued  = 0;
  int        skipped = 0;

  for (int row = 0; row < m_resultTable->rowCount(); ++row) {
    auto* item = m_resultTable->item(row, 0);
    if (!item) continue;

    const int modId  = item->data(Qt::UserRole).toInt();
    const int fileId = item->data(Qt::UserRole + 1).toInt();

    if (modId <= 0 || fileId <= 0) {
      ++skipped;
      continue;  // stub row (no API key) — can't download
    }

    if (seenModIds.contains(modId)) {
      // Older file of the same translation mod — skip
      continue;
    }
    seenModIds.insert(modId);

    const int id = dm->startDownloadNexusFile(modId, fileId);
    if (id >= 0) {
      ++queued;
      LWizardLog::info(QStringLiteral("Download All: queued mod %1 file %2 (dl id %3)")
                           .arg(modId).arg(fileId).arg(id));
    } else {
      ++skipped;
      LWizardLog::warn(QStringLiteral("Download All: failed to queue mod %1 file %2")
                           .arg(modId).arg(fileId));
    }
  }

  m_statusLabel->setText(
      tr("Download All: %1 file(s) queued, %2 skipped (stubs or errors).")
          .arg(queued).arg(skipped));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void NexusTab::setScanning(bool scanning)
{
  m_scanAllBtn->setEnabled(!scanning);
  m_progress->setVisible(scanning);
  if (!scanning)
    m_progress->setValue(0);
}

int NexusTab::nexusIdForMod(const QString& modName) const
{
  return m_modNexusIds.value(modName, 0);
}
