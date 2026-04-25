#include "ui/lwizard_unpack_dialog.h"

#include "core/lwizard_divine.h"
#include "core/lwizard_log.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFont>
#include <QDirIterator>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include <uibase/imodinterface.h>
#include <uibase/imodlist.h>
#include <uibase/imoinfo.h>

LWizardUnpackDialog::LWizardUnpackDialog(MOBase::IOrganizer* organizer, QWidget* parent)
    : QDialog(parent), m_organizer(organizer)
{
  setWindowTitle(tr("Unpack mod — LWizard"));
  setMinimumSize(520, 420);
  setAttribute(Qt::WA_DeleteOnClose);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(8);

  root->addWidget(
      new QLabel(tr("Choose a mod. Every .pak under that mod's folder will be extracted beside "
                    "the .pak (subfolder named from the package).")));

  m_combo = new QComboBox(this);
  refillMods();
  root->addWidget(m_combo);

  m_log = new QTextEdit(this);
  m_log->setReadOnly(true);
  m_log->setLineWrapMode(QTextEdit::NoWrap);
  {
    QFont mono(QStringLiteral("Consolas"), 9);
    mono.setStyleHint(QFont::Monospace);
    m_log->setFont(mono);
  }
  for (const QString& entry : LWizardLog::instance().entries())
    m_log->append(entry);
  auto* sb = m_log->verticalScrollBar();
  if (sb)
    sb->setValue(sb->maximum());
  root->addWidget(m_log, 1);

  connect(&LWizardLog::instance(), &LWizardLog::entryAdded, this, &LWizardUnpackDialog::onLogEntry);

  auto* buttons = new QDialogButtonBox(Qt::Horizontal, this);
  m_unpackBtn   = buttons->addButton(tr("Unpack"), QDialogButtonBox::ActionRole);
  buttons->addButton(QDialogButtonBox::Close);
  connect(m_unpackBtn, &QPushButton::clicked, this, &LWizardUnpackDialog::onUnpack);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void LWizardUnpackDialog::refillMods()
{
  m_combo->clear();
  if (!m_organizer)
    return;

  QStringList       names;
  const QStringList all = m_organizer->modList()->allMods();
  for (const QString& name : all) {
    if (m_organizer->modList()->state(name) & MOBase::IModList::STATE_VALID)
      names.append(name);
  }
  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
    return QString::compare(a, b, Qt::CaseInsensitive) < 0;
  });
  m_combo->addItems(names);
}

void LWizardUnpackDialog::onLogEntry(const QString& entry)
{
  if (m_log) {
    m_log->append(entry);
    auto* sb = m_log->verticalScrollBar();
    if (sb)
      sb->setValue(sb->maximum());
  }
}

void LWizardUnpackDialog::onUnpack()
{
  if (!m_organizer || !m_unpackBtn)
    return;

  const QString modName = m_combo->currentText();
  if (modName.isEmpty()) {
    QMessageBox::warning(this, tr("Unpack"), tr("No mod selected."));
    return;
  }

  MOBase::IModInterface* mod = m_organizer->modList()->getMod(modName);
  if (!mod) {
    QMessageBox::warning(this, tr("Unpack"), tr("Could not open the selected mod."));
    return;
  }

  const QString modPath = QDir::toNativeSeparators(mod->absolutePath());

  QString divine = LWizardDivine::existingExecutable(m_organizer);
  if (divine.isEmpty()) {
    LWizardDivine::ensureDownloadedIfMissing(m_organizer);
    QMessageBox::information(
        this,
        tr("Divine"),
        tr("Divine.exe was not found. A download may be in progress — check the LWizard "
           "log, wait for it to finish, then try again. You can also install Norbyte's "
           "LSLib / ExportTool manually."));
    return;
  }

  m_unpackBtn->setEnabled(false);

  // Set by the worker so the UI thread can open Explorer only when something ran.
  auto* hadPaks = new bool(false);

  auto* thread = QThread::create([modPath, divine, hadPaks]() {
    QStringList  paks;
    QDirIterator it(
        modPath, QStringList{QStringLiteral("*.pak")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
      paks.append(QDir::toNativeSeparators(it.next()));

    if (paks.isEmpty()) {
      LWizardLog::warn(QStringLiteral("No .pak files found under the selected mod."));
      return;
    }

    *hadPaks = true;

    LWizardLog::info(QStringLiteral("Unpack — %1 .pak file(s)").arg(paks.size()));

    for (const QString& pak : paks) {
      const QFileInfo fi(pak);
      const QString   destDir = fi.absolutePath();
      const int       ret     = QProcess::execute(divine,
                                                  {QStringLiteral("-g"),
                                                   QStringLiteral("bg3"),
                                                   QStringLiteral("-a"),
                                                   QStringLiteral("extract-package"),
                                                   QStringLiteral("-s"),
                                                   fi.absoluteFilePath(),
                                                   QStringLiteral("-d"),
                                                   destDir,
                                                   QStringLiteral("--use-package-name")});
      if (ret != 0)
        LWizardLog::warn(QStringLiteral("unpack exit %1: %2").arg(ret).arg(pak));
      else
        LWizardLog::info(QStringLiteral("unpacked: %1").arg(pak));
    }

    LWizardLog::info(QStringLiteral("Unpack finished."));
  });

  QPointer<LWizardUnpackDialog> self(this);
  QObject::connect(thread, &QThread::finished, this, [self, thread, modPath, hadPaks]() {
    if (self && self->m_unpackBtn)
      self->m_unpackBtn->setEnabled(true);
    if (*hadPaks) {
      const QUrl url = QUrl::fromLocalFile(modPath);
      if (!QDesktopServices::openUrl(url))
        LWizardLog::warn(QStringLiteral("Could not open folder in Explorer: ") + modPath);
    }
    delete hadPaks;
    thread->deleteLater();
  });
  thread->start();
}
