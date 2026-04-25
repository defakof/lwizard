#pragma once

#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct NexusTranslationFile
{
  int     modId  = 0;
  int     fileId = 0;
  QString modName;         ///< Display name of the translation mod
  QString fileDisplayName; ///< Human-readable name of the specific file
  QString fileName;        ///< Archive file name (ModName-123-1.0-ts.7z)
  QString version;
  qint64  updatedTimestamp = 0; ///< Unix seconds
  qint64  sizeKb           = 0;
  QString modPageUrl;
  QString category; ///< "MAIN", "OPTIONAL", etc.
};

// ---------------------------------------------------------------------------
// API class
// ---------------------------------------------------------------------------

/**
 * Async Nexus Mods integration for BG3 translation discovery.
 *
 * Discovery pipeline:
 *  1. Scrape  https://www.nexusmods.com/baldursgate3/mods/{id}
 *     Parse <a class="sortme flag flag-{lang}"> links → list of translation mod IDs.
 *  2. For each translation mod ID, call the Nexus REST API to get file list
 *     and metadata (requires an API key).
 *  3. Emit translationsReady with files sorted by updatedTimestamp desc.
 *
 * If no API key is set, step 2 is skipped and only mod-level results
 * (modId, modPageUrl) are emitted without file-level detail.
 */
class LWizardNexusApi : public QObject
{
  Q_OBJECT
public:
  static constexpr const char* k_gameId        = "baldursgate3";
  static constexpr int         k_gameNumericId = 3474;

  explicit LWizardNexusApi(QObject* parent = nullptr);

  void    setApiKey(const QString& key);
  bool    hasApiKey() const;
  QString apiKey() const;

  /** Convert our language name to the Nexus flag slug used in HTML class names. */
  static QString toNexusLanguage(const QString& language);

  /**
   * Start an async search. requestId is echoed in signals for correlation.
   * nexusModId — Nexus mod ID of the *original* (untranslated) mod.
   * language   — our language name ("Russian", "German", …).
   */
  void searchTranslations(const QString& requestId, int nexusModId, const QString& language);

  /**
   * Queue a Nexus translation search triggered by a mod install event.
   * Same contract as scanModAsync in BG3LocalizationContent:
   *  - de-duplicates: if already queued or running, no-op
   *  - one search runs at a time; extras wait in a FIFO queue
   *  - never blocks the caller
   *  - emits translationsReady/searchError when done, same as searchTranslations()
   *
   * Must be called on the main thread (owns a QNetworkAccessManager).
   */
  void scanModAsync(const QString& modName, int nexusId, const QString& language);

  /** Cancel all in-flight requests and drain the queue. */
  void cancelAll();

signals:
  void translationsReady(const QString& requestId, const QList<NexusTranslationFile>& files);
  void searchError(const QString& requestId, const QString& error);
  void searchProgress(const QString& requestId, const QString& status);

private slots:
  void onModPageReply();
  void onFilesReply();

private:
  // ── Internal search helpers ───────────────────────────────────────────────
  QList<int> parseTranslationModIds(const QByteArray& html, const QString& nexusLang) const;
  void startFileFetch(const QString& reqId, const QList<int>& modIds, const QString& nexusLang);

  struct PendingSearch
  {
    QString                     requestId;
    QList<int>                  translationModIds;
    QString                     nexusLang;
    int                         pending = 0;
    QList<NexusTranslationFile> results;
    QMutex                      mutex;
  };

  void finishSearch(const QString& reqId, QList<NexusTranslationFile> files);

  // ── Auto-scan queue (same pattern as BG3LocalizationContent::scanModAsync) ──
  struct QueueEntry
  {
    QString modName;
    int     nexusId;
    QString language;
  };

  void startNextQueued();

  QList<QueueEntry> m_queue;
  QSet<QString>     m_queuePending; ///< mod names currently queued or running
  bool              m_queueRunning = false;
  QMutex            m_queueMutex;

  // ── Core state ────────────────────────────────────────────────────────────
  QString                m_apiKey;
  QNetworkAccessManager* m_nam;
  bool                   m_cancelled = false;

  // active in-flight searches keyed by requestId
  QMap<QString, PendingSearch*> m_searches;
};
