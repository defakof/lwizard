#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QVector>
#include <QSet>
#include <QString>
#include <QStringList>

#include <uibase/game_features/moddatacontent.h>

struct ScriptExtenderEnvironment
{
  bool updaterAvailable = false;
  bool rootBuilder      = false;
  int  currentVersion   = -1;
};

namespace MOBase {
class IOrganizer;
class IFileTree;
} // namespace MOBase

/**
 * ModDataContent feature that detects BG3 localization status for each mod.
 *
 * Shows icons in the MO2 Content column (IDs must match getAllContents()):
 *   0 - Embedded         : localization lives inside the mod's own pak (reference / original)
 *   1 - Installed        : reserved (Nexus pipeline)
 *   2 - Available        : translation available on NexusMods (not installed)
 *   3 - Outdated         : translation installed but a newer version exists
 *   4 - Unavailable      : no localization found for the scan language
 *   5 - Translation mod  : UUID overlap with another mod’s embedded loca (same strings, different
 * language)
 *
 * Icons appear only after an explicit scanAll() call. Before that the column
 * is empty for this feature.
 */
class BG3LocalizationContent : public QObject, public MOBase::ModDataContent
{
  Q_OBJECT
public:
  // Content column IDs — order must match getAllContents()
  static constexpr int CONTENT_EMBEDDED        = 0;
  static constexpr int CONTENT_INSTALLED       = 1;
  static constexpr int CONTENT_AVAILABLE       = 2;
  static constexpr int CONTENT_OUTDATED        = 3;
  static constexpr int CONTENT_UNAVAILABLE     = 4;
  static constexpr int CONTENT_TRANSLATION_MOD = 5;
  static constexpr int CONTENT_INVALID_UUID    = 6;
  static constexpr int CONTENT_MISSING_DEPS    = 7;
  static constexpr int CONTENT_OSIRIS_SCRIPTS  = 8;
  static constexpr int CONTENT_OSIRIS_MODFIXER = 9;
  static constexpr int CONTENT_SE_MISSING      = 10;
  static constexpr int CONTENT_SE_WARNING      = 11;
  static constexpr int CONTENT_SE_REQUIRED     = 12;
  static constexpr int CONTENT_SE_SUPPORTS     = 13;
  static constexpr int CONTENT_TOOLKIT_PROJECT = 14;
  static constexpr int CONTENT_NONE            = -1; // not yet scanned

  explicit BG3LocalizationContent(MOBase::IOrganizer* organizer);

  std::vector<Content> getAllContents() const override;
  std::vector<int> getContentsFor(std::shared_ptr<const MOBase::IFileTree> fileTree) const override;

  /**
   * Drop in-memory scan results (does not erase disk cache).
   * Prefer hydrateMemoryFromPersistent() after language change.
   */
  void clearCache();

  /** Clear all localization scan caches from memory and MO2 persistent storage. */
  bool clearAllCaches();

  /** Load memory cache from MO2 persistent storage for the current language. */
  void hydrateMemoryFromPersistent();

  /** Remove cache entries for mods that no longer exist in the mod list. */
  void pruneMissingModsFromCache();

  /**
   * Keep only the current plugin language in persistent storage; drops all other
   * languages from the on-disk cache (used when "cache only selected language" is on).
   */
  void prunePersistentCacheToCurrentLanguageOnly();

  /** Returns cached embedded UUID->string map for the current language, if present. */
  QMap<QString, QString> embeddedStringsFor(const QString& modName) const;

  /**
   * Synchronously extract UUID->string map for any language directly from the mod's
   * files/pak. Does NOT require a prior scanAll(). Uses the in-process PAK
   * reader for .loca files; call from a background thread for large mods.
   * Returns empty map if the mod has no localization for that language.
   */
  QMap<QString, QString> loadStringsSync(const QString& modName, const QString& language) const;

  /** Returns the paired base mod for a translation mod, or an empty string. */
  QString translationTargetFor(const QString& modName) const;

  /** Returns separate translation mods paired to a base mod. */
  QStringList separateTranslationsFor(const QString& modName) const;

  /** Returns direct linked counterparts for highlight pairing. */
  QStringList linkedModsFor(const QString& modName) const;

  /** Returns custom Content-column tooltip text for translation/base pairs. */
  QString contentTooltipFor(const QString& modName) const;

