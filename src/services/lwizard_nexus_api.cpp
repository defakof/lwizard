#include "services/lwizard_nexus_api.h"

#include "core/lwizard_log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

// ---------------------------------------------------------------------------
// Language map
// ---------------------------------------------------------------------------

static const QMap<QString, QString> k_langToNexus = {
    {QStringLiteral("English"), QStringLiteral("English")},
    {QStringLiteral("French"), QStringLiteral("French")},
    {QStringLiteral("German"), QStringLiteral("German")},
    {QStringLiteral("Italian"), QStringLiteral("Italian")},
    {QStringLiteral("Spanish"), QStringLiteral("Spanish")},
    {QStringLiteral("Polish"), QStringLiteral("Polish")},
    {QStringLiteral("Russian"), QStringLiteral("Russian")},
    {QStringLiteral("ChineseSimplified"), QStringLiteral("Chinese")},
    {QStringLiteral("PortugueseBrazil"), QStringLiteral("Portugueseofbrazil")},
    {QStringLiteral("Turkish"), QStringLiteral("Turkish")},
    {QStringLiteral("Czech"), QStringLiteral("Czech")},
    {QStringLiteral("Ukrainian"), QStringLiteral("Ukrainian")},
    {QStringLiteral("Korean"), QStringLiteral("Korean")},
    {QStringLiteral("Japanese"), QStringLiteral("Japanese")},
};

/*static*/ QString LWizardNexusApi::toNexusLanguage(const QString& language)
{
  return k_langToNexus.value(language, language);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LWizardNexusApi::LWizardNexusApi(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{}

void LWizardNexusApi::setApiKey(const QString& key)
{
  m_apiKey = key.trimmed();
}
bool LWizardNexusApi::hasApiKey() const
{
  return !m_apiKey.isEmpty();
}
QString LWizardNexusApi::apiKey() const
{
  return m_apiKey;
}

void LWizardNexusApi::cancelAll()
{
  m_cancelled = true;
  m_nam->clearConnectionCache();
  qDeleteAll(m_searches);
  m_searches.clear();
  {
    QMutexLocker lk(&m_queueMutex);
    m_queue.clear();
    m_queuePending.clear();
    m_queueRunning = false;
  }
  m_cancelled = false;
}

// ---------------------------------------------------------------------------
// Auto-scan queue
// ---------------------------------------------------------------------------

void LWizardNexusApi::scanModAsync(const QString& modName, int nexusId, const QString& language)
{
  if (modName.isEmpty() || nexusId <= 0)
    return;

  int queuedCount = 0;
  {
    QMutexLocker lk(&m_queueMutex);
    if (m_queuePending.contains(modName)) {
      LWizardLog::debug(QStringLiteral("Nexus: auto-scan already queued for '%1'").arg(modName));
      return;
    }
    m_queuePending.insert(modName);
    m_queue.append({modName, nexusId, language});
    queuedCount = m_queue.size();
  }

  LWizardLog::info(QStringLiteral("Nexus: queued auto-scan for '%1' (Nexus #%2, queue depth: %3)")
                       .arg(modName)
                       .arg(nexusId)
                       .arg(queuedCount));

  // Kick the queue on the main thread (this method may be called from any context)
  QMetaObject::invokeMethod(
      this,
      [this]() {
        startNextQueued();
      },
      Qt::QueuedConnection);
}

void LWizardNexusApi::startNextQueued()
{
  QueueEntry entry;
  {
    QMutexLocker lk(&m_queueMutex);
    if (m_queueRunning || m_queue.isEmpty())
      return;
    entry          = m_queue.takeFirst();
    m_queueRunning = true;
  }

  LWizardLog::info(QStringLiteral("Nexus: starting auto-scan for '%1' (Nexus #%2, lang: %3)")
                       .arg(entry.modName)
                       .arg(entry.nexusId)
                       .arg(entry.language));

  // searchTranslations uses modName as requestId — same key used to drain queue
  searchTranslations(entry.modName, entry.nexusId, entry.language);
}

// ---------------------------------------------------------------------------
// Public: start search
// ---------------------------------------------------------------------------

void LWizardNexusApi::searchTranslations(const QString& requestId,
                                         int            nexusModId,
                                         const QString& language)
{
  if (nexusModId <= 0) {
    emit searchError(requestId, tr("Mod has no Nexus ID."));
    return;
  }

  const QString nexusLang = toNexusLanguage(language);
  const QString url       = QStringLiteral("https://www.nexusmods.com/") + QLatin1String(k_gameId) +
                      QStringLiteral("/mods/%1").arg(nexusModId);

  LWizardLog::info(
      QStringLiteral("Nexus: scraping %1 for %2 translations").arg(nexusModId).arg(nexusLang));

  QNetworkRequest req;
  req.setUrl(QUrl(url));
  // Use a realistic browser UA to avoid Cloudflare blocks
  req.setRawHeader(QByteArrayLiteral("User-Agent"),
                   QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                                     "Chrome/124.0.0.0 Safari/537.36"));
  req.setRawHeader(QByteArrayLiteral("Accept"),
                   QByteArrayLiteral("text/html,application/xhtml+xml"));
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::NoLessSafeRedirectPolicy);

  // Store search context on the reply as properties
  QNetworkReply* reply = m_nam->get(req);
  reply->setProperty("requestId", requestId);
  reply->setProperty("nexusLang", nexusLang);
  connect(reply, &QNetworkReply::finished, this, &LWizardNexusApi::onModPageReply);

  emit searchProgress(requestId, tr("Fetching mod page…"));
}

