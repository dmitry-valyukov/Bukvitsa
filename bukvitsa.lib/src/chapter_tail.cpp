#include <chrono>
#include <utility>

// Заголовки модели после всех стандартных: они ведут к импорту модулей книги,
// wxl.async и wxl.core, а стандартный заголовок после импорта MSVC уже не
// принимает. Поток страниц первым: он тянет за собой книгу и вёрстку с их
// стандартными заголовками.
#include "bukvitsa/reader/page_flow.h"
#include "bukvitsa/reader/chapter_tail.h"

namespace bukvitsa::reader {

namespace {

// Сколько времени досчёту отдаётся за раз. Порция кончается не раньше срока,
// а позже — на цену одного блока: разорвать вёрстку абзаца нечем. Самый
// дорогой блок из четырёх тестовых книг стоит 27 мс, так что худшая порция
// укладывается в те сто миллисекунд, дольше которых читалка не вправе не
// отвечать на ввод. Роман в 650 тысяч знаков проходит за шесть порций.
constexpr auto kSlice = std::chrono::milliseconds{50};

}  // namespace

ChapterTail::ChapterTail(PageFlow& flow, IdleScheduler& scheduler,
                         wxl::async::cancellation_token parent, function<void()> completed)
    : flow_(flow),
      scheduler_(scheduler),
      completed_(std::move(completed)),
      parent_(std::move(parent)),
      stop_(parent_) {}

ChapterTail::~ChapterTail() {
    stop_.cancel();
}

void ChapterTail::start() {
    if (running_ || stop_.is_canceled()) return;   // идёт — возьмёт и эту главу; или владелец ушёл
    if (!flow_.needsTail()) return;                // досчитывать нечего

    // Корутина доходит до первого ожидания ещё внутри вызова: кадр, который
    // уже ждёт простоя.
    running_ = true;
    loop();
}

void ChapterTail::restart() {
    stop_.cancel();
    running_ = false;
    stop_ = wxl::async::cancellation_source{parent_};
}

wxl::async::detached_task ChapterTail::loop() {
    // Правило кадра: отменённый, он не трогает `this` — ни знака, ни потока
    // страниц, ни получателя. Ожидание под отменённым токеном бросает отмену,
    // кадр по ней выходит, и это всё: к этому ходу очереди у держателя может
    // идти уже другой кадр (перевёрстка), а сам держатель может быть разрушен
    // (выход приложения). Поэтому всё, что трогает `this`, стоит либо до
    // первого ожидания (внутри start()), либо после ожидания, которое
    // кончилось не отменой.
    //
    // Порции продолжают счёт с курсора текущей главы, не начиная заново:
    // синхронно посчитанные страницы остаются на месте, добирается лишь хвост
    // ради общего числа страниц. Меняется от него только «из M» — и то один
    // раз, когда глава досчитана; до тех пор колонцифра показывает «из …», и
    // звать получателя по ходу незачем.
    //
    // Ожидание простоя — это и есть «в свободное время»: поток сперва разберёт
    // ввод и покажет нарисованное, а уже потом возьмётся за книгу. Поток без
    // книги отвечает на порцию «досчитывать нечего», и досчёт кончается.
    for (;;) {
        co_await scheduler_.idle(stop_.token());
        if (!flow_.advanceTail(kSlice)) break;
    }

    running_ = false;
    completed_();
}

}  // namespace bukvitsa::reader
