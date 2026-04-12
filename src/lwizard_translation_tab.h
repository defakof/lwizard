#pragma once

#include <memory>

#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

class BG3LocalizationContent;
class LWizardAiTranslator;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QThread;

namespace MOBase {
class IOrganizer;
}

/**
 * "Translation" tab in the LWizard tool window.
 *
 * Workflow:
 *  1. Pick a source mod from the mod list (search supported).
 *  2. Choose source language (default: English) and target language
 *     (default: from plugin settings).
 *  3. Click "Load Strings" — strings are extracted in a background thread.
 *  4. Table shows UUID | Original | Translation columns.
 *     Original renders BG3 markup as HTML (read-only).
 *     Translation renders HTML when not editing, raw text when editing.
 *  5. "Original = Translated" pre-fills all empty translation cells with the
 *     original text so the user can edit from there.
 *  6. Translations are auto-saved to
 *     <MO2>/plugins/lwizard/translations/<modName>_<lang>.json
 *     on every cell edit.
 *  7. "Export .pak" / "Export as Mod" pack and deploy the result.
 */
class TranslationTab : public QWidget
{
  Q_OBJECT

public:
  explicit TranslationTab(MOBase::IOrganizer* organizer,
                          std::shared_ptr<BG3LocalizationContent> content,
                          QWidget* parent = nullptr);
  ~TranslationTab() override;

private:
  // ── dependencies ─────────────────────────────────────────────────────────
  MOBase::IOrganizer*                    m_organizer;
  std::shared_ptr<BG3LocalizationContent> m_content;

  // ── UI ───────────────────────────────────────────────────────────────────
  QListWidget*  m_modList       = nullptr;
  QLineEdit*    m_modSearch     = nullptr;
  QComboBox*    m_srcLangCombo  = nullptr;
  QComboBox*    m_dstLangCombo  = nullptr;
  QPushButton*  m_loadBtn       = nullptr;
  QLabel*       m_statusLabel   = nullptr;
  QLineEdit*    m_strSearch     = nullptr;
  QTableWidget* m_table         = nullptr;
  QPushButton*  m_copyOrigBtn   = nullptr;
  QPushButton*  m_exportPakBtn  = nullptr;
  QPushButton*  m_exportModBtn  = nullptr;

  // AI section
  QLineEdit*    m_apiKeyEdit    = nullptr;
  QComboBox*    m_modelCombo    = nullptr;
  QPushButton*  m_aiTransBtn       = nullptr;
  QPushButton*  m_clipboardCopyBtn = nullptr;
  QPushButton*  m_clipboardImportBtn = nullptr;
  QProgressBar* m_aiProgress      = nullptr;
  QLabel*       m_aiStatusLabel   = nullptr;

  // ── state ─────────────────────────────────────────────────────────────────
  QString m_currentMod;
  QMap<QString, QString> m_originalStrings;   // UUID → original text
  QMap<QString, QString> m_translations;      // UUID → translated text

  struct Row { QString uuid, original, translated; };
  QVector<Row> m_allRows;

  QThread*             m_loadThread = nullptr;
  LWizardAiTranslator* m_aiTranslator = nullptr;

  // ── setup ─────────────────────────────────────────────────────────────────
  void setupUi();
  void populateModList();

  // ── slots ─────────────────────────────────────────────────────────────────
private slots:
  void onModSearchChanged(const QString& filter);
  void onModSelected();
  void onLoadClicked();
  void onStringsLoaded(const QMap<QString, QString>& strings);
  void onLoadError(const QString& message);
  void onStringSearchChanged(const QString& filter);
  void onTableCellChanged(int row, int col);
  void onCopyOriginalClicked();
  void onExportPak();
  void onExportAsMod();

  // AI slots
  void onAiTranslateClicked();
  void onAiBatchDone(const QMap<QString, QString>& results);
  void onAiProgress(int done, int total);
  void onAiFinished();
  void onAiError(const QString& message);
  void onCopyPromptToClipboard();
  void onImportFromClipboard();

  // ── helpers ───────────────────────────────────────────────────────────────
private:
  void fillTable(const QVector<Row>& rows);
  void rebuildAllRows();
  void setLoadingState(bool loading);
  void setExportEnabled(bool enabled);

  QString translationsFilePath() const;
  void    loadTranslationsFromDisk();
  void    saveTranslationsToDisk() const;

  QString modFolderName() const;          // "{currentMod} - {dstLang}"
  QString translationXml() const;         // full XML string for the translated file
  QString metaLsx(const QString& folderName, const QString& uuid) const;
  bool    buildModStructure(const QString& rootPath, const QString& modFolder,
                            QString* outXmlName = nullptr) const;
  bool    packWithDivine(const QString& sourcePath, const QString& outputPak) const;
  QString storedOrNewUuid() const;
  void    storeUuid(const QString& uuid) const;

  QString currentSrcLang() const;
  QString currentDstLang() const;

  // AI helpers
  QString loadApiKey() const;
  void    saveApiKey(const QString& key) const;
  QString aiConfigPath() const;
  void    applyAiBatchToTable(const QMap<QString, QString>& results);
  void    setAiBusy(bool busy);
};
