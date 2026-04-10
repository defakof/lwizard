#pragma once

#include <QString>

namespace MOBase {
class IOrganizer;
}

/**
 * Locates Divine.exe (LSLib) under the MO2 install and optionally downloads it
 * into plugins/lwizard/ when missing — shared by all lwizard plugin modules.
 */
namespace LWizardDivine {

/** Synchronous search only (no download). Returns empty if not found. */
QString existingExecutable(MOBase::IOrganizer* organizer);

/**
 * If Divine is missing, download/extract LSLib on a background thread.
 * Safe to call from init; does nothing when an executable is already present.
 */
void ensureDownloadedIfMissing(MOBase::IOrganizer* organizer);

}  // namespace LWizardDivine
