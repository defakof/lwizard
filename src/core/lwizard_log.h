#pragma once

#include <QMutex>
#include <QObject>
#include <QStringList>

/**
 * Thread-safe singleton log buffer for the lwizard plugin.
 * Use the static helpers debug/info/warn so all plugin code logs through here
 * instead of the global Qt / MO2 log.
 */
class LWizardLog : public QObject
{
  Q_OBJECT

public:
  static LWizardLog& instance();

  static void debug(const QString& msg);
  static void info(const QString& msg);
  static void warn(const QString& msg);

  /** Returns a snapshot of all stored entries. */
  QStringList entries() const;

  /** Remove all stored entries. */
  void clear();

signals:
  /** Emitted (on the main thread) whenever a new entry is appended. */
  void entryAdded(const QString& entry);

private:
  LWizardLog()                             = default;
  ~LWizardLog()                            = default;
  LWizardLog(const LWizardLog&)            = delete;
  LWizardLog& operator=(const LWizardLog&) = delete;

  void append(const QString& level, const QString& msg);

  mutable QMutex m_mutex;
  QStringList    m_entries;

  static constexpr int k_maxEntries = 2000;
};
