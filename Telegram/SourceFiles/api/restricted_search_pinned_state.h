#pragma once

namespace Main {
class Session;
}
namespace Api {
void RestrictedSearchPinnedDialogsResult(
	not_null<Main::Session*> session,
	int folderId,
	bool success);
}