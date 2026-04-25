#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

/**
 * In-process PAK reader backed by bg3rustpaklib.
 *
 * Replaces every Divine.exe subprocess call that bg3_localization_content used
 * for pak listing, file extraction, and .loca parsing.  All functions open the
 * PAK, perform the requested operation, and close it; the mmap cost is
 * negligible compared to the subprocess overhead it replaces.
 */
namespace LWizardPakReader {

/** Returns true if the PAK contains any "Localization/{language}/" entry. */
bool hasLocalization(const QString& pakPath, const QString& language);

/**
 * Returns all internal file paths in the PAK
 * (e.g. "Localization/Russian/strings.loca").
 * Returns an empty list if the PAK cannot be opened.
 */
QStringList listFiles(const QString& pakPath);

/**
 * Read a single file from the PAK by its internal path.
 * Returns an empty QByteArray if not found or on error.
 */
QByteArray readFile(const QString& pakPath, const QString& entryPath);

/**
 * Parse raw binary .loca bytes and return a Qt-compressed JSON map
 * {"uuid":"text", ...} compatible with qUncompress().
 * Returns an empty QByteArray on error.
 */
QByteArray locaBytesToJsonCompressed(const QByteArray& locaBytes);

} // namespace LWizardPakReader
