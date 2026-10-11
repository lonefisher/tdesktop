#pragma once

#include <QtCore/QString>
#include <QtCore/QtGlobal>

namespace Core::FishGramUpdates {

// Creates an offline, recovery-tool-compatible snapshot before a baseline update.
// The storage sibling is `.fishgram-snapshots-` plus the first 16 lowercase
// hex digits of SHA-256 over UTF-8(canonical workDir), where the canonical
// Windows path has separators normalized to `/` and no trailing separator.
[[nodiscard]] bool SnapshotBeforeBaselineUpdate(
	const QString &workDir,
	const QString &installedExecutable,
	quint64 runningVersion,
	QString *error = nullptr);

} // namespace Core::FishGramUpdates
