// Свой заголовок первым: он тянет за собой стандартные заголовки и сам
// кончается импортом wxl.async.
#include "notices.h"

#include "ShowDialog.h"

// Последним: модель сообщений импортирует wxl.core, после чего стандартный
// заголовок MSVC уже не принимает.
#include "bukvitsa/reader/workspace.h"

import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;

Notices::Notices(wxl::CompositionWindow window) : window_(std::move(window)) {
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

void Notices::complain(std::wstring headline, std::wstring details) {
    using namespace wxl::dsl;

    if (lines_) {
        // Диалог открыт — дописываем. Заголовок второго сообщения — строкой
        // в содержимом: title у диалога один, и он принадлежит первому.
        lines_.value().children().append(
            TextBlock{headline, FontWeight{600}, Margin{0, 12, 0, 0}, textWrapping.wrap});
        if (!details.empty()) lines_.value().children().append(TextBlock{details, textWrapping.wrap});
        return;
    }

    // Над тем островом, что показан: диалог — не часть дерева, и без
    // XamlRoot показ падает; его ставит showDialog. Тема — показанного
    // экрана: полоса тёмная, полка и стартовый экран светлые, а корни их
    // все — Grid.
    UIElement const host = window_.content();
    if (!host) {
        // Показывать ещё не над чем: так падает чтение настроек и реестров
        // на старте, до первого экрана. Сообщение дождётся его (flush).
        pending_.emplace_back(std::move(headline), std::move(details));
        return;
    }

    auto lines = StackPanel{};
    if (!details.empty()) lines.children().append(TextBlock{details, textWrapping.wrap});

    auto dialog = ContentDialog{
        title = headline,
        content = lines,
        closeButtonText = u"Закрыть",
        defaultButton = ContentDialogButton::Close,
        onClosed = [this](ContentDialog const&) { lines_ = nullptr; },
    };
    dialog.requestedTheme(host.try_as<FrameworkElement>().actualTheme());

    // Открыт чужой диалог — XAML откажет исключением. Зовут отсюда и из
    // noexcept-обработчика сбоев, так что отказ — не ошибка, а очередь.
    try {
        showDialog(dialog, host);
    } catch (...) {
        pending_.emplace_back(std::move(headline), std::move(details));
        return;
    }
    lines_ = lines;
}

void Notices::flush() {
    std::vector<std::pair<std::wstring, std::wstring>> waiting = std::move(pending_);
    pending_.clear();
    for (auto& [headline, details] : waiting) complain(std::move(headline), std::move(details));
}

void Notices::failed(std::exception_ptr error) noexcept {
    // Какими словами — решает модель (`noticeOf`); здесь только показ.
    Notice notice = noticeOf(std::move(error));
    complain(std::move(notice.headline), std::move(notice.details));
}

}  // namespace bukvitsa::reader
