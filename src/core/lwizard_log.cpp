#include "core/lwizard_log.h"

#include <QDateTime>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

LWizardLog& LWizardLog::instance()
{
  static LWizardLog s_instance;
  return s_instance;
}

void LWizardLog::debug(const QString& msg) { instance().append(QStringLiteral("DBG"), msg); }
void LWizardLog::info(const QString& msg)  { instance().append(QStringLiteral("INF"), msg); }
void LWizardLog::warn(const QString& msg)  { instance().append(QStringLiteral("WRN"), msg); }

void LWizardLog::append(const QString& level, const QString& msg)
{
  const QString entry =
      QStringLiteral("[%1 %2] %3")
          .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")), level,
               msg);

  {
    auto lock = QMutexLocker(&m_mutex);
    if (m_entries.size() >= k_maxEntries)
      m_entries.removeFirst();
    m_entries.append(entry);
  }

  // Emit signal on the main thread so UI widgets can connect safely
  if (QThread::currentThread() == thread()) {
    emit entryAdded(entry);
  } else {
    QMetaObject::invokeMethod(this, [this, entry]() { emit entryAdded(entry); },
                              Qt::QueuedConnection);
  }
}

QStringList LWizardLog::entries() const
{
  auto lock = QMutexLocker(&m_mutex);
  return m_entries;
}

void LWizardLog::clear()
{
  {
    auto lock = QMutexLocker(&m_mutex);
    m_entries.clear();
  }
  // no signal needed for clear — window reloads on open
}
