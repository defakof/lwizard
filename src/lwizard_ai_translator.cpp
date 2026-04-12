#include "lwizard_ai_translator.h"
#include "lwizard_log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

// ---------------------------------------------------------------------------
// Model roster
// ---------------------------------------------------------------------------

/*static*/ const QList<LWizardAiTranslator::ModelInfo>&
LWizardAiTranslator::availableModels()
{
  static const QList<ModelInfo> s_models = {
      {QStringLiteral("Gemini 3 Flash (5 RPM, best quality)"),
       QStringLiteral("gemini-3-flash-preview")},
      {QStringLiteral("Gemini 2.5 Flash (5 RPM, very strong)"),
       QStringLiteral("gemini-2.5-flash")},
      {QStringLiteral("Gemini 2.5 Flash Lite (10 RPM, good quality)"),
       QStringLiteral("gemini-2.5-flash-lite")},
      {QStringLiteral("Gemini 3.1 Flash Lite (15 RPM, fastest)"),
       QStringLiteral("gemini-3.1-flash-lite-preview")},
  };
  return s_models;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LWizardAiTranslator::LWizardAiTranslator(QObject* parent)
    : QObject(parent)
    , m_model(availableModels().constFirst().apiName)
    , m_nam(new QNetworkAccessManager(this))
{}

void LWizardAiTranslator::setApiKey(const QString& key) { m_apiKey = key.trimmed(); }
bool LWizardAiTranslator::hasApiKey() const             { return !m_apiKey.isEmpty(); }
void LWizardAiTranslator::setModel(const QString& apiName) { m_model = apiName; }
QString LWizardAiTranslator::model() const                 { return m_model; }

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void LWizardAiTranslator::translate(
    const QList<QPair<QString, QString>>& ordered,
    const QMap<QString, QString>& allOriginals,
    const QMap<QString, QString>& allTranslated,
    const QString& targetLang,
    const QString& sourceLang)
{
  m_cancelled   = false;
  m_batchIdx    = 0;
  m_doneItems   = 0;
  m_retries     = 0;
  m_totalItems  = ordered.size();
  m_allOriginals  = allOriginals;
  m_allTranslated = allTranslated;
  m_targetLang  = targetLang;
  m_sourceLang  = sourceLang;

  // Split into batches
  m_batches.clear();
  for (int i = 0; i < ordered.size(); i += kBatchSize) {
    m_batches.append(ordered.mid(i, kBatchSize));
  }

  if (m_batches.isEmpty()) {
    emit finished();
    return;
  }

  sendBatch(0);
}

void LWizardAiTranslator::cancel()
{
  m_cancelled = true;
  emit finished();
}

// ---------------------------------------------------------------------------
// Clipboard prompt / import
// ---------------------------------------------------------------------------

QString LWizardAiTranslator::buildClipboardPrompt(
    const QList<QPair<QString, QString>>& selected,
    const QMap<QString, QString>& allOriginals,
    const QMap<QString, QString>& allTranslated,
    const QString& targetLang,
    const QString& sourceLang) const
{
  // Temporarily set state so the private helpers work without a full translate() call
  const_cast<LWizardAiTranslator*>(this)->m_targetLang  = targetLang;
  const_cast<LWizardAiTranslator*>(this)->m_sourceLang  = sourceLang;
  const_cast<LWizardAiTranslator*>(this)->m_allOriginals  = allOriginals;
  const_cast<LWizardAiTranslator*>(this)->m_allTranslated = allTranslated;

  const QString sys  = buildSystemPrompt();
  const QString user = buildUserMessage(selected);

  // ── Assemble a single self-contained prompt ───────────────────────────────
  QString out;
  out += QStringLiteral(
      "════════════════════════════════════════════════════════════\n"
      "  BG3 MOD TRANSLATION REQUEST — DO NOT MODIFY THIS HEADER\n"
      "════════════════════════════════════════════════════════════\n\n");

  out += sys;

  out += QStringLiteral(
      "\n\n════════════════════════════════════════════════════════════\n"
      "  TRANSLATION TASK\n"
      "════════════════════════════════════════════════════════════\n\n");

  out += user;

  out += QStringLiteral(
      "\n\n════════════════════════════════════════════════════════════\n"
      "  MANDATORY RESPONSE FORMAT\n"
      "════════════════════════════════════════════════════════════\n"
      "Your ENTIRE response must be exactly this JSON — nothing before,\n"
      "nothing after, no markdown, no explanation:\n\n"
      "{\n"
      "  \"lwizard_translations\": {\n"
      "    \"<contentuid>\": \"<translated text>\",\n"
      "    \"<contentuid>\": \"<translated text>\"\n"
      "  }\n"
      "}\n\n"
      "• Every UUID from the STRINGS TO TRANSLATE section must appear.\n"
      "• Values are the translated strings with all markup preserved.\n"
      "• Do NOT wrap the JSON in ``` fences.\n"
      "• Do NOT add any text outside the JSON object.\n"
      "════════════════════════════════════════════════════════════\n");

  return out;
}

/*static*/ QMap<QString, QString>
LWizardAiTranslator::importFromClipboardText(const QString& text)
{
  // Helper: parse the JSON object and look for our envelope key
  auto tryParse = [](const QString& s) -> QMap<QString, QString> {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
      return {};

    const QJsonObject root = doc.object();

    // Accept both "lwizard_translations" (our key) and "translations" (generic)
    QJsonObject trans;
    if (root.contains(QStringLiteral("lwizard_translations")))
      trans = root[QStringLiteral("lwizard_translations")].toObject();
    else if (root.contains(QStringLiteral("translations")))
      trans = root[QStringLiteral("translations")].toObject();

    if (trans.isEmpty())
      return {};

    QMap<QString, QString> out;
    for (auto it = trans.begin(); it != trans.end(); ++it)
      out[it.key()] = it.value().toString();
    return out;
  };

  // Level 1: entire text is the JSON object
  {
    auto r = tryParse(text.trimmed());
    if (!r.isEmpty()) return r;
  }

  // Level 2: ```json ... ``` or ``` ... ``` fence
  {
    static const QRegularExpression fenceRe(
        QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)```"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = fenceRe.match(text);
    if (m.hasMatch()) {
      auto r = tryParse(m.captured(1).trimmed());
      if (!r.isEmpty()) return r;
    }
  }

  // Level 3: outermost { ... }
  {
    const int first = text.indexOf(QChar('{'));
    const int last  = text.lastIndexOf(QChar('}'));
    if (first != -1 && last > first) {
      auto r = tryParse(text.mid(first, last - first + 1));
      if (!r.isEmpty()) return r;
    }
  }

  return {};
}

// ---------------------------------------------------------------------------
// Prompt building
// ---------------------------------------------------------------------------

QString LWizardAiTranslator::buildSystemPrompt() const
{
  const QString lang = m_targetLang;

  return QStringLiteral(
R"(You are a professional video game localizer specializing in Baldur's Gate 3 (BG3) — a D&D 5th Edition RPG by Larian Studios.

TARGET LANGUAGE: )") + lang + QStringLiteral(R"(
SOURCE LANGUAGE: )") + m_sourceLang + QStringLiteral(R"(

══════════════════════════════════════════════════════════
MARKUP PRESERVATION — ABSOLUTE REQUIREMENT
══════════════════════════════════════════════════════════
BG3 strings use custom XML-like markup. You MUST keep every tag byte-perfect:

• <LSTag Type="X" Tooltip="Y">visible text</LSTag>
  → Keep the opening and closing LSTag exactly. Translate ONLY the inner visible text.
• <br>  → keep as-is (line break)
• <b>, </b>, <i>, </i>, <s>, </s>, <u>, </u>  → keep as-is (formatting)
• [1], [2], [3] …  → numeric placeholders for game values, keep as-is
• Attribute values inside tags (Type=, Tooltip=) are internal IDs — NEVER translate them

Correct example:
  Input:  "You gain <LSTag Tooltip="Resistant">Resistance</LSTag> to Psychic damage, and you have <LSTag Tooltip="Advantage">Advantage</LSTag> on <LSTag Tooltip="SavingThrow">Saving Throws</LSTag>."
  Output: "Вы получаете <LSTag Tooltip="Resistant">сопротивление</LSTag> к психическому урону и <LSTag Tooltip="Advantage">преимущество</LSTag> при прохождении <LSTag Tooltip="SavingThrow">испытаний</LSTag>."

══════════════════════════════════════════════════════════
D&D 5E CORE TERMINOLOGY
══════════════════════════════════════════════════════════
Use official D&D 5e localization terminology for the target language.
For Russian (Hobby World / Wizards of the Coast official translations):

  Saving Throw          → испытание
  Spell Slot            → ячейка заклинания
  Advantage             → преимущество
  Disadvantage          → помеха
  Action                → действие
  Bonus Action          → бонусное действие
  Reaction              → реакция
  Movement / Move       → передвижение / пройти
  Hit Points / HP       → очки здоровья
  Armor Class / AC      → класс доспеха
  Attack Roll           → бросок атаки
  Ability Check         → проверка характеристики
  Skill Check           → проверка навыка
  Concentration         → концентрация
  Proficiency           → владение
  Proficiency Bonus     → бонус мастерства
  Short Rest            → короткий отдых
  Long Rest             → долгий отдых
  Turn                  → ход
  Round                 → раунд
  Prone                 → лежащий / пал плашмя
  Frightened            → испуг
  Charmed               → очарован
  Silenced              → немота / немой
  Stunned               → оглушён
  Paralyzed             → парализован
  Invisible             → невидимость
  Resistance            → сопротивление
  Immunity              → иммунитет
  Vulnerability         → уязвимость
  Cantrip               → заговор
  Spell                 → заклинание
  Modifier              → модификатор
  Strength (STR)        → сила
  Dexterity (DEX)       → ловкость
  Constitution (CON)    → телосложение
  Intelligence (INT)    → интеллект
  Wisdom (WIS)          → мудрость
  Charisma (CHA)        → харизма

══════════════════════════════════════════════════════════
BG3 CLASS & SUBCLASS TERMINOLOGY
══════════════════════════════════════════════════════════
  Fighter      → воин           Cleric     → жрец
  Wizard       → волшебник      Rogue      → плут
  Bard         → бард           Druid      → друид
  Ranger       → следопыт       Paladin    → паладин
  Monk         → монах          Sorcerer   → чародей
  Warlock      → колдун         Barbarian  → варвар
  Domain       → домен          Circle     → круг
  Path         → путь           College    → коллегия
  Oath         → клятва         Archetype  → архетип

══════════════════════════════════════════════════════════
BG3 COMMON MECHANICS
══════════════════════════════════════════════════════════
  Aura                → аура
  Status Effect       → эффект состояния / статус
  Passive             → пассивная способность
  Feature             → умение / способность
  Cantrip             → заговор
  Spell Scroll        → свиток заклинания
  Wild Shape          → дикий облик
  Eldritch Invocation → мистическое воззвание
  Sneak Attack        → скрытая атака
  Bardic Inspiration  → бардовское вдохновение
  Rage                → ярость
  Smite               → кара
  Lay on Hands        → наложение рук
  Second Wind         → второе дыхание
  Action Surge        → всплеск действий
  Flurry of Blows     → шквал ударов
  Stunning Strike     → оглушающий удар
  Ki                  → ки
  Spell Attack        → атака заклинанием
  Spell Save DC       → сл заклинания

══════════════════════════════════════════════════════════
CONSISTENCY — THE MOST IMPORTANT RULE
══════════════════════════════════════════════════════════
You will receive:
  A) A CONSISTENCY GLOSSARY — terms found in previously translated strings.
     YOU MUST USE THESE EXACT TRANSLATIONS — no synonyms, no alternatives.
  B) ALREADY TRANSLATED STRINGS — full pairs for rich context.

If the same English phrase appears in a new string, use the EXACT same translation
that appears in the glossary or context. This is mandatory.

For proper noun names (spell names, status names, feature names) that appear
multiple times: always use one consistent translation throughout.

══════════════════════════════════════════════════════════
TRANSLATION QUALITY GUIDELINES
══════════════════════════════════════════════════════════
• Use high-fantasy register: formal, dramatic, immersive
• Short tooltip labels: concise noun phrases (no articles if unnatural)
• Full descriptions: proper sentences, natural flow
• Preserve the TONE of the original: threatening, poetic, clinical, etc.
• Do NOT add explanations or translator notes
• Do NOT transliterate — always use native vocabulary where it exists
• Numbers and symbol characters ([1], %, +, –) stay unchanged
)");
}

QString LWizardAiTranslator::buildUserMessage(
    const QList<QPair<QString, QString>>& batch) const
{
  QString msg;

  // ── Consistency glossary ──────────────────────────────────────────────────
  // Build a simple glossary by finding short phrases (1-3 words) from batch
  // originals that appear in the existing translations.
  // We pass up to kMaxContextPairs context pairs; the model extracts terms.

  msg += QStringLiteral("═══════════════════════════════\n"
                        "ALREADY TRANSLATED STRINGS (consistency context)\n"
                        "═══════════════════════════════\n");

  int ctxCount = 0;
  // Iterate in the same order the map was built (insertion order not guaranteed,
  // but we just want a representative sample — favour shorter strings first).
  QList<QPair<QString,QString>> ctxPairs;
  for (auto it = m_allTranslated.constBegin();
       it != m_allTranslated.constEnd() && ctxCount < kMaxContextPairs;
       ++it, ++ctxCount)
  {
    const QString& orig  = m_allOriginals.value(it.key());
    const QString& trans = it.value();
    if (!orig.isEmpty() && !trans.isEmpty())
      ctxPairs.append({orig, trans});
  }
  // Sort by original length so shorter (=term-like) strings appear first
  std::sort(ctxPairs.begin(), ctxPairs.end(),
            [](const QPair<QString,QString>& a, const QPair<QString,QString>& b){
              return a.first.length() < b.first.length();
            });
  for (const auto& p : ctxPairs)
    msg += QStringLiteral("  [EN] %1\n  [%2] %3\n\n")
               .arg(p.first, m_targetLang, p.second);

  if (ctxPairs.isEmpty())
    msg += QStringLiteral("  (none yet — you are the first batch)\n\n");

  // ── Strings to translate ──────────────────────────────────────────────────
  msg += QStringLiteral("═══════════════════════════════\n"
                        "STRINGS TO TRANSLATE\n"
                        "═══════════════════════════════\n");

  QJsonObject stringsObj;
  for (const auto& [uuid, orig] : batch)
    stringsObj[uuid] = orig;

  msg += QJsonDocument(stringsObj).toJson(QJsonDocument::Indented);

  msg += QStringLiteral(
      "\n\n═══════════════════════════════\n"
      "RESPONSE FORMAT\n"
      "═══════════════════════════════\n"
      "Return ONLY this JSON object — no markdown fences, no explanation:\n"
      "{\n"
      "  \"lwizard_translations\": {\n"
      "    \"<contentuid>\": \"<translated text>\",\n"
      "    ...\n"
      "  }\n"
      "}\n");

  return msg;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

QByteArray LWizardAiTranslator::buildRequestBody(
    const QList<QPair<QString, QString>>& batch) const
{
  // System instruction
  QJsonObject sysText;
  sysText[QStringLiteral("text")] = buildSystemPrompt();
  QJsonObject sysInstruction;
  sysInstruction[QStringLiteral("parts")] = QJsonArray{sysText};

  // User turn
  QJsonObject userText;
  userText[QStringLiteral("text")] = buildUserMessage(batch);
  QJsonObject userContent;
  userContent[QStringLiteral("role")]  = QStringLiteral("user");
  userContent[QStringLiteral("parts")] = QJsonArray{userText};

  // Generation config — low temperature for deterministic translation
  QJsonObject genCfg;
  genCfg[QStringLiteral("temperature")]     = 0.05;
  genCfg[QStringLiteral("maxOutputTokens")] = 8192;

  // Google Search grounding tool
  QJsonObject googleSearch;
  QJsonObject searchTool;
  searchTool[QStringLiteral("google_search")] = googleSearch;

  QJsonObject body;
  body[QStringLiteral("system_instruction")] = sysInstruction;
  body[QStringLiteral("contents")]           = QJsonArray{userContent};
  body[QStringLiteral("tools")]              = QJsonArray{searchTool};
  body[QStringLiteral("generationConfig")]   = genCfg;

  return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void LWizardAiTranslator::sendBatch(int idx)
{
  if (m_cancelled || idx >= m_batches.size()) {
    emit finished();
    return;
  }

  const QString urlStr =
      QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/") +
      m_model +
      QStringLiteral(":generateContent?key=") +
      m_apiKey;

  QNetworkRequest req;
  req.setUrl(QUrl(urlStr));
  req.setRawHeader(QByteArrayLiteral("Content-Type"),
                   QByteArrayLiteral("application/json"));

  QByteArray body = buildRequestBody(m_batches.at(idx));
  LWizardLog::debug(
      QStringLiteral("AI translate: sending batch %1/%2 (%3 strings)")
          .arg(idx + 1).arg(m_batches.size()).arg(m_batches.at(idx).size()));

  QNetworkReply* reply = m_nam->post(req, body);
  connect(reply, &QNetworkReply::finished, this,
          &LWizardAiTranslator::onReplyFinished);
}

// ---------------------------------------------------------------------------
// Retry helper
// ---------------------------------------------------------------------------

void LWizardAiTranslator::retryBatchAfter(int seconds)
{
  const int ms = qMax(seconds, 1) * 1000 + 500; // +0.5s cushion
  LWizardLog::info(QStringLiteral("AI translate: rate-limited, retrying batch %1 in %2s")
                       .arg(m_batchIdx + 1).arg(seconds));
  emit rateLimited(seconds);
  QTimer::singleShot(ms, this, [this]() {
    if (!m_cancelled)
      sendBatch(m_batchIdx);
  });
}

/*static*/ int LWizardAiTranslator::parseRetryAfter(const QString& msg)
{
  // Gemini format: "Please retry in 14.969702028s."
  static const QRegularExpression re(QStringLiteral("retry in (\\d+(?:\\.\\d+)?)s"),
                                     QRegularExpression::CaseInsensitiveOption);
  const auto m = re.match(msg);
  if (m.hasMatch())
    return qMax(1, static_cast<int>(m.captured(1).toDouble()) + 1);
  return 15; // safe default
}

// ---------------------------------------------------------------------------
// Response handling
// ---------------------------------------------------------------------------

void LWizardAiTranslator::onReplyFinished()
{
  auto* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) return;
  reply->deleteLater();

  if (m_cancelled) {
    emit finished();
    return;
  }

  const QByteArray body = reply->readAll();

  if (reply->error() != QNetworkReply::NoError) {
    // Parse Gemini error body
    QString detail;
    int     httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
      detail = doc.object()[QStringLiteral("error")]
                   .toObject()[QStringLiteral("message")]
                   .toString();
    }

    // Rate limit → retry
    if (httpStatus == 429 && m_retries < kMaxRetries) {
      ++m_retries;
      retryBatchAfter(parseRetryAfter(detail));
      return;
    }

    const QString errMsg = detail.isEmpty()
        ? QStringLiteral("Network error %1: %2").arg(reply->error()).arg(reply->errorString())
        : QStringLiteral("Gemini API error: ") + detail;

    LWizardLog::warn(QStringLiteral("AI translate: ") + errMsg);
    emit error(errMsg);
    emit finished();
    return;
  }

  m_retries = 0; // successful reply — reset retry counter

  const QMap<QString, QString> results = parseResponse(body);

  if (results.isEmpty()) {
    LWizardLog::warn(QStringLiteral("AI translate: batch %1 — empty or unparseable response")
                         .arg(m_batchIdx + 1));
    // Emit error but continue remaining batches
    emit error(QStringLiteral("Batch %1: could not parse translation response. "
                              "Check Logs tab for details.")
                   .arg(m_batchIdx + 1));
  } else {
    // Merge results into running context so subsequent batches see them
    for (auto it = results.constBegin(); it != results.constEnd(); ++it)
      m_allTranslated[it.key()] = it.value();

    m_doneItems += m_batches.at(m_batchIdx).size();
    emit batchDone(results);
    emit progress(m_doneItems, m_totalItems);
    LWizardLog::info(
        QStringLiteral("AI translate: batch %1/%2 done — %3 strings")
            .arg(m_batchIdx + 1).arg(m_batches.size()).arg(results.size()));
  }

  ++m_batchIdx;
  if (m_batchIdx >= m_batches.size())
    emit finished();
  else
    sendBatch(m_batchIdx);
}

// ---------------------------------------------------------------------------
// JSON extraction — three-level fallback
// ---------------------------------------------------------------------------

QMap<QString, QString> LWizardAiTranslator::parseResponse(const QByteArray& body) const
{
  // 1. Unwrap Gemini response envelope
  QJsonParseError pe;
  const QJsonDocument envelope = QJsonDocument::fromJson(body, &pe);
  if (pe.error != QJsonParseError::NoError) {
    LWizardLog::warn(QStringLiteral("AI translate: envelope parse error: ") +
                     pe.errorString());
    return {};
  }

  // candidates[0].content.parts[0].text
  const QJsonArray candidates =
      envelope.object()[QStringLiteral("candidates")].toArray();
  if (candidates.isEmpty()) {
    LWizardLog::warn(QStringLiteral("AI translate: no candidates in response"));
    LWizardLog::debug(QString::fromUtf8(body.left(400)));
    return {};
  }

  QString text;
  const QJsonArray parts =
      candidates[0].toObject()[QStringLiteral("content")]
          .toObject()[QStringLiteral("parts")]
          .toArray();
  for (const QJsonValue& part : parts) {
    const QString t = part.toObject()[QStringLiteral("text")].toString();
    if (!t.isEmpty()) { text = t; break; }
  }

  if (text.isEmpty()) {
    LWizardLog::warn(QStringLiteral("AI translate: empty text in response"));
    return {};
  }

  LWizardLog::debug(QStringLiteral("AI translate: raw text (first 300): ") +
                    text.left(300));

  return extractTranslationsJson(text);
}

/* static */ QMap<QString, QString>
LWizardAiTranslator::extractTranslationsJson(const QString& text)
{
  // Helper: try to parse JSON object and return "translations" sub-map
  auto tryParse = [](const QString& s) -> QMap<QString, QString> {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
      return {};
    const QJsonObject trans =
        doc.object()[QStringLiteral("translations")].toObject();
    if (trans.isEmpty())
      return {};
    QMap<QString, QString> out;
    for (auto it = trans.begin(); it != trans.end(); ++it)
      out[it.key()] = it.value().toString();
    return out;
  };

  // Level 1: entire text is JSON
  {
    auto r = tryParse(text);
    if (!r.isEmpty()) return r;
  }

  // Level 2: extract from ```json ... ``` or ``` ... ``` markdown fences
  {
    static const QRegularExpression fenceRe(
        QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)```"),
        QRegularExpression::CaseInsensitiveOption);
    auto m = fenceRe.match(text);
    if (m.hasMatch()) {
      auto r = tryParse(m.captured(1).trimmed());
      if (!r.isEmpty()) return r;
    }
  }

  // Level 3: find outermost { ... } in the text
  {
    const int first = text.indexOf(QChar('{'));
    const int last  = text.lastIndexOf(QChar('}'));
    if (first != -1 && last > first) {
      auto r = tryParse(text.mid(first, last - first + 1));
      if (!r.isEmpty()) return r;
    }
  }

  LWizardLog::warn(QStringLiteral("AI translate: JSON extraction failed. "
                                  "Raw text: ") + text.left(500));
  return {};
}
