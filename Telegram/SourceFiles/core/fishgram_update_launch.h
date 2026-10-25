/* FishGram update launch control. See Telegram Desktop's LEGAL for license. */
#pragma once

namespace Core::FishGramUpdates {

class UpdateLaunch final {
public:
	template <typename Start, typename Quit, typename Failed>
	bool request(Start start, Quit quit, Failed failed) {
		if (_starting) return false;
		if (_started) return true;
		_starting = true;
		_started = start();
		_starting = false;
		if (!_started) {
			failed();
			return false;
		}
		quit();
		return true;
	}

	[[nodiscard]] bool started() const { return _started; }

private:
	bool _starting = false;
	bool _started = false;
};

} // namespace Core::FishGramUpdates