// ---------------------------------------------------------------------------
// Slot: mod page HTML received
// ---------------------------------------------------------------------------

void LWizardNexusApi::onModPageReply()
{
  auto* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply)
    return;
  reply->deleteLater();

  const QString reqId     = reply->property("requestId").toString();
  const QString nexusLang = reply->property("nexusLang").toString();

  if (m_cancelled)
    return;

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply->error() != QNetworkReply::NoError || status == 403 || status == 429) {
    const QString msg = reply->error() != QNetworkReply::NoError
                            ? reply->errorString()
                            : QStringLiteral("HTTP %1 — Nexus blocked the request. "
                                             "Log in to Nexus first or try again later.")
                                  .arg(status);
    LWizardLog::warn(QStringLiteral("Nexus scrape failed (%1): %2").arg(reqId, msg));
    emit searchError(reqId, msg);
    // Release queue runner and continue with next entry
    {
      QMutexLocker lk(&m_queueMutex);
      if (m_queueRunning) {
        m_queuePending.remove(reqId);
        m_queueRunning = false;
      }
    }
    startNextQueued();
    return;
  }

  const QByteArray html   = reply->readAll();
  const QList<int> modIds = parseTranslationModIds(html, nexusLang);

  if (modIds.isEmpty()) {
    LWizardLog::info(
        QStringLiteral("Nexus: no %1 translations found for request %2").arg(nexusLang, reqId));
    finishSearch(reqId, {});
    return;
  }

  LWizardLog::info(QStringLiteral("Nexus: found %1 translation mod(s) for request %2")
                       .arg(modIds.size())
                       .arg(reqId));

  if (!hasApiKey()) {
    // No API key: return stub entries with only modPageUrl
    QList<NexusTranslationFile> stubs;
    for (int modId : modIds) {
      NexusTranslationFile f;
      f.modId      = modId;
      f.modPageUrl = QStringLiteral("https://www.nexusmods.com/") + QLatin1String(k_gameId) +
                     QStringLiteral("/mods/%1").arg(modId);
      stubs.append(f);
    }
    finishSearch(reqId, stubs);
    return;
  }

  // Fetch file lists from API for each translation mod
  emit searchProgress(reqId, tr("Fetching file lists (%1 mods)…").arg(modIds.size()));
  startFileFetch(reqId, modIds, nexusLang);
}

