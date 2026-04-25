#include "core/lwizard_divine.h"
#include "core/lwizard_log.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QThread>

#include <uibase/imoinfo.h>

namespace LWizardDivine {

QString existingExecutable(MOBase::IOrganizer* organizer)
{
  if (!organizer)
    return {};

  const QString base = organizer->basePath();

  const QString known =
      base + QStringLiteral("/plugins/basic_games/games/baldursgate3/tools/Divine.exe");
  if (QFileInfo::exists(known))
    return QDir::toNativeSeparators(known);

  QDirIterator it(base + QStringLiteral("/plugins"),
                  QStringList{QStringLiteral("Divine.exe")}, QDir::Files,
                  QDirIterator::Subdirectories);
  if (it.hasNext())
    return QDir::toNativeSeparators(it.next());

  const QString downloaded = base + QStringLiteral("/plugins/lwizard/Divine.exe");
  if (QFileInfo::exists(downloaded))
    return QDir::toNativeSeparators(downloaded);

  return {};
}

void ensureDownloadedIfMissing(MOBase::IOrganizer* organizer)
{
  if (!organizer)
    return;
  if (!existingExecutable(organizer).isEmpty())
    return;

  const QString targetDir =
      QDir::toNativeSeparators(organizer->basePath() + QStringLiteral("/plugins/lwizard"));

  LWizardLog::info(QStringLiteral("Divine.exe not found; downloading LSLib to ") + targetDir);

  auto* thread = QThread::create([targetDir]() {
    static constexpr const char* k_zipUrl =
        "https://github.com/Norbyte/lslib/releases/download/v1.19.5/"
        "ExportTool-v1.19.5.zip";

    const QString tempZip =
        QDir::temp().filePath(QStringLiteral("ExportTool-v1.19.5.zip"));

    const int dlRet = QProcess::execute(
        QStringLiteral("powershell"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QString::fromLatin1(
             "[Net.ServicePointManager]::SecurityProtocol = "
             "[Net.SecurityProtocolType]::Tls12; "
             "Invoke-WebRequest -Uri '%1' -OutFile '%2'")
             .arg(QString::fromLatin1(k_zipUrl), tempZip)});

    if (dlRet != 0) {
      LWizardLog::warn(
          QStringLiteral("Failed to download LSLib (exit code %1)").arg(dlRet));
      return;
    }

    QDir().mkpath(targetDir);

    const QString psExtract = QString::fromLatin1(
        "Add-Type -Assembly 'System.IO.Compression.FileSystem';"
        "$zip = [IO.Compression.ZipFile]::OpenRead('%1');"
        "foreach ($e in $zip.Entries) {"
        "  if ($e.FullName -like 'Tools/*' -and $e.Name -ne '') {"
        "    $dest = '%2\\' + $e.Name;"
        "    [IO.Compression.ZipFileExtensions]::ExtractToFile($e,$dest,$true)"
        "  }"
        "};"
        "$zip.Dispose()")
                                  .arg(tempZip, targetDir);

    const int exRet = QProcess::execute(
        QStringLiteral("powershell"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"), psExtract});

    if (exRet != 0) {
      LWizardLog::warn(
          QStringLiteral("Failed to extract LSLib (exit code %1)").arg(exRet));
      return;
    }

    LWizardLog::info(QStringLiteral("LSLib downloaded and extracted to ") + targetDir);
  });

  QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  thread->start();
}

}  // namespace LWizardDivine
