/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Media::View {

enum class EnhanceKind {
	Vsr,
	Hdr,
	Fg,
	Nr,
};

struct EnhanceToggleModel {
	bool stored = false;
	bool available = false;
};

struct EnhanceToggleAppearance {
	bool checked = false;
	bool enabled = false;
};

[[nodiscard]] inline EnhanceToggleAppearance ReadEnhanceToggle(
		EnhanceToggleModel model) {
	return {
		.checked = model.stored,
		.enabled = model.available,
	};
}

[[nodiscard]] inline EnhanceToggleModel ApplyEnhanceToggleClick(
		EnhanceToggleModel model) {
	if (!model.available) {
		return model;
	}
	model.stored = !model.stored;
	return model;
}

[[nodiscard]] inline EnhanceToggleModel ApplyEnhanceToggleSettings(
		bool stored,
		bool available) {
	return {
		.stored = stored,
		.available = available,
	};
}

struct ViewerControlMetrics {
	int barWidth = 0;
	int volumeLeft = 0;
	int volumeToggleWidth = 0;
	int volumeSkip = 0;
	int volumeSliderWidth = 0;
	int playWidth = 0;
	int buttonsRight = 0;
	int speedWidth = 0;
	int pipSkip = 0;
	int pipWidth = 0;
	int fullSkip = 0;
	int fullWidth = 0;
	int rowTop = 0;
	int rowHeight = 0;
};

struct ViewerControlRects {
	int volumeRight = 0;
	int playLeft = 0;
	int playRight = 0;
	int speedLeft = 0;
	int pipLeft = 0;
	int fullLeft = 0;
	int clusterLeft = 0;
};

[[nodiscard]] inline ViewerControlRects LayoutViewerControls(
		ViewerControlMetrics metrics) {
	auto rects = ViewerControlRects();
	rects.volumeRight = metrics.volumeLeft
		+ metrics.volumeToggleWidth
		+ metrics.volumeSkip
		+ metrics.volumeSliderWidth;
	rects.playLeft = (metrics.barWidth - metrics.playWidth) / 2;
	rects.playRight = rects.playLeft + metrics.playWidth;
	auto right = metrics.buttonsRight;
	if (metrics.speedWidth > 0) {
		rects.speedLeft = metrics.barWidth - right - metrics.speedWidth;
		right += metrics.speedWidth + metrics.pipSkip;
	}
	rects.pipLeft = metrics.barWidth - right - metrics.pipWidth;
	right += metrics.pipWidth + metrics.fullSkip;
	rects.fullLeft = metrics.barWidth - right - metrics.fullWidth;
	rects.clusterLeft = rects.fullLeft;
	return rects;
}

struct EnhanceBarSlot {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	bool shown = false;
};

struct EnhanceBarLayout {
	EnhanceBarSlot slot[4] = {};
};

struct EnhanceBarInput {
	int rowTop = 0;
	int rowHeight = 0;
	int volumeRight = 0;
	int playLeft = 0;
	int playRight = 0;
	int clusterLeft = 0;
	int guard = 0;
	int skip = 0;
	int minWidth = 0;
	int preferredWidth = 0;
	int count = 4;
};

[[nodiscard]] inline EnhanceBarInput EnhanceBarFromControls(
		const ViewerControlRects &rects,
		int rowTop,
		int rowHeight,
		int guard,
		int skip,
		int minWidth,
		int preferredWidth) {
	return {
		.rowTop = rowTop,
		.rowHeight = rowHeight,
		.volumeRight = rects.volumeRight,
		.playLeft = rects.playLeft,
		.playRight = rects.playRight,
		.clusterLeft = rects.clusterLeft,
		.guard = guard,
		.skip = skip,
		.minWidth = minWidth,
		.preferredWidth = preferredWidth,
		.count = 4,
	};
}

[[nodiscard]] inline int EnhanceSpanWidth(int left, int right) {
	return (right > left) ? (right - left) : 0;
}

[[nodiscard]] inline int EnhanceButtonsWidth(
		int count,
		int width,
		int skip) {
	if (count <= 0 || width <= 0) {
		return 0;
	}
	return (count * width) + ((count - 1) * skip);
}

[[nodiscard]] inline int EnhanceFittedWidth(
		int span,
		int count,
		int preferred,
		int minWidth,
		int skip) {
	if (count <= 0 || span <= 0 || preferred <= 0) {
		return 0;
	}
	if (EnhanceButtonsWidth(count, preferred, skip) <= span) {
		return preferred;
	}
	const auto gaps = (count > 1) ? ((count - 1) * skip) : 0;
	if (span <= gaps) {
		return 0;
	}
	const auto each = (span - gaps) / count;
	return (each >= minWidth) ? each : 0;
}

[[nodiscard]] inline int EnhanceButtonCapacity(
		int span,
		int width,
		int skip) {
	if (span <= 0 || width <= 0 || span < width) {
		return 0;
	}
	return (span + skip) / (width + skip);
}

inline void PlaceEnhanceRow(
		EnhanceBarSlot *slots,
		int first,
		int count,
		int width,
		int skip,
		int y,
		int height,
		int spanLeft,
		int spanRight,
		bool fromRight) {
	if (count <= 0 || width <= 0) {
		return;
	}
	const auto total = EnhanceButtonsWidth(count, width, skip);
	auto x = fromRight ? (spanRight - total) : spanLeft;
	for (auto i = 0; i != count; ++i) {
		auto &slot = slots[first + i];
		slot.x = x;
		slot.y = y;
		slot.width = width;
		slot.height = height;
		slot.shown = true;
		x += width + skip;
	}
}

[[nodiscard]] inline EnhanceBarLayout LayoutEnhanceBar(EnhanceBarInput in) {
	auto layout = EnhanceBarLayout();
	const auto count = (in.count < 0) ? 0 : ((in.count > 4) ? 4 : in.count);
	if (!count || in.preferredWidth <= 0 || in.minWidth <= 0) {
		return layout;
	}
	const auto rightLeft = in.playRight + in.guard;
	const auto rightRight = in.clusterLeft - in.guard;
	const auto leftLeft = in.volumeRight + in.guard;
	const auto leftRight = in.playLeft - in.guard;
	const auto rightSpan = EnhanceSpanWidth(rightLeft, rightRight);
	const auto leftSpan = EnhanceSpanWidth(leftLeft, leftRight);
	const auto preferred = (in.preferredWidth < in.minWidth)
		? in.minWidth
		: in.preferredWidth;
	if (const auto width = EnhanceFittedWidth(
			rightSpan,
			count,
			preferred,
			in.minWidth,
			in.skip)) {
		PlaceEnhanceRow(
			layout.slot,
			0,
			count,
			width,
			in.skip,
			in.rowTop,
			in.rowHeight,
			rightLeft,
			rightRight,
			true);
		return layout;
	}
	auto nRight = EnhanceButtonCapacity(rightSpan, in.minWidth, in.skip);
	auto nLeft = EnhanceButtonCapacity(leftSpan, in.minWidth, in.skip);
	if (nRight > count) {
		nRight = count;
	}
	if (nLeft > count - nRight) {
		nLeft = count - nRight;
	}
	if (nLeft > 0) {
		PlaceEnhanceRow(
			layout.slot,
			0,
			nLeft,
			in.minWidth,
			in.skip,
			in.rowTop,
			in.rowHeight,
			leftLeft,
			leftRight,
			false);
	}
	if (nRight > 0) {
		PlaceEnhanceRow(
			layout.slot,
			count - nRight,
			nRight,
			in.minWidth,
			in.skip,
			in.rowTop,
			in.rowHeight,
			rightLeft,
			rightRight,
			true);
	}
	return layout;
}

[[nodiscard]] inline int EnhanceBarShownCount(const EnhanceBarLayout &layout) {
	auto count = 0;
	for (const auto &slot : layout.slot) {
		if (slot.shown) {
			++count;
		}
	}
	return count;
}

[[nodiscard]] inline bool EnhanceBarOnSpeedRow(
		const EnhanceBarLayout &layout,
		int rowTop,
		int rowHeight) {
	for (const auto &slot : layout.slot) {
		if (!slot.shown) {
			continue;
		}
		if (slot.y != rowTop || slot.height != rowHeight) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline bool EnhanceRangesOverlap(
		int a,
		int aWidth,
		int b,
		int bWidth) {
	return aWidth > 0
		&& bWidth > 0
		&& a < (b + bWidth)
		&& b < (a + aWidth);
}

[[nodiscard]] inline bool EnhanceBarOverlapsCore(
		const EnhanceBarLayout &layout,
		const ViewerControlRects &core) {
	for (const auto &slot : layout.slot) {
		if (!slot.shown) {
			continue;
		}
		if (EnhanceRangesOverlap(slot.x, slot.width, 0, core.volumeRight)
			|| EnhanceRangesOverlap(
				slot.x,
				slot.width,
				core.playLeft,
				core.playRight - core.playLeft)
			|| EnhanceRangesOverlap(
				slot.x,
				slot.width,
				core.clusterLeft,
				core.playRight + core.clusterLeft)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline bool EnhanceBarOverlapsBand(
		const EnhanceBarLayout &layout,
		int bandTop,
		int bandBottom) {
	for (const auto &slot : layout.slot) {
		if (!slot.shown) {
			continue;
		}
		const auto bottom = slot.y + slot.height;
		if (slot.y < bandBottom && bottom > bandTop) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline int EnhanceNearestEdge(
		int from,
		int fallback,
		const EnhanceBarLayout &layout) {
	auto edge = fallback;
	for (const auto &slot : layout.slot) {
		if (slot.shown && slot.x >= from && slot.x < edge) {
			edge = slot.x;
		}
	}
	return edge;
}

struct DownloadLabelPlace {
	int x = 0;
	int y = 0;
	bool shown = false;
};

[[nodiscard]] inline DownloadLabelPlace PlaceDownloadLabel(
		int labelWidth,
		int labelHeight,
		int anchorTop,
		int anchorHeight,
		const ViewerControlRects &core,
		const EnhanceBarLayout &enhance) {
	const auto y = anchorTop + ((anchorHeight - labelHeight) / 2);
	const auto trySpan = [&](int left, int right) -> DownloadLabelPlace {
		if (labelWidth <= 0 || (right - left) < labelWidth) {
			return {};
		}
		const auto x = left + ((right - left - labelWidth) / 2);
		for (const auto &slot : enhance.slot) {
			if (slot.shown
				&& EnhanceRangesOverlap(x, labelWidth, slot.x, slot.width)) {
				return {};
			}
		}
		return {
			.x = x,
			.y = y,
			.shown = true,
		};
	};
	if (const auto placed = trySpan(
			core.playRight,
			EnhanceNearestEdge(core.playRight, core.clusterLeft, enhance))
		; placed.shown) {
		return placed;
	}
	if (const auto placed = trySpan(
			core.volumeRight,
			EnhanceNearestEdge(core.volumeRight, core.playLeft, enhance))
		; placed.shown) {
		return placed;
	}
	return {
		.y = y,
	};
}

struct BarRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};

[[nodiscard]] inline bool BarContains(BarRect rect, int x, int y) {
	return rect.w > 0
		&& rect.h > 0
		&& x >= rect.x
		&& y >= rect.y
		&& x < (rect.x + rect.w)
		&& y < (rect.y + rect.h);
}

[[nodiscard]] inline bool IncludeControlBarInHdrCutout(bool controlsShown) {
	return controlsShown;
}

[[nodiscard]] inline bool HdrOverlayBlocksControlPoint(
		BarRect video,
		BarRect controlBar,
		bool controlsShown,
		int x,
		int y) {
	if (!BarContains(video, x, y)) {
		return false;
	}
	if (IncludeControlBarInHdrCutout(controlsShown)
		&& BarContains(controlBar, x, y)) {
		return false;
	}
	return true;
}

} // namespace Media::View
