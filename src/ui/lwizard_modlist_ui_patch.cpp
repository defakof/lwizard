#include "ui/lwizard_modlist_ui_patch.h"

#include "core/bg3_localization_content.h"
#include "core/lwizard_log.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QHelpEvent>
#include <QIdentityProxyModel>
#include <QItemSelectionModel>
#include <QMainWindow>
#include <QToolTip>
#include <QTreeView>
#include <QVariant>

#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

namespace {

constexpr int  kContentColumn    = 3;
constexpr char kRowPathSeparator = '\x1f';

enum LinkedHighlightKind
{
  HighlightTranslation = 1,
  HighlightBase        = 2,
};

QString resolveModName(MOBase::IOrganizer* organizer,
                       QAbstractItemModel* model,
                       const QModelIndex&  index)
{
  if (!organizer || !model || !index.isValid())
    return {};

  const QModelIndex nameIndex = model->index(index.row(), 0, index.parent());
  if (!nameIndex.isValid())
    return {};

  const QString displayName = model->data(nameIndex, Qt::DisplayRole).toString().trimmed();
  if (displayName.isEmpty())
    return {};

  if (organizer->modList()->getMod(displayName))
    return displayName;

  const QStringList allMods = organizer->modList()->allMods();
  for (const QString& modName : allMods) {
    if (organizer->modList()->displayName(modName) == displayName)
      return modName;
  }

  return {};
}

QString displayNameForIndex(QAbstractItemModel* model, const QModelIndex& index)
{
  if (!model || !index.isValid())
    return {};

  const QModelIndex nameIndex = model->index(index.row(), 0, index.parent());
  if (!nameIndex.isValid())
    return {};

  return model->data(nameIndex, Qt::DisplayRole).toString().trimmed();
}

QColor translationHighlightColor()
{
  return QColor(186, 225, 255, 110);
}

QColor baseHighlightColor()
{
  return QColor(244, 196, 234, 110);
}

QColor criticalHighlightColor()
{
  return QColor(193, 0, 0, 50);
}

QColor criticalTextColor()
{
  return QColor(190, 0, 0);
}

QColor toolkitHighlightColor()
{
  return QColor(0, 255, 77, 32);
}

} // namespace

class LinkedHighlightProxyModel : public QIdentityProxyModel
{
public:
  explicit LinkedHighlightProxyModel(MOBase::IOrganizer* organizer, QObject* parent = nullptr)
      : QIdentityProxyModel(parent), m_organizer(organizer)
  {}

  void refreshModNameCache()
  {
    m_modNameByDisplayName.clear();
    if (!m_organizer)
      return;

    const QStringList allMods = m_organizer->modList()->allMods();
    for (const QString& modName : allMods) {
      m_modNameByDisplayName.insert(modName, modName);
      const QString displayName = m_organizer->modList()->displayName(modName).trimmed();
      if (!displayName.isEmpty())
        m_modNameByDisplayName.insert(displayName, modName);
    }
  }

  QVariant data(const QModelIndex& index, int role) const override
  {
    if ((role == Qt::BackgroundRole || role == Qt::ForegroundRole) && index.isValid()) {
      if (role == Qt::ForegroundRole && m_extraHighlightedMods.isEmpty())
        return QIdentityProxyModel::data(index, role);

      const QString modName = resolveModNameFromCache(sourceModel(), mapToSource(index));
      const auto    extraIt = m_extraHighlightedMods.constFind(modName);
      if (extraIt != m_extraHighlightedMods.constEnd()) {
        if (extraIt.value() == BG3LocalizationContent::CONTENT_INVALID_UUID ||
            extraIt.value() == BG3LocalizationContent::CONTENT_MISSING_DEPS) {
          if (role == Qt::BackgroundRole)
            return criticalHighlightColor();
          return criticalTextColor();
        }
        if (role == Qt::BackgroundRole &&
            extraIt.value() == BG3LocalizationContent::CONTENT_TOOLKIT_PROJECT)
          return toolkitHighlightColor();
      }

      if (role != Qt::BackgroundRole)
        return QIdentityProxyModel::data(index, role);

      const auto it = m_highlightedMods.constFind(modName);
      if (it != m_highlightedMods.constEnd()) {
        if (it.value() == HighlightTranslation)
          return translationHighlightColor();
        if (it.value() == HighlightBase)
          return baseHighlightColor();
      }
    }

    return QIdentityProxyModel::data(index, role);
  }

