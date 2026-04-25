#pragma once

#include <memory>

#include <QHash>
#include <QMetaObject>
#include <QModelIndex>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class BG3LocalizationContent;
class QMainWindow;
class QTreeView;

namespace MOBase {
class IOrganizer;
}

class LWizardModListUiPatch : public QObject
{
  Q_OBJECT

public:
  LWizardModListUiPatch(MOBase::IOrganizer*                     organizer,
                        std::shared_ptr<BG3LocalizationContent> content,
                        QObject*                                parent = nullptr);

  bool attach(QMainWindow* mainWindow);
  void ensureInstalled();
  void refreshFromSelection();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  MOBase::IOrganizer*                     m_organizer = nullptr;
  std::shared_ptr<BG3LocalizationContent> m_content;
  QPointer<QMainWindow>                   m_mainWindow;
  QPointer<QTreeView>                     m_modList;
  class LinkedHighlightProxyModel*        m_proxy = nullptr;
  QMetaObject::Connection                 m_selectionChangedConnection;
  QMetaObject::Connection                 m_currentChangedConnection;
  bool                                    m_missingModListLogged = false;

  QString             modNameForIndex(const QModelIndex& index) const;
  QSet<QString>       selectedMods() const;
  QHash<QString, int> highlightedModKinds() const;
  QModelIndex findModIndex(const QString& modName, const QModelIndex& parent = QModelIndex()) const;
  void        reconnectSelectionModel();
  void        restoreSelection(const QStringList& selectedMods, const QString& currentMod);
};