  bool                hasLinkedMods(const QString& modName) const;
  bool                translationStatusVisible() const;
  bool                extraContentStatusesVisible() const;
  bool                hasCriticalExtraStatus(const QString& modName) const;
  bool                hasToolkitStatus(const QString& modName) const;
  QHash<QString, int> extraHighlightKinds() const;

  /**
   * Mark a mod as having translations available on Nexus (updates content state
   * from CONTENT_UNAVAILABLE → CONTENT_AVAILABLE and stores translation mod IDs).
   * Thread-safe; emits contentCacheUpdated() on the main thread.
   */
  void markNexusAvailable(const QString& modName, const QList<int>& nexusTranslationModIds);

  /** Return stored Nexus translation mod IDs, or empty list if none known. */
  QList<int> nexusTranslationModIds(const QString& modName) const;

  void invalidateDerivedCaches();

  /**
   * Return all mod names that are currently CONTENT_UNAVAILABLE for the active language.
   * Used by the Nexus tab to queue discovery searches.
   */
  QStringList unavailableMods() const;

  /**
   * Scan all valid mods on a background thread.
   * Logs results via LWizardLog, emits scanFinished() when done.
   * Returns false if a scan is already running.
   */
  bool scanAll();

  /** Queue a background scan for a single newly installed mod. */
  void scanModAsync(const QString& modName);

signals:
  void contentCacheUpdated();
  void scanProgress(int done, int total, const QString& currentMod);
  void scanFinished();

private:
  MOBase::IOrganizer* m_organizer;
  std::atomic<bool>   m_scanning{false};

  struct ScanMetrics
  {
    qint64 fingerprintMs = 0;
    qint64 detectMs      = 0;
    qint64 uuidMs        = 0;
    qint64 matchMs       = 0;
    qint64 persistMs     = 0;
  };

  struct PakManifestCache
  {
    QHash<QString, QStringList> entriesByPakPath;
  };

public:
  enum class OsirisStatus
  {
    None,
    Scripts,
    ModFixer,
  };

  struct MetadataDependency
  {
    QString uuid;
    QString name;
  };

  struct ModMetadata
  {
    QString                   uuid;
    QString                   name;
    QString                   folder;
    QString                   type;
    QList<MetadataDependency> dependencies;
    QStringList               scriptExtenderFeatureFlags;
    int                       scriptExtenderRequiredVersion = -1;
    bool                      metadataKnown                 = false;
    bool                      invalidUuid                   = false;
    bool                      editorProject                 = false;
    bool                      scriptExtenderHasSettings     = false;
    OsirisStatus              osirisStatus                  = OsirisStatus::None;
  };

private:
  struct SingleModScanResult
  {
    QString       modName;
    QString       language;
    QString       modPath;
    QString       fingerprint;
    QString       translationTarget;
    QByteArray    embeddedStrings;
    QSet<QString> uuidKeys;
    ModMetadata   metadata;
    int           baseContentId  = CONTENT_NONE;
    int           finalContentId = CONTENT_NONE;
    bool          valid          = false;
    bool          cacheHit       = false;
  };

  struct CacheEntry
  {
    QString language;
    int     contentId = CONTENT_NONE;
    /** Fingerprint of on-disk files relevant to localization; empty = unknown. */
    QString     fingerprint;
    QString     translationTarget;
    QStringList separateTranslations;
    bool        relationshipsKnown = false;
    ModMetadata metadata;
    /** Nexus Mods translation mod IDs discovered by the Nexus tab scanner. */
    QList<int> nexusTranslationModIds;
  };
  mutable QHash<QString, CacheEntry>   m_cache;
  mutable QMutex                       m_cacheMutex;
  mutable std::optional<QSet<QString>> m_activeMetadataUuidsCache;
  mutable std::optional<ScriptExtenderEnvironment>
      m_scriptExtenderEnvironmentCache;

  mutable QMutex m_autoScanMutex;
  QStringList    m_autoScanQueue;
  QSet<QString>  m_autoScanPending;
  bool           m_autoScanRunning = false;

  QString       currentLanguage() const;
  bool          cacheOnlyCurrentLanguage() const;
  QStringList   validModNames() const;
  QStringList   missingDependencyNames(const CacheEntry& entry) const;
  QStringList   missingDependencyNames(const CacheEntry&    entry,
                                       const QSet<QString>& activeUuids) const;
  QSet<QString> activeMetadataUuids() const;
  QVector<int>  extraContentIdsFor(const CacheEntry& entry) const;

