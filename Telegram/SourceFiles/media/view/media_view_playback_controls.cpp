/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/view/media_view_playback_controls.h"

#include "media/audio/media_audio.h"
#include "media/player/media_player_button.h"
#include "media/player/media_player_dropdown.h"
#include "media/streaming/media_streaming_rtx_fg.h"
#include "media/streaming/media_streaming_rtx_nr.h"
#include "media/streaming/media_streaming_rtx_vsr.h"
#include "media/view/media_view_playback_progress.h"
#include "logs.h"
#include "ui/widgets/cross_fade_label.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/effects/fade_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/tooltip.h"
#include "ui/text/format_values.h"
#include "ui/cached_round_corners.h"
#include "lang/lang_keys.h"
#include "styles/style_media_view.h"

#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <array>

namespace Media {
namespace View {
namespace  {

constexpr auto kEps = 0.005;
constexpr auto kEnhanceRefreshMs = crl::time(2000);

[[nodiscard]] rpl::producer<QString> EnhanceShort(EnhanceKind kind) {
	switch (kind) {
	case EnhanceKind::Vsr:
		return tr::lng_fishgram_enhance_vsr_short();
	case EnhanceKind::Hdr:
		return tr::lng_fishgram_enhance_hdr_short();
	case EnhanceKind::Fg:
		return tr::lng_fishgram_enhance_fg_short();
	case EnhanceKind::Nr:
		return tr::lng_fishgram_enhance_nr_short();
	}
	return rpl::single(QString());
}

[[nodiscard]] rpl::producer<QString> EnhanceFeature(EnhanceKind kind) {
	switch (kind) {
	case EnhanceKind::Vsr:
		return tr::lng_fishgram_rtx_vsr();
	case EnhanceKind::Hdr:
		return tr::lng_fishgram_rtx_hdr();
	case EnhanceKind::Fg:
		return tr::lng_fishgram_rtx_fg();
	case EnhanceKind::Nr:
		return tr::lng_fishgram_dlss_nr();
	}
	return rpl::single(QString());
}

} // namespace

class EnhanceToggle final
	: public Ui::RippleButton
	, public Ui::AbstractTooltipShower {
public:
	EnhanceToggle(
		QWidget *parent,
		EnhanceKind kind,
		Fn<void()> onClick,
		Fn<void()> relayout);

	void setState(bool stored, bool available);
	[[nodiscard]] int preferredWidth() const;

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;
	[[nodiscard]] QAccessible::Role accessibilityRole() override;
	[[nodiscard]] Ui::AccessibilityState accessibilityState() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

private:
	Fn<void()> _relayout;
	QString _label;
	QString _feature;
	bool _stored = false;
	bool _available = false;

};

EnhanceToggle::EnhanceToggle(
	QWidget *parent,
	EnhanceKind kind,
	Fn<void()> onClick,
	Fn<void()> relayout)
: Ui::RippleButton(parent, st::mediaviewSpeedButton.ripple)
, _relayout(std::move(relayout)) {
	setMouseTracking(true);
	setPointerCursor(false);
	EnhanceShort(kind) | rpl::on_next([=](QString text) {
		_label = std::move(text);
		update();
		if (_relayout && width() > 0) {
			_relayout();
		}
	}, lifetime());
	EnhanceFeature(kind) | rpl::on_next([=](QString text) {
		_feature = std::move(text);
		setAccessibleName(_feature);
	}, lifetime());
	clicks() | rpl::on_next([=] {
		if (onClick) {
			onClick();
		}
	}, lifetime());
}

void EnhanceToggle::setState(bool stored, bool available) {
	if (_stored == stored && _available == available) {
		return;
	}
	_stored = stored;
	_available = available;
	setPointerCursor(available);
	update();
	accessibilityStateChanged({ .checked = true });
}

int EnhanceToggle::preferredWidth() const {
	const auto text = st::mediaviewEnhanceText.font->width(_label);
	const auto padded = text + (st::mediaviewEnhanceTextSkip * 2);
	return std::clamp(
		padded,
		st::mediaviewEnhanceMinWidth,
		st::mediaviewEnhanceMaxWidth);
}

void EnhanceToggle::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	paintRipple(p, QPoint(), nullptr);