  void setHighlightedMods(const QHash<QString, int>& highlightedMods)
  {
    if (highlightedMods == m_highlightedMods)
      return;
    m_highlightedMods = highlightedMods;
    emitBackgroundChanged(QModelIndex());
  }

  void setExtraHighlightedMods(const QHash<QString, int>& highlightedMods)
  {
    if (highlightedMods == m_extraHighlightedMods)
      return;
    m_extraHighlightedMods = highlightedMods;
    emitBackgroundChanged(QModelIndex());
  }

  void emitContentColumnChanged()
  {
    emitContentColumnChanged(QModelIndex());
  }

private:
  QString resolveModNameFromCache(QAbstractItemModel* model, const QModelIndex& index) const
  {
    const QString displayName = displayNameForIndex(model, index);
    if (displayName.isEmpty())
      return {};
    return m_modNameByDisplayName.value(displayName, displayName);
  }

  void emitBackgroundChanged(const QModelIndex& parent)
  {
    if (!sourceModel())
      return;

    const int rows = rowCount(parent);
    const int cols = columnCount(parent);
    if (rows <= 0 || cols <= 0)
      return;

    emit dataChanged(index(0, 0, parent),
                     index(rows - 1, cols - 1, parent),
                     {Qt::BackgroundRole, Qt::ForegroundRole});

    for (int row = 0; row < rows; ++row)
      emitBackgroundChanged(index(row, 0, parent));
  }

  void emitContentColumnChanged(const QModelIndex& parent)
  {
    if (!sourceModel())
      return;

    const int rows = rowCount(parent);
    if (rows <= 0 || columnCount(parent) <= kContentColumn)
      return;

    emit dataChanged(index(0, kContentColumn, parent), index(rows - 1, kContentColumn, parent));

    for (int row = 0; row < rows; ++row)
      emitContentColumnChanged(index(row, 0, parent));
  }

private:
  MOBase::IOrganizer*     m_organizer = nullptr;
  QHash<QString, int>     m_highlightedMods;
  QHash<QString, int>     m_extraHighlightedMods;
  QHash<QString, QString> m_modNameByDisplayName;
};

LWizardModListUiPatch::LWizardModListUiPatch(MOBase::IOrganizer*                     organizer,
                                             std::shared_ptr<BG3LocalizationContent> content,
                                             QObject*                                parent)
    : QObject(parent), m_organizer(organizer), m_content(std::move(content))
{}

bool LWizardModListUiPatch::attach(QMainWindow* mainWindow)
{
  m_mainWindow = mainWindow;
  ensureInstalled();
  return m_modList != nullptr && m_proxy != nullptr;
}

void LWizardModListUiPatch::ensureInstalled()
{
  if (!m_mainWindow)
    return;

  QTreeView* view = m_mainWindow->findChild<QTreeView*>(QStringLiteral("modList"));
  if (!view) {
    if (!m_missingModListLogged) {
      LWizardLog::warn(QStringLiteral("LWizard could not find MO2 mod list widget \"modList\"."));
      m_missingModListLogged = true;
    }
    return;
  }
  m_missingModListLogged = false;

  if (m_modList && m_modList != view && m_modList->viewport())
    m_modList->viewport()->removeEventFilter(this);

  m_modList = view;
  if (m_modList->viewport()) {
    m_modList->viewport()->removeEventFilter(this);
    m_modList->viewport()->installEventFilter(this);
  }

  QAbstractItemModel* currentModel = m_modList->model();
  if (!currentModel)
    return;

  if (!m_proxy)
    m_proxy = new LinkedHighlightProxyModel(m_organizer, this);
  m_proxy->refreshModNameCache();

  if (currentModel != m_proxy) {
    const QStringList   selected   = selectedMods().values();
    const QString       currentMod = modNameForIndex(m_modList->currentIndex());
    const QSet<QString> expanded   = expandedRows();

    m_proxy->setSourceModel(currentModel);
    m_modList->setModel(m_proxy);
    restoreExpandedRows(expanded);
    restoreSelection(selected, currentMod);
  }

  reconnectSelectionModel();
}

