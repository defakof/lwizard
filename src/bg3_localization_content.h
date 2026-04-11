#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <uibase/game_features/moddatacontent.h>

namespace MOBase {
class IOrganizer;
class IFileTree;
}  // namespace MOBase

/**
 * ModDataContent feature that detects BG3 localization status for each mod.
 *
 * Shows icons in the MO2 Content column (IDs must match getAllContents()):
 *   0 - Embedded         : localization lives inside the mod's own pak (reference / original)
 *   1 - Installed        : reserved (Nexus pipeline)
 *   2 - Available        : translation available on NexusMods (not installed)
 *   3 - Outdated         : translation installed but a newer version exists
 *   4 - Unavailable      : no localization found for the scan language
 *   5 - Translation mod  : UUID overlap with another mod’s embedded loca (same strings, different language)
 *
 * Icons appear only after an explicit scanAll() call. Before that the column
 * is empty for this feature.
 */
class BG3LocalizationContent : public QObject, public MOBase::ModDataContent
{
  Q_OBJECT
public:
  // Content column IDs — order must match getAllContents()
  static constexpr int CONTENT_EMBEDDED    = 0;
  static constexpr int CONTENT_INSTALLED   = 1;
  static constexpr int CONTENT_AVAILABLE   = 2;
  static constexpr int CONTENT_OUTDATED    = 3;
  static constexpr int CONTENT_UNAVAILABLE = 4;
  static constexpr int CONTENT_TRANSLATION_MOD = 5;
  static constexpr int CONTENT_NONE        = -1;  // not yet scanned

  explicit BG3LocalizationContent(MOBase::IOrganizer* organizer);

  std::vector<Content> getAllContents() const override;
  std::vector<int>
  getContentsFor(std::shared_ptr<const MOBase::IFileTree> fileTree) const override;

  /**
   * Drop in-memory scan results (does not erase disk cache).
   * Prefer hydrateMemoryFromPersistent() after language change.
   */
  void clearCache();

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

  /** Returns the paired base mod for a translation mod, or an empty string. */
  QString translationTargetFor(const QString& modName) const;

  /** Returns separate translation mods paired to a base mod. */
  QStringList separateTranslationsFor(const QString& modName) const;

  /** Returns direct linked counterparts for highlight pairing. */
  QStringList linkedModsFor(const QString& modName) const;

  /** Returns custom Content-column tooltip text for translation/base pairs. */
  QString contentTooltipFor(const QString& modName) const;

  bool hasLinkedMods(const QString& modName) const;

  /**
   * Scan all valid mods on a background thread.
   * Logs results via LWizardLog, emits scanFinished() when done.
   * Returns false if a scan is already running.
   */
  bool scanAll();

signals:
  void scanFinished();

private:
  MOBase::IOrganizer* m_organizer;
  std::atomic<bool>   m_scanning{false};

  struct CacheEntry
  {
    QString language;
    int     contentId = CONTENT_NONE;
    /** Fingerprint of on-disk files relevant to localization; empty = unknown. */
    QString fingerprint;
    QString translationTarget;
    QStringList separateTranslations;
    bool relationshipsKnown = false;
  };
  mutable QHash<QString, CacheEntry> m_cache;
  mutable QMutex                     m_cacheMutex;

  // Lazily resolved path to Divine.exe; empty = not found.
  mutable QString m_divinePath;
  mutable bool    m_divinePathResolved = false;
  mutable QMutex  m_divineMutex;

  QString resolvedDivinePath() const;
  QString currentLanguage() const;
  bool    cacheOnlyCurrentLanguage() const;

  /** Returns CONTENT_EMBEDDED if localization is found, CONTENT_UNAVAILABLE otherwise. */
  int  detectContentId(std::shared_ptr<const MOBase::IFileTree> tree,
                       const QString& language) const;
  bool pakHasLocalization(const QString& pakPath, const QString& language) const;

  void    savePersistentFromMemory();
  void    filterModsJsonToSingleLanguage(QJsonObject& mods, const QString& keepLang) const;
  QString localizationFingerprint(const QString& modAbsPath) const;

  // Embedded strings cache (UUID -> string), persisted separately from icon cache.
  QByteArray loadEmbeddedStringsBlob(const QString& modName, const QString& lang) const;
  void       saveEmbeddedStringsBlob(const QString& modName, const QString& lang,
                                     const QString& modPath, const QString& fingerprint,
                                     const QByteArray& compressedJson);
  QMap<QString, QString> parseEmbeddedStringsCompressedJson(const QByteArray& compressedJson) const;

  QByteArray buildEmbeddedStringsForMod(const QString& modAbsPath, const QString& lang) const;
  QByteArray locaFileToJsonMapCompressed(const QString& locaAbsPath) const;
  QByteArray mergeJsonMapsCompressed(const QList<QByteArray>& compressedJsonMaps) const;

  QStringList listPakFileEntries(const QString& pakPath) const;
  bool        extractPakEntryToFile(const QString& pakPath, const QString& packagedPath,
                                   const QString& destAbsFile) const;
  void        collectPakPathsUnderMod(const QString& modAbsPath, QStringList* outPaks) const;

  QSet<QString> uuidKeysFromCompressed(const QByteArray& compressedJson) const;
  QSet<QString> discoverLanguagesInMod(const QString& modAbsPath) const;
  QSet<QString> uuidKeysUnionAllLanguages(const QString& modAbsPath) const;

  /**
   * UUID overlap vs another mod: returns (contentId, matchedRefModName).
   * Translation mod only if this mod embeds the current scan language (baseContentId==EMBEDDED);
   * otherwise overlap → CONTENT_INSTALLED (other-language / redundant for scan).
   */
  QPair<int, QString> applyTranslationModClassification(
      const QString& modName, int baseContentId, const QStringList& allModNames,
      const QHash<QString, int>& baseIdThisScan,
      const QHash<QString, QSet<QString>>& uuidsThisScan) const;

  QSet<QString> embeddedUuidKeysUnionFromPersistent(const QString& modName) const;
  CacheEntry entryForCurrentLanguage(const QString& modName) const;
};
