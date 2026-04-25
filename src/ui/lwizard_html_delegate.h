#pragma once

#include <QModelIndex>
#include <QStyledItemDelegate>

/**
 * Table delegate for the Translation tab.
 *
 * Column 0 (UUID)        : plain text, read-only, elided.
 * Column 1 (Original)    : BG3 markup rendered as HTML, read-only.
 * Column 2 (Translation) : HTML when displaying, raw text in QPlainTextEdit when editing.
 *
 * BG3 markup → HTML conversion:
 *   <LSTag ...>text</LSTag>  →  <span style="color:#7ab4d4">text</span>
 *   <br>                     →  <br/>
 *   <b>, <i>, <s>, <u>       →  kept as-is
 *   unknown tags             →  stripped
 */
class HtmlItemDelegate : public QStyledItemDelegate
{
  Q_OBJECT

public:
  explicit HtmlItemDelegate(QObject* parent = nullptr);

  void paint(QPainter*                   painter,
             const QStyleOptionViewItem& option,
             const QModelIndex&          index) const override;

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  QWidget* createEditor(QWidget*                    parent,
                        const QStyleOptionViewItem& option,
                        const QModelIndex&          index) const override;

  void setEditorData(QWidget* editor, const QModelIndex& index) const override;

  void setModelData(QWidget*            editor,
                    QAbstractItemModel* model,
                    const QModelIndex&  index) const override;

  void updateEditorGeometry(QWidget*                    editor,
                            const QStyleOptionViewItem& option,
                            const QModelIndex&          index) const override;

  void destroyEditor(QWidget* editor, const QModelIndex& index) const override;

  /** Convert BG3 raw markup text to displayable Qt HTML. */
  static QString toDisplayHtml(const QString& rawText);

private:
  mutable QModelIndex m_editingIndex; // index currently open in an editor

  static constexpr int k_minRowHeight = 36;
  static constexpr int k_maxRowHeight = 240;
  static constexpr int k_hPad         = 6;
  static constexpr int k_vPad         = 4;
};
