/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/global_media/info_global_media_inner_widget.h"
#include "ui/restricted_search_status.h"

#include "info/global_media/info_global_media_provider.h"
#include "info/global_media/info_global_media_widget.h"
#include "info/media/info_media_empty_widget.h"
#include "info/media/info_media_list_widget.h"
#include "info/info_controller.h"
#include "ui/widgets/labels.h"
#include "ui/search_field_controller.h"
#include "lang/lang_keys.h"
#include "styles/style_info.h"

namespace Info::GlobalMedia {

InnerWidget::InnerWidget(
	QWidget *parent,
	not_null<Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _empty(this) {
	_status = object_ptr<Ui::FlatLabel>(this, st::infoEmptyLabel);
	_status->hide();
	_empty->setType(type());
	_empty->heightValue(
	) | rpl::on_next(
		[this] { refreshHeight(); },
		_empty->lifetime());
	_list = setupList();
	_list->heightValue() | rpl::on_next([=] {
		const auto loading = _list->globalMediaRestrictedSearchLoading();
		_empty->setLoading(loading);
		_empty->setLoadedCount(loading
			? _list->globalMediaRestrictedSearchLoadedCount()
			: std::nullopt);
		if (!_list->globalMediaRestrictedSearchRetryable()) {
			_empty->setStatus(QString(), [] {});
		}
	}, _list->lifetime());
	_list->globalMediaSearchSnapshotValue(
	) | rpl::on_next([=](
			const std::optional<GlobalMediaSliceSnapshot> &snapshot) {
		_empty->setLoading(_list->globalMediaRestrictedSearchLoading());
		if (!snapshot) {
			_status->hide();
			_empty->setStatus(QString(), [] {});
			refreshHeight();
			return;
		}
		auto text = QString();
		if (snapshot->partial) {
			text = Ui::RestrictedSearchFailureText(snapshot->error);
		} else if (!snapshot->exactTotal
			&& (snapshot->hasMore
				|| _list->globalMediaRestrictedSearchLoading())) {
			text = tr::lng_search_loaded_results(
				tr::now,
				lt_count,
				snapshot->loadedCount);
		}
		_empty->setStatus(snapshot->partial
			? text
			: QString(), [=] {
				if (!_list->globalMediaRestrictedSearchCooldown()) {
					_list->retryGlobalMediaRestrictedSearch();
				}
			});
		_empty->setLoadedCount(snapshot->hasMore
			? std::make_optional(snapshot->loadedCount)
			: std::nullopt);
		_status->setText(text);
		auto filter = Ui::FlatLabel::ClickHandlerFilter();
		if (snapshot->partial) {
			filter = [=](const ClickHandlerPtr &, Qt::MouseButton) {
				if (!_list->globalMediaRestrictedSearchCooldown()) {
					_list->retryGlobalMediaRestrictedSearch();
				}
				return true;
			};
		}
		_status->setClickHandlerFilter(std::move(filter));
		_status->setCursor(snapshot->partial
			? style::cur_pointer
			: style::cur_default);
		_status->setVisible(!text.isEmpty()
			&& !snapshot->positions.empty());
		refreshHeight();
	}, _list->lifetime());
}

object_ptr<Media::ListWidget> InnerWidget::setupList() {
	auto result = object_ptr<Media::ListWidget>(this, _controller);

	// Setup list widget connections
	result->heightValue(
	) | rpl::on_next([this] {
		refreshHeight();
	}, result->lifetime());

	using namespace rpl::mappers;
	result->scrollToRequests(
	) | rpl::map([widget = result.data()](int to) {
		return Ui::ScrollToRequest{
			widget->y() + to,
			-1
		};
	}) | rpl::start_to_stream(
		_scrollToRequests,
		result->lifetime());

	_controller->searchQueryValue(
	) | rpl::on_next([this](const QString &query) {
		_empty->setSearchQuery(query);
		_empty->setStatus(QString(), [] {});
		_empty->setLoadedCount(std::nullopt);
		_empty->setLoading(_list->globalMediaRestrictedSearchLoading());
	}, result->lifetime());

	return result;
}

Storage::SharedMediaType InnerWidget::type() const {
	return _controller->section().mediaType();
}

void InnerWidget::visibleTopBottomUpdated(
		int visibleTop,
		int visibleBottom) {
	setChildVisibleTopBottom(
		_list,
		visibleTop - _list->y(),
		visibleBottom - _list->y());
}

bool InnerWidget::showInternal(not_null<Memento*> memento) {
	if (memento->section().type() == Section::Type::GlobalMedia
		&& memento->section().mediaType() == type()) {
		restoreState(memento);
		return true;
	}
	return false;
}

void InnerWidget::saveState(not_null<Memento*> memento) {
	_list->saveState(&memento->media());
}

void InnerWidget::restoreState(not_null<Memento*> memento) {
	_list->restoreState(&memento->media());
}

rpl::producer<SelectedItems> InnerWidget::selectedListValue() const {
	return _selectedLists.events_starting_with(
		_list->selectedListValue()
	) | rpl::flatten_latest();
}

void InnerWidget::selectionAction(SelectionAction action) {
	_list->selectionAction(action);
}

InnerWidget::~InnerWidget() = default;

int InnerWidget::resizeGetHeight(int newWidth) {
	_inResize = true;
	auto guard = gsl::finally([this] { _inResize = false; });

	_status->resizeToNaturalWidth(
		newWidth - 2 * st::infoEmptyLabelSkip);
	_list->resizeToWidth(newWidth);
	_empty->resizeToWidth(newWidth);
	return recountHeight();
}

void InnerWidget::refreshHeight() {
	if (_inResize) {
		return;
	}
	resize(width(), recountHeight());
}

int InnerWidget::recountHeight() {
	auto top = 0;
	auto listHeight = 0;
	if (_status && _status->isVisible()) {
		_status->moveToLeft(st::infoEmptyLabelSkip, top, width());
		top += _status->height();
	}
	if (_list) {
		_list->moveToLeft(0, top);
		listHeight = _list->heightNoMargins();
		top += listHeight;
	}
	if (listHeight > 0) {
		_empty->hide();
	} else {
		_empty->show();
		_empty->moveToLeft(0, top);
		top += _empty->heightNoMargins();
	}
	return top;
}

void InnerWidget::setScrollHeightValue(rpl::producer<int> value) {
	using namespace rpl::mappers;
	_empty->setFullHeight(rpl::combine(
		std::move(value),
		_listTops.events_starting_with(
			_list->topValue()
		) | rpl::flatten_latest(),
		_1 - _2));
}

rpl::producer<Ui::ScrollToRequest> InnerWidget::scrollToRequests() const {
	return _scrollToRequests.events();
}

} // namespace Info::GlobalMedia
