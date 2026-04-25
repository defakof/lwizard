#pragma once

#include "services/lwizard_nexus_api.h"

#include <QMap>
#include <QPointer>
#include <QWidget>

class BG3LocalizationContent;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace MOBase { class IOrganizer; }

/**
 * "Nexus" tab in the LWizard window.
 *
 * Workflow:
 *  1. Lists all mods in the current profile.
 *  2. User clicks "Scan" to search Nexus for translations.
 *  3. Results table shows found files with Download / Open Page buttons.
 *  4. Download invokes IDownloadManager::startDownloadNexusFile().
 *
 * Also provides the API key input so the file-level detail (name, size, date)
 * is retrieved from the Nexus REST API.
 */
class NexusTab : public QWidget
{
  Q_OBJECT
public:
  /**
   * @param nexusApi  Shared API owned by the plugin (must outlive this tab).
   *                  Pass nullptr only in tests — tab will create its own instance.
   */
  explicit NexusTab(MOBase::IOrganizer*                       organizer,
                    std::shared_ptr<BG3LocalizationContent>   content,
                    LWizardNexusApi*                          nexusApi,
                    QWidget*                                  parent = nullptr);
  ~NexusTab() override;

private slots:
  void onScanAllClicked();
  void onScanModClicked();
  void onDownloadAllClicked();
  void onTranslationsReady(const QString& requestId,
                           const QList<NexusTranslationFile>& files);
  void onSearchError(const QString& requestId, const QString& error);
  void onSearchProgress(const QString& requestId, const QString& status);
  void onSaveApiKeyClicked();
  void onModFilterChanged(const QString& filter);
  void onClearResults();

private:
  void setupUi();
  void populateModList();
  void startSearch(const QString& modName);
  void addResultRows(const QString& modName, const QList<NexusTranslationFile>& files);
  void setScanning(bool scanning);

  QString loadApiKey() const;
  void    saveApiKey(const QString& key) const;

  int nexusIdForMod(const QString& modName) const;

  MOBase::IOrganizer*                     m_organizer;
  std::shared_ptr<BG3LocalizationContent> m_content;
  LWizardNexusApi*                        m_api     = nullptr;  ///< NOT owned by tab
  bool                                    m_ownApi  = false;    ///< true only if tab created it

  // ── Widgets ──────────────────────────────────────────────────────────────
  QLineEdit*    m_apiKeyEdit    = nullptr;
  QLineEdit*    m_modFilter     = nullptr;
  QTableWidget* m_modTable      = nullptr;   // left: mods
  QTableWidget* m_resultTable   = nullptr;   // right: found files
  QPushButton*  m_scanAllBtn    = nullptr;
  QPushButton*  m_dlAllBtn      = nullptr;
  QPushButton*  m_clearBtn      = nullptr;
  QProgressBar* m_progress      = nullptr;
  QLabel*       m_statusLabel   = nullptr;

  // ── State ─────────────────────────────────────────────────────────────────
  int     m_pendingScans = 0;
  int     m_totalScans   = 0;
  // modName → nexusId mapping (from IModInterface)
  QMap<QString, int> m_modNexusIds;
};
