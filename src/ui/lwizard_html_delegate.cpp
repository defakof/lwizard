#include "ui/lwizard_html_delegate.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QModelIndex>
#include <QPainter>
#include <QPlainTextEdit>
#include <QStyle>
#include <QStyleOption>
#include <QTextDocument>

HtmlItemDelegate::HtmlItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

// ---------------------------------------------------------------------------
// BG3 markup → Qt HTML
// ---------------------------------------------------------------------------

QString HtmlItemDelegate::toDisplayHtml(const QString& rawText)
{
  QString out;
  out.reserve(rawText.size() + 64);

  int pos = 0;
  const int len = rawText.length();

  while (pos < len) {
    const int tagStart = rawText.indexOf(QChar('<'), pos);
    if (tagStart == -1) {
      out += rawText.mid(pos).toHtmlEscaped();
      break;
    }

    // Plain text before this tag
    if (tagStart > pos)
      out += rawText.mid(pos, tagStart - pos).toHtmlEscaped();

    const int tagEnd = rawText.indexOf(QChar('>'), tagStart);
    if (tagEnd == -1) {
      // Unclosed tag — treat as plain text
      out += rawText.mid(tagStart).toHtmlEscaped();
      break;
    }

    const QString tag     = rawText.mid(tagStart, tagEnd - tagStart + 1);
    const QString tagLow  = tag.toLower();

    if (tagLow.startsWith(QStringLiteral("<lstag"))) {
      out += QStringLiteral("<span style=\"color:#7ab4d4\">");
    } else if (tagLow == QStringLiteral("</lstag>")) {
      out += QStringLiteral("</span>");
    } else if (tagLow == QStringLiteral("<br>") ||
               tagLow == QStringLiteral("<br/>") ||
               tagLow == QStringLiteral("<br />")) {
      out += QStringLiteral("<br/>");
    } else if (tagLow == QStringLiteral("<b>")  || tagLow == QStringLiteral("</b>") ||
               tagLow == QStringLiteral("<i>")  || tagLow == QStringLiteral("</i>") ||
               tagLow == QStringLiteral("<s>")  || tagLow == QStringLiteral("</s>") ||
               tagLow == QStringLiteral("<u>")  || tagLow == QStringLiteral("</u>")) {
      out += tag;
    }
    // Unknown tags: silently drop

    pos = tagEnd + 1;
  }

  return out;
}

// ---------------------------------------------------------------------------
// paint
// ---------------------------------------------------------------------------

void HtmlItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
  QStyleOptionViewItem opt = option;
  initStyleOption(&opt, index);

  const QWidget* widget = opt.widget;
  QStyle*        style  = widget ? widget->style() : QApplication::style();

  // While a cell editor is open Qt may still call paint() underneath it —
  // skip custom drawing entirely so the HTML doesn't bleed through the editor.
  if (index == m_editingIndex) {
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);
    return;
  }

  // Draw background / selection highlight
  style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

  const QString rawText = index.data(Qt::DisplayRole).toString();
  if (rawText.isEmpty())
    return;

  const bool selected = opt.state & QStyle::State_Selected;
  const QRect inner   = opt.rect.adjusted(k_hPad, k_vPad, -k_hPad, -k_vPad);

  if (index.column() == 0) {
    // UUID — plain elided text
    painter->save();
    painter->setFont(opt.font);
    painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
    const QString elided = opt.fontMetrics.elidedText(rawText, Qt::ElideMiddle, inner.width());
    painter->drawText(inner, Qt::AlignLeft | Qt::AlignVCenter, elided);
    painter->restore();
    return;
  }

  // HTML rendering for columns 1 and 2
  const QString html = toDisplayHtml(rawText);

  // For selected rows inject white text override so content is readable on the
  // highlight background; the LSTag spans will also be overridden.
  const QString bodyStyle = selected
      ? QStringLiteral("color:white;")
      : QString();

  QTextDocument doc;
  doc.setDefaultFont(opt.font);
  doc.setHtml(
      QStringLiteral("<html><body style='margin:0;padding:0;white-space:pre-wrap;%1'>")
          .arg(bodyStyle) +
      html +
      QStringLiteral("</body></html>"));
  doc.setTextWidth(inner.width());

  painter->save();
  painter->translate(inner.topLeft());
  const QRectF clip(0, 0, inner.width(), inner.height());
  painter->setClipRect(clip);
  doc.drawContents(painter, clip);
  painter->restore();
}

// ---------------------------------------------------------------------------
// sizeHint
// ---------------------------------------------------------------------------

QSize HtmlItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
  const QSize base = QStyledItemDelegate::sizeHint(option, index);

  if (index.column() == 0)
    return base;

  const QString rawText = index.data(Qt::DisplayRole).toString();
  if (rawText.isEmpty())
    return QSize(base.width(), k_minRowHeight);

  const QString html = toDisplayHtml(rawText);

  QTextDocument doc;
  doc.setDefaultFont(option.font);
  doc.setHtml(html);
  // Use a reasonable column width estimate for height calculation
  doc.setTextWidth(qMax(option.rect.width() - 2 * k_hPad, 200));

  const int h = qBound(k_minRowHeight,
                       static_cast<int>(doc.size().height()) + 2 * k_vPad,
                       k_maxRowHeight);
  return QSize(base.width(), h);
}

// ---------------------------------------------------------------------------
// Editor (column 2 only)
// ---------------------------------------------------------------------------

QWidget* HtmlItemDelegate::createEditor(QWidget* parent,
                                        const QStyleOptionViewItem& /*option*/,
                                        const QModelIndex& index) const
{
  if (index.column() != 2)
    return nullptr;

  m_editingIndex = index;

  auto* editor = new QPlainTextEdit(parent);
  editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  editor->setAutoFillBackground(true);
  return editor;
}

void HtmlItemDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
  auto* pte = qobject_cast<QPlainTextEdit*>(editor);
  if (!pte)
    return;
  pte->setPlainText(index.data(Qt::DisplayRole).toString());
}

void HtmlItemDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                    const QModelIndex& index) const
{
  auto* pte = qobject_cast<QPlainTextEdit*>(editor);
  if (!pte)
    return;
  model->setData(index, pte->toPlainText(), Qt::DisplayRole);
}

void HtmlItemDelegate::destroyEditor(QWidget* editor, const QModelIndex& index) const
{
  m_editingIndex = QModelIndex();
  QStyledItemDelegate::destroyEditor(editor, index);
}

void HtmlItemDelegate::updateEditorGeometry(QWidget* editor,
                                            const QStyleOptionViewItem& option,
                                            const QModelIndex& /*index*/) const
{
  editor->setGeometry(option.rect);
}
