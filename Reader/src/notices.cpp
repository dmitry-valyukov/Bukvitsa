// Свой заголовок первым: он тянет за собой стандартные заголовки и сам
// кончается импортом wxl.async.
#include "notices.h"

#include "ShowDialog.h"

// Последним: слова читателю (`noticeOf`) несут импорт wxl.async, после чего
// стандартный заголовок MSVC уже не принимает.
#include "bukvitsa/reader/notice.h"

import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;

Notices::Notices(wxl::CompositionWindow window, observable<ElementTheme const>& screenTheme)
    : window_(std::move(window)), screenTheme_(screenTheme) {
    instance_ = this;
    previous_ = wxl::async::on_detached_task_failure();
    wxl::async::on_detached_task_failure() = [](std::exception_ptr error) noexcept {
        if (instance_) instance_->failed(std::move(error));
    };
}

Notices::~Notices() {
    if (instance_ != this) return;
    wxl::async::on_detached_task_failure() = previous_;
    instance_ = nullptr;
}

void Notices::post(Notice notice) {
    using namespace wxl::dsl;

    if (lines_) {
        // Диалог открыт — дописываем. Заголовок второго сообщения — строкой
        // в содержимом: title у диалога один, и он принадлежит первому.
        lines_.value().children().append(
            TextBlock{notice.headline, FontWeight{600}, Margin{0, 12, 0, 0}, textWrapping.wrap});
        if (!notice.details.empty()) lines_.value().children().append(TextBlock{notice.details, textWrapping.wrap});
        return;
    }

    // Над тем островом, что показан: диалог — не часть дерева, и без
    // XamlRoot показ падает; его ставит showDialog. Тема — показанного
    // экрана (screenTheme_): полоса тёмная, полка и стартовый экран светлые.
    UIElement const host = window_.content();
    if (!host) {
        // Показывать ещё не над чем: так падает чтение настроек и реестров
        // на старте, до первого экрана. Сообщение дождётся его (flush).
        pending_.push_back(std::move(notice));
        return;
    }

    auto lines = StackPanel{};
    if (!notice.details.empty()) lines.children().append(TextBlock{notice.details, textWrapping.wrap});

    auto dialog = ContentDialog{
        title = notice.headline,
        content = lines,
        closeButtonText = u"Закрыть",
        defaultButton = ContentDialogButton::Close,
        onClosed = [this](ContentDialog const&) { lines_ = nullptr; },
    };
    dialog.requestedTheme(screenTheme_.get());

    // Открыт чужой диалог — XAML откажет исключением. Зовут отсюда и из
    // noexcept-обработчика сбоев, так что отказ — не ошибка, а очередь.
    try {
        showDialog(dialog, host);
    } catch (...) {
        pending_.push_back(std::move(notice));
        return;
    }
    lines_ = lines;
}

void Notices::flush() {
    std::vector<Notice> waiting = std::move(pending_);
    pending_.clear();
    for (Notice& notice : waiting) post(std::move(notice));
}

void Notices::failed(std::exception_ptr error) noexcept {
    // Какими словами — решает модель (`noticeOf`); здесь только показ.
    post(noticeOf(std::move(error)));
}

}  // namespace bukvitsa::reader
