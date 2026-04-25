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

constexpr int kContentColumn = 3;

enum LinkedHighlightKind
{
  HighlightTranslation = 1,
  HighlightBase        = 2,
};

QString resolveModName(MOBase::IOrganizer* organizer, QAbstractItemModel* model,
                       const QModelIndex& index)
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

QColor translationHighlightColor()
{
  return QColor(186, 225, 255, 110);
}

QColor baseHighlightColor()
{
  return QColor(244, 196, 234, 110);
}

}  // namespace

class LinkedHighlightProxyModel : public QIdentityProxyModel
{
public:
  explicit LinkedHighlightProxyModel(MOBase::IOrganizer* organizer, QObject* parent = nullptr)
      : QIdentityProxyModel(parent), m_organizer(organizer)
  {}

  QVariant data(const QModelIndex& index, int role) const override
  {
    if (role == Qt::BackgroundRole && index.isValid()) {
      const QString modName =
          resolveModName(m_organizer, sourceModel(), mapToSource(index));
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

private:
  void emitBackgroundChanged(const QModelIndex& parent)
  {
    if (!sourceModel())
      return;

    const int rows = rowCount(parent);
    const int cols = columnCount(parent);
    if (rows <= 0 || cols <= 0)
      return;

    emit dataChanged(index(0, 0, parent), index(rows - 1, cols - 1, parent),
                     {Qt::BackgroundRole});

    for (int row = 0; row < rows; ++row)
      emitBackgroundChanged(index(row, 0, parent));
  }

private:
  MOBase::IOrganizer* m_organizer = nullptr;
  QHash<QString, int> m_highlightedMods;
};

LWizardModListUiPatch::LWizardModListUiPatch(
    MOBase::IOrganizer* organizer, std::shared_ptr<BG3LocalizationContent> content,
    QObject* parent)
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

  if (currentModel != m_proxy) {
    const QStringList selected =
        selectedMods().values();
    const QString currentMod = modNameForIndex(m_modList->currentIndex());

    m_proxy->setSourceModel(currentModel);
    m_modList->setModel(m_proxy);
    restoreSelection(selected, currentMod);
  }

  reconnectSelectionModel();
}

void LWizardModListUiPatch::refreshFromSelection()
{
  ensureInstalled();
  if (!m_modList || !m_proxy)
    return;

  m_proxy->setHighlightedMods(highlightedModKinds());
  if (m_modList->viewport())
    m_modList->viewport()->update();
}

bool LWizardModListUiPatch::eventFilter(QObject* watched, QEvent* event)
{
  if (!m_modList || watched != m_modList->viewport() || event == nullptr)
    return QObject::eventFilter(watched, event);

  if (event->type() != QEvent::ToolTip)
    return QObject::eventFilter(watched, event);

  auto* helpEvent = static_cast<QHelpEvent*>(event);
  const QModelIndex index = m_modList->indexAt(helpEvent->pos());
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

QModelIndex LWizardModListUiPatch::findModIndex(const QString& modName,
                                                const QModelIndex& parent) const
{
  if (!m_modList || !m_modList->model())
    return {};

  QAbstractItemModel* model = m_modList->model();
  const int rows            = model->rowCount(parent);
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
  m_selectionChangedConnection =
      connect(selectionModel, &QItemSelectionModel::selectionChanged, this,
              [this](const QItemSelection&, const QItemSelection&) {
                refreshFromSelection();
              });
  m_currentChangedConnection =
      connect(selectionModel, &QItemSelectionModel::currentChanged, this,
              [this](const QModelIndex&, const QModelIndex&) {
                refreshFromSelection();
              });
}

void LWizardModListUiPatch::restoreSelection(const QStringList& selectedMods,
                                             const QString& currentMod)
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
    selectionModel->setCurrentIndex(
        currentIndex, QItemSelectionModel::NoUpdate | QItemSelectionModel::Rows);
    m_modList->scrollTo(currentIndex);
  }
}
