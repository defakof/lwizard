#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

#include <uibase/game_features/moddatacontent.h>

namespace MOBase {
class IOrganizer;
class IFileTree;
}  // namespace MOBase

/**
 * ModDataContent feature that detects BG3 localization status for each mod.
 *
 * Shows one of five icons in the MO2 Content column:
 *   0 - Embedded       : localization lives inside the mod's own pak
 *   1 - Installed      : a separate translation mod is installed
 *   2 - Available      : translation available on NexusMods (not installed)
 *   3 - Outdated       : translation installed but a newer version exists
 *   4 - Unavailable    : no localization found anywhere
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
  static constexpr int CONTENT_NONE        = -1;  // not yet scanned

  explicit BG3LocalizationContent(MOBase::IOrganizer* organizer);

  std::vector<Content> getAllContents() const override;
  std::vector<int>
  getContentsFor(std::shared_ptr<const MOBase::IFileTree> fileTree) const override;

  /** Invalidate the scan cache (call after refresh or language change). */
  void clearCache();

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
  };
  mutable QHash<QString, CacheEntry> m_cache;
  mutable QMutex                     m_cacheMutex;

  // Lazily resolved path to Divine.exe; empty = not found.
  mutable QString m_divinePath;
  mutable bool    m_divinePathResolved = false;
  mutable QMutex  m_divineMutex;

  QString resolvedDivinePath() const;
  QString currentLanguage() const;

  /** Returns CONTENT_EMBEDDED if localization is found, CONTENT_UNAVAILABLE otherwise. */
  int  detectContentId(std::shared_ptr<const MOBase::IFileTree> tree,
                       const QString& language) const;
  bool pakHasLocalization(const QString& pakPath, const QString& language) const;
};