void LWizardModListUiPatch::refreshOrganizerPreservingState()
{
  ensureInstalled();

  const QStringList selected   = selectedMods().values();
  const QString     currentMod = m_modList ? modNameForIndex(m_modList->currentIndex()) : QString();
  const QSet<QString> expanded = expandedRows();

  if (m_organizer)
    m_organizer->refresh(false);

  ensureInstalled();
  restoreExpandedRows(expanded);
  restoreSelection(selected, currentMod);
  refreshFromSelection();
}

void LWizardModListUiPatch::refreshContentColumn()
{
  ensureInstalled();
  if (!m_modList || !m_proxy)
    return;

  if (m_contentRefreshQueued)
    return;

  m_contentRefreshQueued = true;
  QMetaObject::invokeMethod(
      this,
      [this]() {
        m_contentRefreshQueued = false;
        refreshOrganizerPreservingState();
      },
      Qt::QueuedConnection);
}

void LWizardModListUiPatch::refreshFromSelection()
{
  ensureInstalled();
  if (!m_modList || !m_proxy)
    return;

  m_proxy->setHighlightedMods(highlightedModKinds());
  m_proxy->setExtraHighlightedMods(extraHighlightedModKinds());
  if (m_modList->viewport())
    m_modList->viewport()->update();
}

bool LWizardModListUiPatch::eventFilter(QObject* watched, QEvent* event)
{
  if (!m_modList || watched != m_modList->viewport() || event == nullptr)
    return QObject::eventFilter(watched, event);

  if (event->type() != QEvent::ToolTip)
    return QObject::eventFilter(watched, event);

  auto*             helpEvent = static_cast<QHelpEvent*>(event);
  const QModelIndex index     = m_modList->indexAt(helpEvent->pos());
  if (!index.isValid() || index.column() != kContentColumn || !m_content)
    return QObject::eventFilter(watched, event);

  const QString modName = modNameForIndex(index);
  if (modName.isEmpty())
    return QObject::eventFilter(watched, event);

  const QString tooltip = m_content->contentTooltipFor(modName);
  if (tooltip.isEmpty())
    return QObject::eventFilter(watched, event);

  QToolTip::showText(helpEvent->globalPos(), tooltip, m_modList->viewport());
  return true;
}

QString LWizardModListUiPatch::modNameForIndex(const QModelIndex& index) const
{
  if (!m_modList)
    return {};
  return resolveModName(m_organizer, m_modList->model(), index);
}

QSet<QString> LWizardModListUiPatch::selectedMods() const
{
  QSet<QString> mods;
  if (!m_modList || !m_modList->selectionModel())
    return mods;

  const QModelIndexList selectedRows = m_modList->selectionModel()->selectedRows();
  for (const QModelIndex& row : selectedRows) {
    const QString modName = modNameForIndex(row);
    if (!modName.isEmpty())
      mods.insert(modName);
  }

  return mods;
}

QHash<QString, int> LWizardModListUiPatch::highlightedModKinds() const
{
  QHash<QString, int> highlighted;
  if (!m_content)
    return highlighted;

  auto assignKind = [&](const QString& modName) {
    if (modName.isEmpty())
      return;
    if (!m_content->translationTargetFor(modName).isEmpty()) {
      highlighted.insert(modName, HighlightTranslation);
      return;
    }
    if (!m_content->separateTranslationsFor(modName).isEmpty()) {
      highlighted.insert(modName, HighlightBase);
    }
  };

  for (const QString& modName : selectedMods()) {
    if (!m_content->hasLinkedMods(modName))
      continue;

    assignKind(modName);
    const QStringList linked = m_content->linkedModsFor(modName);
    for (const QString& linkedMod : linked)
      assignKind(linkedMod);
  }

  return highlighted;
}

QHash<QString, int> LWizardModListUiPatch::extraHighlightedModKinds() const
{
  if (!m_content)
    return {};
  return m_content->extraHighlightKinds();
}

