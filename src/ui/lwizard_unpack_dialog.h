#pragma once

#include <QDialog>

namespace MOBase {
class IOrganizer;
}

class QComboBox;
class QTextEdit;
class QPushButton;

/**
 * Select a mod and unpack every .pak under its directory next to each .pak
 * (Divine extract-package with --use-package-name into the pak's folder).
 */
class LWizardUnpackDialog : public QDialog
{
  Q_OBJECT

public:
  explicit LWizardUnpackDialog(MOBase::IOrganizer* organizer, QWidget* parent = nullptr);

private slots:
  void onUnpack();
  void onLogEntry(const QString& entry);

private:
  void refillMods();

  MOBase::IOrganizer* m_organizer = nullptr;
  QComboBox*          m_combo     = nullptr;
  QTextEdit*          m_log       = nullptr;
  QPushButton*        m_unpackBtn = nullptr;
};