// ---------------------------------------------------------------------------
// HTML parsing
// ---------------------------------------------------------------------------
//
// SSE-AT approach (Python/BeautifulSoup):
//   1. parsed.find("ul", {"class": "translations"})
//      → language names in <li> children  (e.g. "Russian")
//   2. parsed.find_all("a", {"class": f"sortme flag flag-{lang_text}"})
//      → href URLs; extract mod ID from each
//
// We replicate this with two targeted regex passes rather than a full HTML
// parser. The key insight from SSE-AT: search for flag-{lang} anywhere in an
// <a> tag's attributes, then extract the mod ID from the href on the same tag.
// Attribute order (href-before-class or class-before-href) must not matter.
// ---------------------------------------------------------------------------

QList<int> LWizardNexusApi::parseTranslationModIds(const QByteArray& html,
                                                   const QString&    nexusLang) const
{
  const QString text = QString::fromUtf8(html);

  // ── Debug diagnostics ─────────────────────────────────────────────────────
  const bool hasTranslationsUl =
      text.contains(QStringLiteral("class=\"translations\""), Qt::CaseInsensitive) ||
      text.contains(QStringLiteral("class='translations'"), Qt::CaseInsensitive);
  const QString flagToken    = QStringLiteral("flag-") + nexusLang;
  const bool    hasFlagClass = text.contains(flagToken, Qt::CaseInsensitive);

  LWizardLog::debug(
      QStringLiteral("Nexus HTML parse: translations_ul=%1  flag-%2=%3  html_bytes=%4")
          .arg(hasTranslationsUl)
          .arg(nexusLang)
          .arg(hasFlagClass)
          .arg(html.size()));

  if (!hasFlagClass)
    return {};

  // ── Pass 1: find every <a …> opening tag that has flag-{lang} in it ──────
  // [^>]* in a character-class negation always matches newlines, so this
  // handles multi-line attribute strings without extra flags.
  const QRegularExpression reAnchor(QStringLiteral("<a\\b[^>]*\\bflag-") + nexusLang +
                                        QStringLiteral("\\b[^>]*>"),
                                    QRegularExpression::CaseInsensitiveOption);

  // ── Pass 2: extract mod ID from the href on the same tag ─────────────────
  static const QRegularExpression reModId(QStringLiteral("href\\s*=\\s*[\"'][^\"']*/mods/(\\d+)"),
                                          QRegularExpression::CaseInsensitiveOption);

  QList<int> found;
  QSet<int>  seen;

  auto it = reAnchor.globalMatch(text);
  while (it.hasNext()) {
    const QString anchorTag = it.next().captured(0);

    const auto modMatch = reModId.match(anchorTag);
    if (!modMatch.hasMatch())
      continue;

    const int modId = modMatch.captured(1).toInt();
    if (modId > 0 && !seen.contains(modId)) {
      seen.insert(modId);
      found.append(modId);
      LWizardLog::debug(QStringLiteral("Nexus: found translation mod %1 in tag: %2")
                            .arg(modId)
                            .arg(anchorTag.left(120)));
    }
  }

  return found;
}

// ---------------------------------------------------------------------------
// File list fetch
// ---------------------------------------------------------------------------

void LWizardNexusApi::startFileFetch(const QString&    reqId,
                                     const QList<int>& modIds,
                                     const QString&    nexusLang)
{
  auto* search              = new PendingSearch;
  search->requestId         = reqId;
  search->translationModIds = modIds;
  search->nexusLang         = nexusLang;
  search->pending           = modIds.size();
  m_searches[reqId]         = search;

  for (int modId : modIds) {
    // Build URL via concatenation — avoids %%/arg() escaping issues where
    // Qt's .arg() greedily replaces %2 inside %%2C giving %{modId}C.
    const QString path =
        QStringLiteral("https://api.nexusmods.com/v1/games/") + QLatin1String(k_gameId) +
        QStringLiteral("/mods/") + QString::number(modId) +
        QStringLiteral(
            "/files.json?category=main%2Cupdate%2Coptional%2Cold_version%2Cmiscellaneous");

    QNetworkRequest req;
    req.setUrl(QUrl(path));
    req.setRawHeader(QByteArrayLiteral("accept"), QByteArrayLiteral("application/json"));
    req.setRawHeader(QByteArrayLiteral("apikey"), m_apiKey.toUtf8());
    req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("lwizard/1.0"));

    QNetworkReply* reply = m_nam->get(req);
    reply->setProperty("requestId", reqId);
    reply->setProperty("modId", modId);
    connect(reply, &QNetworkReply::finished, this, &LWizardNexusApi::onFilesReply);
  }
}