QModelIndex LWizardModListUiPatch::findModIndex(const QString&     modName,
                                                const QModelIndex& parent) const
{
  if (!m_modList || !m_modList->model())
    return {};

  QAbstractItemModel* model = m_modList->model();
  const int           rows  = model->rowCount(parent);
  for (int row = 0; row < rows; ++row) {
    const QModelIndex rowIndex = model->index(row, 0, parent);
    if (!rowIndex.isValid())
      continue;

    if (modNameForIndex(rowIndex) == modName)
      return rowIndex;

    const QModelIndex child = findModIndex(modName, rowIndex);
    if (child.isValid())
      return child;
  }

  return {};
}

void LWizardModListUiPatch::reconnectSelectionModel()
{
  QObject::disconnect(m_selectionChangedConnection);
  QObject::disconnect(m_currentChangedConnection);

  if (!m_modList || !m_modList->selectionModel())
    return;

  QItemSelectionModel* selectionModel = m_modList->selectionModel();
  m_selectionChangedConnection = connect(selectionModel,
                                         &QItemSelectionModel::selectionChanged,
                                         this,
                                         [this](const QItemSelection&, const QItemSelection&) {
                                           refreshFromSelection();
                                         });
  m_currentChangedConnection   = connect(selectionModel,
                                         &QItemSelectionModel::currentChanged,
                                         this,
                                         [this](const QModelIndex&, const QModelIndex&) {
                                         refreshFromSelection();
                                         });
}

QSet<QString> LWizardModListUiPatch::expandedRows(const QModelIndex& parent) const
{
  QSet<QString> rows;
  if (!m_modList || !m_modList->model())
    return rows;

  QAbstractItemModel* model    = m_modList->model();
  const int           rowCount = model->rowCount(parent);
  for (int row = 0; row < rowCount; ++row) {
    const QModelIndex index = model->index(row, 0, parent);
    if (!index.isValid())
      continue;

    if (m_modList->isExpanded(index))
      rows.insert(rowPathKey(index));

    rows.unite(expandedRows(index));
  }

  return rows;
}

void LWizardModListUiPatch::restoreExpandedRows(const QSet<QString>& expandedRows,
                                                const QModelIndex&   parent)
{
  if (!m_modList || !m_modList->model() || expandedRows.isEmpty())
    return;

  QAbstractItemModel* model    = m_modList->model();
  const int           rowCount = model->rowCount(parent);
  for (int row = 0; row < rowCount; ++row) {
    const QModelIndex index = model->index(row, 0, parent);
    if (!index.isValid())
      continue;

    if (expandedRows.contains(rowPathKey(index)))
      m_modList->setExpanded(index, true);

    restoreExpandedRows(expandedRows, index);
  }
}

void LWizardModListUiPatch::restoreSelection(const QStringList& selectedMods,
                                             const QString&     currentMod)
{
  if (!m_modList || !m_modList->selectionModel())
    return;

  QItemSelectionModel* selectionModel = m_modList->selectionModel();
  selectionModel->clearSelection();

  for (const QString& modName : selectedMods) {
    const QModelIndex index = findModIndex(modName);
    if (!index.isValid())
      continue;
    selectionModel->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  }

  const QModelIndex currentIndex = currentMod.isEmpty() ? QModelIndex() : findModIndex(currentMod);
  if (currentIndex.isValid()) {
    selectionModel->setCurrentIndex(currentIndex,
                                    QItemSelectionModel::NoUpdate | QItemSelectionModel::Rows);
    m_modList->scrollTo(currentIndex);
  }
}

QString LWizardModListUiPatch::rowPathKey(const QModelIndex& index) const
{
  if (!m_modList || !m_modList->model() || !index.isValid())
    return {};

  QStringList path;
  QModelIndex current = index;
  while (current.isValid()) {
    const QString displayName = displayNameForIndex(m_modList->model(), current);
    path.prepend(displayName.isEmpty() ? QString::number(current.row()) : displayName);
    current = current.parent();
  }

  return path.join(QChar::fromLatin1(kRowPathSeparator));
}
