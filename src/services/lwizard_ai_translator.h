#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/**
 * Gemini (Google AI Studio) translation client for BG3 mod strings.
 *
 * Model roster (free-tier quotas as of 2025-07):
 *   gemini-3-flash-preview      5 RPM  250K TPM  — best quality
 *   gemini-2.5-flash            5 RPM  250K TPM  — very strong, proven
 *   gemini-2.5-flash-lite      10 RPM  250K TPM  — good quality, faster
 *   gemini-3.1-flash-lite-preview 15 RPM 250K TPM — highest RPM
 *
 * Rate-limit handling:
 *   On HTTP 429 the API returns a Retry-After-style delay in the error body.
 *   We parse that value and schedule a retry via QTimer.  Max retries: kMaxRetries.
 */
class LWizardAiTranslator : public QObject
{
  Q_OBJECT

public:
  static constexpr int kBatchSize       = 12;
  static constexpr int kMaxContextPairs = 80;
  static constexpr int kMaxRetries      = 4;

  // Available models shown in the UI (display name → API name)
  struct ModelInfo
  {
    QString display;
    QString apiName;
  };
  static const QList<ModelInfo>& availableModels();

  explicit LWizardAiTranslator(QObject* parent = nullptr);

  void setApiKey(const QString& key);
  bool hasApiKey() const;

  /** Set which model to use (apiName from availableModels()). */
  void    setModel(const QString& apiName);
  QString model() const;

  void translate(const QList<QPair<QString, QString>>& ordered,
                 const QMap<QString, QString>&         allOriginals,
                 const QMap<QString, QString>&         allTranslated,
                 const QString&                        targetLang,
                 const QString&                        sourceLang = QStringLiteral("English"));

  /**
   * Build a self-contained clipboard prompt for ALL selected strings at once.
   * Designed to be pasted into any AI chat (Claude, ChatGPT, Gemini web, etc.).
   * The prompt instructs the AI to reply in a specific JSON envelope that
   * importFromClipboard() can parse directly.
   */
  QString buildClipboardPrompt(const QList<QPair<QString, QString>>& selected,
                               const QMap<QString, QString>&         allOriginals,
                               const QMap<QString, QString>&         allTranslated,
                               const QString&                        targetLang,
                               const QString& sourceLang = QStringLiteral("English")) const;

  /**
   * Parse a clipboard string that contains the AI's response.
   * Accepts raw JSON, ```json fenced blocks, or any text containing
   * the {"translations":{...}} envelope anywhere inside it.
   * Returns UUID→translatedText map; empty on failure.
   */
  static QMap<QString, QString> importFromClipboardText(const QString& text);

  void cancel();

signals:
  void batchDone(const QMap<QString, QString>& results);
  void progress(int done, int total);
  void finished();
  void error(const QString& message);
  /** Emitted when rate-limited so the UI can show a countdown. */
  void rateLimited(int retryAfterSeconds);

private slots:
  void onReplyFinished();

private:
  QString                m_apiKey;
  QString                m_model;
  QNetworkAccessManager* m_nam       = nullptr;
  bool                   m_cancelled = false;
  int                    m_retries   = 0;

  QList<QList<QPair<QString, QString>>> m_batches;
  int                                   m_batchIdx   = 0;
  int                                   m_doneItems  = 0;
  int                                   m_totalItems = 0;

  QMap<QString, QString> m_allOriginals;
  QMap<QString, QString> m_allTranslated;
  QString                m_targetLang;
  QString                m_sourceLang;

  void       sendBatch(int idx);
  void       retryBatchAfter(int seconds);
  QByteArray buildRequestBody(const QList<QPair<QString, QString>>& batch) const;
  QString    buildSystemPrompt() const;
  QString    buildUserMessage(const QList<QPair<QString, QString>>& batch) const;

  QMap<QString, QString>        parseResponse(const QByteArray& body) const;
  static QMap<QString, QString> extractTranslationsJson(const QString& text);

  // Parse "Please retry in X.Xs" from Gemini error body → seconds (0 = unknown)
  static int parseRetryAfter(const QString& errorMessage);
};