// ---------------------------------------------------------------------------
// Slot: files API reply
// ---------------------------------------------------------------------------

void LWizardNexusApi::onFilesReply()
{
  auto* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply)
    return;
  reply->deleteLater();

  const QString reqId = reply->property("requestId").toString();
  const int     modId = reply->property("modId").toInt();

  if (m_cancelled)
    return;

  PendingSearch* search = m_searches.value(reqId, nullptr);
  if (!search)
    return;

  bool                        finished = false;
  QList<NexusTranslationFile> done;

  {
    QMutexLocker lk(&search->mutex);

    if (reply->error() == QNetworkReply::NoError) {
      const QByteArray    body = reply->readAll();
      const QJsonDocument doc  = QJsonDocument::fromJson(body);

      // Parse mod name from a separate call or extract from files json
      QString          modName;
      const QJsonArray files = doc.object()[QStringLiteral("files")].toArray();

      // Try to extract mod name from file names — use the mod page URL as fallback
      const QString modPageUrl = QStringLiteral("https://www.nexusmods.com/") +
                                 QLatin1String(k_gameId) + QStringLiteral("/mods/%1").arg(modId);

      for (const QJsonValue& v : files) {
        const QJsonObject f   = v.toObject();
        const QString     cat = f[QStringLiteral("category_name")].toString();
        // Skip archived/deleted files
        if (cat.isEmpty() || cat == QStringLiteral("ARCHIVED"))
          continue;

        NexusTranslationFile tf;
        tf.modId            = modId;
        tf.fileId           = f[QStringLiteral("file_id")].toInt();
        tf.modName          = modName; // filled later by mod details if needed
        tf.fileDisplayName  = f[QStringLiteral("name")].toString();
        tf.fileName         = f[QStringLiteral("file_name")].toString();
        tf.version          = f[QStringLiteral("version")].toString();
        tf.updatedTimestamp = f[QStringLiteral("uploaded_timestamp")].toInteger();
        tf.sizeKb           = f[QStringLiteral("size_kb")].toInteger();
        tf.modPageUrl       = modPageUrl;
        tf.category         = cat;
        search->results.append(tf);
      }

      LWizardLog::debug(
          QStringLiteral("Nexus: got %1 file(s) for mod %2").arg(files.size()).arg(modId));
    } else {
      LWizardLog::warn(QStringLiteral("Nexus: files API failed for mod %1: %2")
                           .arg(modId)
                           .arg(reply->errorString()));
    }

    --search->pending;
    if (search->pending <= 0) {
      finished = true;
      done     = search->results;
    }
  }

  if (finished) {
    // Sort by updatedTimestamp descending (newest first)
    std::sort(
        done.begin(), done.end(), [](const NexusTranslationFile& a, const NexusTranslationFile& b) {
          return a.updatedTimestamp > b.updatedTimestamp;
        });

    finishSearch(reqId, done);
  }
}

void LWizardNexusApi::finishSearch(const QString& reqId, QList<NexusTranslationFile> files)
{
  delete m_searches.take(reqId);
  emit translationsReady(reqId, files);

  // If this search was a queued auto-scan, release the runner lock and
  // start the next entry — same pattern as BG3LocalizationContent::finishAutoScan.
  {
    QMutexLocker lk(&m_queueMutex);
    if (m_queueRunning) {
      m_queuePending.remove(reqId);
      m_queueRunning = false;
    }
  }
  startNextQueued();
}