	const auto on = _stored && _available;
	const auto over = isOver() && _available;
	auto fg = on
		? st::mediaviewTextLinkFg->c
		: (over
			? st::mediaviewPlaybackIconFgOver->c
			: st::mediaviewPlaybackIconFg->c);
	if (!_available) {
		fg.setAlpha(96);
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto inner = rect().marginsRemoved(st::mediaviewEnhanceInset);
	const auto radius = st::mediaviewEnhanceRadius;
	if (on) {
		auto fill = st::mediaviewTextLinkFg->c;
		fill.setAlpha(over ? 72 : 48);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(inner, radius, radius);
	} else if (_available) {
		auto pen = fg;
		pen.setAlpha(over ? 180 : 90);
		p.setPen(QPen(pen, st::mediaviewEnhanceStroke));
		p.setBrush(Qt::NoBrush);
		const auto stroke = st::mediaviewEnhanceStroke;
		p.drawRoundedRect(
			inner.adjusted(0, 0, -stroke, -stroke),
			radius,
			radius);
	}
	p.setPen(fg);
	p.setFont(st::mediaviewEnhanceText.font);
	auto text = _label;
	if (st::mediaviewEnhanceText.font->width(text) > inner.width()) {
		text = st::mediaviewEnhanceText.font->elided(text, inner.width());
	}
	p.drawText(inner, Qt::AlignCenter, text);
}

void EnhanceToggle::mouseMoveEvent(QMouseEvent *e) {
	RippleButton::mouseMoveEvent(e);
	if (rect().contains(e->pos())) {
		Ui::Tooltip::Show(1000, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

void EnhanceToggle::leaveEventHook(QEvent *e) {
	RippleButton::leaveEventHook(e);
	Ui::Tooltip::Hide();
}

QString EnhanceToggle::tooltipText() const {
	const auto state = !_available
		? tr::lng_fishgram_enhance_unavailable(tr::now)
		: (_stored
			? tr::lng_fishgram_enhance_on(tr::now)
			: tr::lng_fishgram_enhance_off(tr::now));
	return _feature + u": "_q + state;
}

QPoint EnhanceToggle::tooltipPos() const {
	return QCursor::pos();
}

bool EnhanceToggle::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

QImage EnhanceToggle::prepareRippleMask() const {
	const auto inner = rect().marginsRemoved(st::mediaviewEnhanceInset);
	return Ui::RippleAnimation::RoundRectMask(
		inner.size(),
		st::mediaviewEnhanceRadius);
}

QPoint EnhanceToggle::prepareRippleStartPosition() const {
	if (!_available) {
		return DisabledRippleStartPosition();
	}
	const auto inner = rect().marginsRemoved(st::mediaviewEnhanceInset);
	const auto result = mapFromGlobal(QCursor::pos()) - inner.topLeft();
	return inner.contains(result)
		? result
		: DisabledRippleStartPosition();
}

QAccessible::Role EnhanceToggle::accessibilityRole() {
	return QAccessible::Role::CheckBox;
}

Ui::AccessibilityState EnhanceToggle::accessibilityState() const {
	return {
		.checkable = true,
		.checked = _stored,
	};
}

PlaybackControls::PlaybackControls(
	QWidget *parent,
	not_null<Delegate*> delegate)
: RpWidget(parent)
, _delegate(delegate)
, _speedControllable(Media::Audio::SupportsSpeedControl())
, _qualitiesList(_delegate->playbackControlsQualities())
, _playPauseResume(this, st::mediaviewPlayButton)
, _playbackSlider(this, st::mediaviewPlayback)
, _playbackProgress(std::make_unique<PlaybackProgress>())
, _volumeToggle(this, st::mediaviewVolumeToggle)
, _volumeController(this, st::mediaviewPlayback)
, _speedToggle((_speedControllable || !_qualitiesList.empty())
	? object_ptr<Player::SettingsButton>(this, st::mediaviewSpeedButton)
	: nullptr)
, _fullScreenToggle(this, st::mediaviewFullScreenButton)
, _pictureInPicture(this, st::mediaviewPipButton)
, _playedAlready(this, st::mediaviewPlayProgressLabel)
, _toPlayLeft(this, st::mediaviewPlayProgressLabel)
, _speedController(_speedToggle
	? std::make_unique<Player::SpeedController>(
		_speedToggle.data(),
		_speedToggle->st(),
		parent,
		[=](bool) {},
		(_speedControllable
			? [=](bool lastNonDefault) {
				return speedLookup(lastNonDefault);
			}
			: Fn<float64(bool)>()),
		(_speedControllable
			? [=](float64 speed) { saveSpeed(speed); }
			: Fn<void(float64)>()),
		_qualitiesList,
		[=] { return _delegate->playbackControlsCurrentQuality(); },
		[=](Media::VideoQuality quality) { saveQuality(quality); })
	: nullptr)
, _fadeAnimation(std::make_unique<Ui::FadeAnimation>(this)) {
	_fadeAnimation->show();
	_fadeAnimation->setFinishedCallback([=] {
		fadeFinished();
	});
	_fadeAnimation->setUpdatedCallback([=](float64 opacity) {
		fadeUpdated(opacity);
	});

	_speedToggle->setSpeed(_speedControllable
		? _delegate->playbackControlsCurrentSpeed(false)
		: 1.);
	updateSpeedToggleQuality();

	if (const auto controller = _speedController.get()) {
		controller->menuToggledValue(
		) | rpl::on_next([=](bool toggled) {
			_speedToggle->setActive(toggled);
		}, _speedToggle->lifetime());
	}

	_pictureInPicture->addClickHandler([=] {
		_delegate->playbackControlsToPictureInPicture();
	});

	_volumeController->setValue(_delegate->playbackControlsCurrentVolume());
	_volumeController->setChangeProgressCallback([=](float64 value) {
		_delegate->playbackControlsVolumeChanged(value);
		updateVolumeToggleIcon();
	});
	_volumeController->setChangeFinishedCallback([=](float64) {
		_delegate->playbackControlsVolumeChangeFinished();
	});
	updateVolumeToggleIcon();
	_volumeToggle->setClickedCallback([=] {
		_delegate->playbackControlsVolumeToggled();
		_volumeController->setValue(_delegate->playbackControlsCurrentVolume());
		updateVolumeToggleIcon();
	});

	_playPauseResume->addClickHandler([=] {
		if (_showPause) {
			_delegate->playbackControlsPause();
		} else {
			_delegate->playbackControlsPlay();
		}
	});
	_fullScreenToggle->addClickHandler([=] {
		if (_inFullScreen) {
			_delegate->playbackControlsFromFullScreen();
		} else {
			_delegate->playbackControlsToFullScreen();
		}
	});

	_playbackProgress->setValueChangedCallback([=](
			float64 value,
			float64 receivedTill) {
		_playbackSlider->setValue(value, receivedTill);
	});
	_playbackSlider->setChangeProgressCallback([=](float64 value) {
		_playbackProgress->setValue(value, false);

		// This may destroy PlaybackControls.
		handleSeekProgress(value);
	});
	_playbackSlider->setChangeFinishedCallback([=](float64 value) {
		_playbackProgress->setValue(value, false);
		handleSeekFinished(value);
	});

	const auto relayout = [=] {
		resizeEvent(nullptr);
	};
	const auto make = [&](auto &button, EnhanceKind kind) {
		button.create(this, kind, [=] {
			toggleEnhance(kind);
		}, relayout);
		button->hide();
	};
	make(_enhanceVsr, EnhanceKind::Vsr);
	make(_enhanceHdr, EnhanceKind::Hdr);
	make(_enhanceFg, EnhanceKind::Fg);
	make(_enhanceNr, EnhanceKind::Nr);

	const auto refresh = [=](bool) {
		refreshEnhanceToggles();
	};
	Streaming::RtxVideoSuperResolutionEnabledValue(
	) | rpl::on_next(refresh, lifetime());
	Streaming::RtxVideoHdrEnabledValue(
	) | rpl::on_next(refresh, lifetime());
	Streaming::RtxVideoFrameGenerationEnabledValue(
	) | rpl::on_next(refresh, lifetime());
	Streaming::RtxDlssNrEnabledValue(
	) | rpl::on_next(refresh, lifetime());
}

void PlaybackControls::handleSeekProgress(float64 progress) {
	if (!_lastDurationMs) return;

	const auto positionMs = std::clamp(
		static_cast<crl::time>(progress * _lastDurationMs),
		crl::time(0),
		_lastDurationMs);
	if (_seekPositionMs != positionMs) {
		_seekPositionMs = positionMs;
		refreshTimeTexts();
		updateTimestampLabel();

		// This may destroy PlaybackControls.
		_delegate->playbackControlsSeekProgress(positionMs);
	}
}

void PlaybackControls::handleSeekFinished(float64 progress) {
	if (!_lastDurationMs) return;

	const auto positionMs = std::clamp(
		static_cast<crl::time>(progress * _lastDurationMs),
		crl::time(0),
		_lastDurationMs);
	_seekPositionMs = -1;
	refreshTimeTexts();

	// This may destroy PlaybackControls.
	_delegate->playbackControlsSeekFinished(positionMs);
}

template <typename Callback>
void PlaybackControls::startFading(Callback start) {
	if (!_fadeAnimation->animating()) {
		showChildren();
		applyEnhanceGeometry();
		_playbackSlider->disablePaint(true);
		_volumeController->disablePaint(true);
		_childrenHidden = false;
	}
	start();
	if (_fadeAnimation->animating()) {
		for (const auto child : children()) {
			if (child->isWidgetType()
				&& child != _playbackSlider
				&& child != _volumeController) {
				static_cast<QWidget*>(child)->hide();
			}
		}
		_childrenHidden = true;
	} else {
		fadeFinished();
	}
	_playbackSlider->disablePaint(false);
	_volumeController->disablePaint(false);
}

void PlaybackControls::showAnimated() {
	startFading([this]() {
		_fadeAnimation->fadeIn(st::mediaviewShowDuration);
	});
}

void PlaybackControls::hideAnimated() {
	startFading([this]() {
		_fadeAnimation->fadeOut(st::mediaviewHideDuration);
	});
}

void PlaybackControls::fadeFinished() {
	fadeUpdated(_fadeAnimation->visible() ? 1. : 0.);
}

void PlaybackControls::fadeUpdated(float64 opacity) {
	_playbackSlider->setFadeOpacity(opacity);
	_volumeController->setFadeOpacity(opacity);
}

float64 PlaybackControls::speedLookup(bool lastNonDefault) const {
	return _delegate->playbackControlsCurrentSpeed(lastNonDefault);
}

void PlaybackControls::saveSpeed(float64 speed) {
	_speedToggle->setSpeed(speed);
	_delegate->playbackControlsSpeedChanged(speed);
}

void PlaybackControls::saveQuality(Media::VideoQuality quality) {
	_speedToggle->setQuality(quality);
	_delegate->playbackControlsQualityChanged(quality);
}

void PlaybackControls::updateSpeedToggleQuality() {
	const auto qualities = _delegate->playbackControlsQualities();
	if (_qualitiesList != qualities) {
		_qualitiesList = qualities;
		if (_speedController) {
			_speedController->setQualities(qualities);
		}
	}
	_speedToggle->setQuality(_delegate->playbackControlsCurrentQuality());
}

void PlaybackControls::updatePlaybackSpeed(float64 speed) {
	DEBUG_LOG(("Media playback speed: update to %1.").arg(speed));
	_delegate->playbackControlsSpeedChanged(speed);
	resizeEvent(nullptr);
}

void PlaybackControls::updatePlayback(const Player::TrackState &state) {
	const auto now = crl::now();
	if (!_enhanceRefreshMs || now - _enhanceRefreshMs >= kEnhanceRefreshMs) {
		_enhanceRefreshMs = now;
		refreshEnhanceToggles();
	}
	updatePlayPauseResumeState(state);
	_playbackProgress->updateState(state, countDownloadedTillPercent(state));
	updateTimeTexts(state);
	updateTimestampLabel();
}

void PlaybackControls::updateVolumeToggleIcon() {
	const auto volume = _delegate->playbackControlsCurrentVolume();
	_volumeToggle->setIconOverride([&] {
		return (volume <= 0.)
			? nullptr
			: (volume < 1 / 2.)
			? &st::mediaviewVolumeIcon1
			: &st::mediaviewVolumeIcon2;
	}(), [&] {
		return (volume <= 0.)
			? nullptr
			: (volume < 1 / 2.)
			? &st::mediaviewVolumeIcon1Over
			: &st::mediaviewVolumeIcon2Over;
	}());
}

float64 PlaybackControls::countDownloadedTillPercent(
		const Player::TrackState &state) const {
	if (_loadingReady > 0 && _loadingReady == _loadingTotal) {
		return 1.;
	}
	const auto header = state.fileHeaderSize;
	if (!header || _loadingReady <= header || _loadingTotal <= header) {
		return 0.;
	}
	return (_loadingReady - header) / float64(_loadingTotal - header);
}

void PlaybackControls::setTimestamps(std::vector<TimestampData> timestamps) {
	_playbackSlider->clearDividers();
	_playbackSlider->setDividerStyle(
		Ui::MediaSlider::DividerStyle::Gaps);
	_timestamps = std::move(timestamps);
	for (const auto &ts : _timestamps) {
		if (ts.position > 0.) {
			_playbackSlider->addDivider(
				ts.position,
				st::mediaviewPlaybackTimestamp);
		}
	}
	const auto hasLabels = ranges::any_of(
		_timestamps,
		[](const auto &ts) { return !ts.label.isEmpty(); });
	if (hasLabels && !_timestampLabel) {
		_timestampLabel.create(
			this,
			st::mediaviewTimestampLabel);
		_timestampLabel->show();
	} else if (!hasLabels) {
		_timestampLabel.destroy();
	}
	_playbackSlider->update();
	_currentTimestampIndex = -1;
	updateTimestampLabel();
}

bool PlaybackControls::hasTimestamps() const {
	return _timestampLabel != nullptr;
}

auto PlaybackControls::nextTimestamp(float64 progress) const
-> std::optional<TimestampData> {
	for (const auto &ts : _timestamps) {
		if (ts.position > progress + kEps) {
			return ts;
		}
	}
	return std::nullopt;
}

auto PlaybackControls::prevTimestamp(float64 progress) const
-> std::optional<TimestampData> {
	for (auto i = int(_timestamps.size()) - 1; i >= 0; --i) {
		if (_timestamps[i].position < progress - kEps) {
			return _timestamps[i];
		}
	}
	return std::nullopt;
}

void PlaybackControls::updateTimestampLabel() {
	if (!_timestampLabel) {
		return;
	}
	const auto progress = _playbackSlider->value();
	auto index = -1;
	for (auto i = int(_timestamps.size()) - 1; i >= 0; --i) {
		if (_timestamps[i].position <= progress) {
			index = i;
			break;
		}
	}
	if (index == _currentTimestampIndex) {
		return;
	}
	const auto previous = _currentTimestampIndex;
	_currentTimestampIndex = index;
	_timestampLabel->setDirection((index > previous) ? 1 : -1);
	auto text = (index >= 0) ? _timestamps[index].label : QString();
	const auto maxWidth = _playbackSlider->width();
	if (maxWidth > 0
		&& st::mediaviewTimestampLabel.font->width(text) > maxWidth) {
		text = st::mediaviewTimestampLabel.font->elided(text, maxWidth);
	}
	_timestampLabel->setText(text);
	resizeEvent(nullptr);
	refreshFadeCache();
}

void PlaybackControls::setLoadingProgress(int64 ready, int64 total) {
	if (_loadingReady == ready && _loadingTotal == total) {
		return;
	}
	_loadingReady = ready;
	_loadingTotal = total;
	if (_loadingReady != 0 && _loadingReady != _loadingTotal) {
		if (!_downloadProgress) {
			_downloadProgress.create(this, st::mediaviewPlayProgressLabel);
			_downloadProgress->setVisible(!_fadeAnimation->animating());
			_loadingPercent = -1;
		}
		const auto progress = total ? (ready / float64(total)) : 0.;
		const auto percent = int(base::SafeRound(progress * 100));
		if (_loadingPercent != percent) {
			_loadingPercent = percent;
			_downloadProgress->setText(QString::number(percent) + '%');
			updateDownloadProgressPosition();
			refreshFadeCache();
		}
	} else {
		_downloadProgress.destroy();
	}
}

void PlaybackControls::refreshFadeCache() {
	if (!_fadeAnimation->animating()) {
		return;
	}
	startFading([&] {
		_fadeAnimation->refreshCache();
	});
}

void PlaybackControls::updatePlayPauseResumeState(const Player::TrackState &state) {
	auto showPause = ShowPauseIcon(state.state) || (_seekPositionMs >= 0);
	if (showPause != _showPause) {
		_showPause = showPause;
		_playPauseResume->setIconOverride(_showPause ? &st::mediaviewPauseIcon : nullptr, _showPause ? &st::mediaviewPauseIconOver : nullptr);
	}
}

void PlaybackControls::updateTimeTexts(const Player::TrackState &state) {
	qint64 position = 0;

	if (Player::IsStoppedAtEnd(state.state)) {
		position = state.length;
	} else if (!Player::IsStoppedOrStopping(state.state)) {
		position = state.position;
	} else {
		position = 0;
	}
	auto playFrequency = state.frequency;
	auto playAlready = position / playFrequency;
	auto playLeft = (state.length / playFrequency) - playAlready;

	_lastDurationMs = (state.length * crl::time(1000)) / playFrequency;

	_timeAlready = Ui::FormatDurationText(playAlready);
	auto minus = QChar(8722);
	_timeLeft = minus + Ui::FormatDurationText(playLeft);

	if (_seekPositionMs < 0) {
		refreshTimeTexts();
	}
}

void PlaybackControls::refreshTimeTexts() {
	auto alreadyChanged = false, leftChanged = false;
	auto timeAlready = _timeAlready;
	auto timeLeft = _timeLeft;
	if (_seekPositionMs >= 0) {
		auto playAlready = _seekPositionMs / crl::time(1000);
		auto playLeft = (_lastDurationMs / crl::time(1000)) - playAlready;

		timeAlready = Ui::FormatDurationText(playAlready);
		auto minus = QChar(8722);
		timeLeft = minus + Ui::FormatDurationText(playLeft);
	}

	_playedAlready->setText(timeAlready, &alreadyChanged);
	_toPlayLeft->setText(timeLeft, &leftChanged);
	if (alreadyChanged || leftChanged) {
		resizeEvent(nullptr);
		refreshFadeCache();
	}
}

void PlaybackControls::setInFullScreen(bool inFullScreen) {
	if (_inFullScreen != inFullScreen) {
		_inFullScreen = inFullScreen;
		_fullScreenToggle->setIconOverride(
			_inFullScreen ? &st::mediaviewFullScreenOutIcon : nullptr,
			_inFullScreen ? &st::mediaviewFullScreenOutIconOver : nullptr);
		refreshEnhanceToggles();
	}
}

void PlaybackControls::setVideoEnhanceVisible(bool visible) {
	if (_enhanceVisible == visible) {
		return;
	}
	_enhanceVisible = visible;
	LOG(("Video enhance bar: visible=%1").arg(visible ? 1 : 0));
	refreshEnhanceToggles();
	resizeEvent(nullptr);
}

bool PlaybackControls::acceptsHdrCutout() const {
	return !isHidden() && _fadeAnimation->visible();
}

EnhanceToggleModel PlaybackControls::enhanceModel(EnhanceKind kind) const {
	const auto windowId = [&] {
		const auto top = window();
		return (top && top->internalWinId())
			? quintptr(top->internalWinId())
			: quintptr(0);
	}();
	switch (kind) {
	case EnhanceKind::Vsr:
		return ApplyEnhanceToggleSettings(
			Streaming::RtxVideoSuperResolutionEnabled(),
			Streaming::RtxVideoSuperResolutionSupported());
	case EnhanceKind::Hdr:
		return ApplyEnhanceToggleSettings(
			Streaming::RtxVideoHdrEnabled(),
			Streaming::RtxHdrShortcutAvailable(
				Streaming::RtxVideoHdrSupported(),
				Streaming::RtxWindowOnHdrDisplay(windowId)));
	case EnhanceKind::Fg:
		return ApplyEnhanceToggleSettings(
			Streaming::RtxVideoFrameGenerationEnabled(),
			Streaming::RtxVideoFrameGenerationMenuAvailable());
	case EnhanceKind::Nr:
		return ApplyEnhanceToggleSettings(
			Streaming::RtxDlssNrEnabled(),
			Streaming::RtxDlssNrAvailable());
	}
	return {};
}

void PlaybackControls::toggleEnhance(EnhanceKind kind) {
	const auto model = enhanceModel(kind);
	const auto next = ApplyEnhanceToggleClick(model);
	if (next.stored == model.stored) {
		return;
	}
	switch (kind) {
	case EnhanceKind::Vsr:
		Streaming::SetRtxVideoSuperResolutionEnabled(next.stored);
		LOG(("Video enhance bar: vsr=%1").arg(next.stored ? 1 : 0));
		break;
	case EnhanceKind::Hdr:
		Streaming::RtxVideoHdrSetEnabled(next.stored);
		LOG(("Video enhance bar: hdr=%1").arg(next.stored ? 1 : 0));
		break;
	case EnhanceKind::Fg:
		Streaming::SetRtxVideoFrameGenerationEnabled(next.stored);
		LOG(("Video enhance bar: fg=%1").arg(next.stored ? 1 : 0));
		break;
	case EnhanceKind::Nr:
		Streaming::RtxDlssNrSetEnabled(next.stored);
		LOG(("Video enhance bar: nr=%1").arg(next.stored ? 1 : 0));
		break;
	}
}

void PlaybackControls::refreshEnhanceToggles() {
	const auto apply = [&](
			const object_ptr<EnhanceToggle> &button,
			EnhanceKind kind) {
		if (button) {
			const auto model = enhanceModel(kind);
			button->setState(model.stored, model.available);
		}
	};
	apply(_enhanceVsr, EnhanceKind::Vsr);
	apply(_enhanceHdr, EnhanceKind::Hdr);
	apply(_enhanceFg, EnhanceKind::Fg);
	apply(_enhanceNr, EnhanceKind::Nr);
}

void PlaybackControls::applyEnhanceGeometry() {
	const auto toggles = std::array<EnhanceToggle*, 4>{
		_enhanceVsr.data(),
		_enhanceHdr.data(),
		_enhanceFg.data(),
		_enhanceNr.data(),
	};
	for (auto i = 0; i != int(toggles.size()); ++i) {
		const auto button = toggles[i];
		if (!button) {
			continue;
		}
		const auto &slot = _enhanceLayout.slot[i];
		if (_enhanceVisible && slot.shown) {
			button->setGeometry(slot.x, slot.y, slot.width, slot.height);
			button->show();
		} else {
			button->hide();
		}
	}
}

void PlaybackControls::resizeEvent(QResizeEvent *e) {
	const auto textSkip = st::mediaviewPlayProgressSkip;
	const auto textLeft = st::mediaviewPlayProgressLeft;
	const auto textTop = st::mediaviewPlayProgressTop;
	_playedAlready->moveToLeft(textLeft + textSkip, textTop);
	_toPlayLeft->moveToRight(textLeft + textSkip, textTop);
	const auto remove = 2 * textLeft + 4 * textSkip + _playedAlready->width() + _toPlayLeft->width();
	auto playbackWidth = width() - remove;
	_playbackSlider->resize(playbackWidth, st::mediaviewPlayback.seekSize.height());
	_playbackSlider->moveToLeft(textLeft + 2 * textSkip + _playedAlready->width(), st::mediaviewPlaybackTop);

	if (_timestampLabel) {
		_timestampLabel->resize(
			_playbackSlider->width(),
			_timestampLabel->height());
		_timestampLabel->moveToLeft(
			_playbackSlider->x(),
			st::mediaviewTimestampLabelTop);
	}

	_playPauseResume->moveToLeft(
		(width() - _playPauseResume->width()) / 2,
		st::mediaviewPlayButtonTop);

	const auto metrics = ViewerControlMetrics{
		.barWidth = width(),
		.volumeLeft = st::mediaviewVolumeToggleLeft,
		.volumeToggleWidth = _volumeToggle->width(),
		.volumeSkip = st::mediaviewVolumeSkip,
		.volumeSliderWidth = st::mediaviewVolumeWidth,
		.playWidth = _playPauseResume->width(),
		.buttonsRight = st::mediaviewButtonsRight,
		.speedWidth = _speedToggle ? _speedToggle->width() : 0,
		.pipSkip = st::mediaviewPipButtonSkip,
		.pipWidth = _pictureInPicture->width(),
		.fullSkip = st::mediaviewFullScreenButtonSkip,
		.fullWidth = _fullScreenToggle->width(),
		.rowTop = st::mediaviewButtonsTop,
		.rowHeight = _fullScreenToggle->height(),
	};
	const auto placed = LayoutViewerControls(metrics);
	if (_speedToggle) {
		_speedToggle->move(placed.speedLeft, st::mediaviewButtonsTop);
	}
	_pictureInPicture->move(placed.pipLeft, st::mediaviewButtonsTop);
	_fullScreenToggle->move(placed.fullLeft, st::mediaviewButtonsTop);

	auto preferred = st::mediaviewEnhanceMinWidth;
	const auto consider = [&](const object_ptr<EnhanceToggle> &button) {
		if (button) {
			preferred = std::max(preferred, button->preferredWidth());
		}
	};
	consider(_enhanceVsr);
	consider(_enhanceHdr);
	consider(_enhanceFg);
	consider(_enhanceNr);
	_enhanceLayout = _enhanceVisible
		? LayoutEnhanceBar(EnhanceBarFromControls(
			placed,
			st::mediaviewButtonsTop,
			_fullScreenToggle->height(),
			st::mediaviewEnhanceGuard,
			st::mediaviewEnhanceSkip,
			st::mediaviewEnhanceMinWidth,
			preferred))
		: EnhanceBarLayout();
	applyEnhanceGeometry();

	auto left = st::mediaviewVolumeToggleLeft;
	_volumeToggle->moveToLeft(left, st::mediaviewVolumeTop);
	left += _volumeToggle->width() + st::mediaviewVolumeSkip;
	_volumeController->resize(
		st::mediaviewVolumeWidth,
		st::mediaviewPlayback.seekSize.height());
	_volumeController->moveToLeft(
		left,
		st::mediaviewVolumeTop
			+ (_volumeToggle->height() - _volumeController->height()) / 2);

	updateDownloadProgressPosition();
}

void PlaybackControls::updateDownloadProgressPosition() {
	if (!_downloadProgress) {
		return;
	}
	const auto placed = PlaceDownloadLabel(
		_downloadProgress->width(),
		_downloadProgress->height(),
		_playPauseResume->y(),
		_playPauseResume->height(),
		ViewerControlRects{
			.volumeRight = _volumeController->x() + _volumeController->width(),
			.playLeft = _playPauseResume->x(),
			.playRight = _playPauseResume->x() + _playPauseResume->width(),
			.clusterLeft = _fullScreenToggle->x(),
		},
		_enhanceLayout);
	if (!placed.shown) {
		_downloadProgress->hide();
		return;
	}
	_downloadProgress->move(placed.x, placed.y);
	if (!_fadeAnimation->animating()) {
		_downloadProgress->show();
	}
}

void PlaybackControls::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	if (_fadeAnimation->paint(p)) {
		return;
	}
	if (_childrenHidden) {
		showChildren();
		applyEnhanceGeometry();
		_playbackSlider->setFadeOpacity(1.);
		_volumeController->setFadeOpacity(1.);
		_childrenHidden = false;
	}
	Ui::FillRoundRect(p, rect(), st::mediaviewSaveMsgBg, Ui::MediaviewSaveCorners);
}

void PlaybackControls::mousePressEvent(QMouseEvent *e) {
	e->accept(); // Don't pass event to the Media::View::OverlayWidget.
}

bool PlaybackControls::hasMenu() const {
	return _speedController && _speedController->menu();
}

bool PlaybackControls::dragging() const {
	return _volumeController->isChanging()
		|| _playbackSlider->isChanging()
		|| _playPauseResume->isOver()
		|| _volumeToggle->isOver()
		|| (_speedToggle && _speedToggle->isOver())
		|| _fullScreenToggle->isOver()
		|| _pictureInPicture->isOver()
		|| (_enhanceVsr && _enhanceVsr->isOver())
		|| (_enhanceHdr && _enhanceHdr->isOver())
		|| (_enhanceFg && _enhanceFg->isOver())
		|| (_enhanceNr && _enhanceNr->isOver())
		|| hasMenu();
}

PlaybackControls::~PlaybackControls() = default;

} // namespace View
} // namespace Media