  /** Returns CONTENT_EMBEDDED if localization is found, CONTENT_UNAVAILABLE otherwise. */
  int  detectContentId(std::shared_ptr<const MOBase::IFileTree> tree,
                       const QString&                           language,
                       PakManifestCache*                        pakManifestCache = nullptr,
                       ScanMetrics*                             metrics          = nullptr) const;
  bool pakHasLocalization(const QString&    pakPath,
                          const QString&    language,
                          PakManifestCache* pakManifestCache = nullptr,
                          ScanMetrics*      metrics          = nullptr) const;

  void        savePersistentFromMemory();
  void        savePersistentFromMemory(const QHash<QString, CacheEntry>& entries);
  void        filterModsJsonToSingleLanguage(QJsonObject& mods, const QString& keepLang) const;
  QString     localizationFingerprint(const QString& modAbsPath) const;
  ModMetadata readModMetadata(const QString&    modAbsPath,
                              const QString&    modName,
                              PakManifestCache* pakManifestCache = nullptr,
                              ScanMetrics*      metrics          = nullptr) const;
  QString     scriptExtenderSupportToolTipText(const CacheEntry& entry) const;
  int         scriptExtenderContentId(const CacheEntry& entry) const;

  // Embedded strings cache (UUID -> string), persisted separately from icon cache.
  QByteArray loadEmbeddedStringsBlob(const QString& modName, const QString& lang) const;
  void       saveEmbeddedStringsBlob(const QString&    modName,
                                     const QString&    lang,
                                     const QString&    modPath,
                                     const QString&    fingerprint,
                                     const QByteArray& compressedJson);
  void       saveEmbeddedStringsBlobs(const QHash<QString, QByteArray>& compressedJsonByMod,
                                      const QHash<QString, QString>&    modPathByMod,
                                      const QHash<QString, QString>&    fingerprintByMod,
                                      const QString&                    lang);
  QMap<QString, QString> parseEmbeddedStringsCompressedJson(const QByteArray& compressedJson) const;

  QByteArray buildEmbeddedStringsForMod(const QString&    modAbsPath,
                                        const QString&    lang,
                                        PakManifestCache* pakManifestCache = nullptr,
                                        ScanMetrics*      metrics          = nullptr) const;
  QByteArray locaFileToJsonMapCompressed(const QString& locaAbsPath,
                                         ScanMetrics*   metrics = nullptr) const;
  QByteArray mergeJsonMapsCompressed(const QList<QByteArray>& compressedJsonMaps) const;

  QStringList listPakFileEntries(const QString&    pakPath,
                                 PakManifestCache* pakManifestCache = nullptr,
                                 ScanMetrics*      metrics          = nullptr) const;
  void        collectPakPathsUnderMod(const QString& modAbsPath, QStringList* outPaks) const;

  QSet<QString>                 uuidKeysFromCompressed(const QByteArray& compressedJson) const;
  QHash<QString, QSet<QString>> preloadPersistentUuidKeys() const;
  QSet<QString>                 discoverLanguagesInMod(const QString&    modAbsPath,
                                                       PakManifestCache* pakManifestCache = nullptr,
                                                       ScanMetrics*      metrics = nullptr) const;
  QSet<QString> uuidKeysUnionAllLanguages(const QString&    modAbsPath,
                                          PakManifestCache* pakManifestCache = nullptr,
                                          ScanMetrics*      metrics          = nullptr) const;

  /**
   * UUID overlap vs another mod: returns (contentId, matchedRefModName).
   * Translation mod only if this mod embeds the current scan language (baseContentId==EMBEDDED);
   * otherwise overlap → CONTENT_INSTALLED (other-language / redundant for scan).
   */
  QPair<int, QString> applyTranslationModClassification(
      const QString&                       modName,
      int                                  baseContentId,
      const QStringList&                   allModNames,
      const QHash<QString, int>&           baseIdThisScan,
      const QHash<QString, QSet<QString>>& uuidsThisScan,
      const QHash<QString, QSet<QString>>& persistentUuidsByMod = QHash<QString, QSet<QString>>(),
      const QHash<QString, QStringList>&   uuidIndexByUuid = QHash<QString, QStringList>()) const;

  SingleModScanResult computeSingleModScan(const QString& modName, const QString& language) const;
  void                applySingleModScanResult(const SingleModScanResult& result);
  void                finishAutoScan(const QString& modName);
  void                startNextQueuedAutoScan();

  QSet<QString> embeddedUuidKeysUnionFromPersistent(const QString& modName) const;
  CacheEntry    entryForCurrentLanguage(const QString& modName) const;
};
