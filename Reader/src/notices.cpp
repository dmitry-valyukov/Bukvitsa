// Свой заголовок первым: он тянет за собой стандартные заголовки и сам
// кончается импортом wxl.async.
#include "notices.h"

#include "Failure.h"
#include "ShowDialog.h"

// Последним: слова читателю (`noticeOf`) несут импорт wxl.async, после чего
// стандартный заголовок MSVC уже не принимает.
#include "bukvitsa/reader/notice.h"

import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;

namespace {

/// Чем XAML отказывает показу, пока над островом открыт другой диалог:
/// E_ASYNC_OPERATION_NOT_STARTED (ERROR_CONTENTDIALOG_MULTIPLE_OPEN).
constexpr int32_t kAnotherDialogOpen = static_cast<int32_t>(0x80000019);

/// Сообщение, которому показаться уже не над чем, — строкой в stderr и в
/// вывод отладчика: заголовок и подробности.
void writeOut(const Notice& notice) {
    std::string line = "Сообщение, которое некому показать: ";
    unicode::append_utf8(line, unicode::repaired(notice.headline));
    if (!notice.details.empty()) {
        line += " — ";
        unicode::append_utf8(line, unicode::repaired(notice.details));
    }
    core::report(line);
}

}  // namespace

Notices::Notices(wxl::CompositionWindow window, observable<wxl::ElementTheme const>& screenTheme,
                 wxl::async::cancellation_token token)
    : window_(std::move(window)), screenTheme_(screenTheme), dialogStop_(std::move(token)) {}

Notices::~Notices() {
    // Не вышедшие из очереди — сообщения, которых читатель так и не увидел:
    // экрана не дождались или чужой диалог не закрылся до выхода. Терять их
    // нельзя и здесь.
    for (const Notice& notice : pending_) writeOut(notice);
}

void Notices::post(Notice notice) {
    using namespace wxl::dsl;

    // Окно закрыто: на выходе показать уже нечем — диалог под отменённым
    // токеном и не показался бы, — а терять нельзя: это сбой, отказ последней
    // записи или сценарий, упавший на выходе, и процесс выйдет с кодом 0.
    // Заголовок и подробности — строкой в stderr и в вывод отладчика.
    if (dialogStop_.is_canceled()) {
        writeOut(notice);
        return;
    }

    if (lines_) {
        // Диалог открыт — дописываем. Заголовок второго сообщения — строкой
        // в содержимом: title у диалога один, и он принадлежит первому. Тот
        // же заголовок, что у последнего, — только подробности: строки одного
        // рода сходятся под ним.
        if (notice.headline != lastHeadline_) {
            lines_.value().children().append(
                TextBlock{notice.headline, FontWeight{600}, Margin{0, 12, 0, 0}, textWrapping.wrap});
            lastHeadline_ = notice.headline;
            if (!notice.details.empty())
                lines_.value().children().append(TextBlock{notice.details, textWrapping.wrap});
            return;
        }

        // Под тем же заголовком — отступом в строку: подробности сообщения
        // бывают сами из нескольких строк («путь», пустая, «причина»), и
        // соседние без зазора слились бы в одно.
        if (!notice.details.empty())
            lines_.value().children().append(TextBlock{notice.details, Margin{0, 12, 0, 0}, textWrapping.wrap});
        return;
    }

    // Ждёт показа сообщение с тем же заголовком — подробности дописываются
    // в него, и выйдет оно одним.
    const auto mergeIntoPending = [this](Notice& arrived) {
        if (pending_.empty() || pending_.back().headline != arrived.headline) return false;
        if (!arrived.details.empty()) {
            // Пустой строкой: подробности бывают сами из нескольких строк
            // («путь», пустая, «причина»), и без зазора слились бы.
            if (!pending_.back().details.empty()) pending_.back().details += L"\n\n";
            pending_.back().details += arrived.details;
        }
        return true;
    };

    // Над тем островом, что показан: диалог — не часть дерева, и без
    // XamlRoot показ падает; его ставит showDialog. Тема — показанного
    // экрана (screenTheme_): полоса тёмная, полка и стартовый экран светлые.
    UIElement const host = window_.content();
    if (!host) {
        // Показывать ещё не над чем: так падает чтение настроек и реестров
        // на старте, до первого экрана. Сообщение дождётся его (flush).
        if (!mergeIntoPending(notice)) pending_.push_back(std::move(notice));
        return;
    }

    auto lines = StackPanel{};
    if (!notice.details.empty()) lines.children().append(TextBlock{notice.details, textWrapping.wrap});

    auto dialog = ContentDialog {
        title = notice.headline,
        content = lines,
        closeButtonText = u"Закрыть",
        defaultButton = ContentDialogButton::Close,
        onClosed += [this](ContentDialog const&) {
            lines_ = nullptr;
            lastHeadline_.clear();
        },
    };
    dialog.requestedTheme(screenTheme_.get());

    // Открыт чужой диалог (вопрос об удалении обложки) — XAML отказывает
    // показу сразу, и showDialog отдаёт отказ исключением из вызова. Отказ —
    // не ошибка, а очередь: сообщение выйдет, когда вопрос закроется (flush).
    // Иное исключение — не отказ, и прятать его в очередь нельзя: оно идёт
    // дальше, тому, кто сообщение публикует.
    // Ответа диалога ждёт задача под токеном сообщений: выход её тело видит, а
    // закрытие окна закрывает диалог.
    try {
        dialogWait_ = showDialog(dialog, host, dialogStop_.token());
    } catch (...) {
        if (failureOf(std::current_exception()).hresult != kAnotherDialogOpen) throw;
        if (!mergeIntoPending(notice)) pending_.push_back(std::move(notice));
        return;
    }
    lines_ = lines;
    lastHeadline_ = notice.headline;
}

void Notices::flush() {
    std::vector<Notice> waiting = std::move(pending_);
    pending_.clear();
    for (Notice& notice : waiting) post(std::move(notice));
}

}  // namespace bukvitsa::reader
