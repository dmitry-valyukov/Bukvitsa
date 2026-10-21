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
                         wxl::async::cancellation_token ownerToken, function<void()> completionReceiver,
                         function<void(std::exception_ptr)> failureReceiver)
    : flow_(flow),
      scheduler_(scheduler),
      completionReceiver_(std::move(completionReceiver)),
      failureReceiver_(std::move(failureReceiver)),
      ownerToken_(std::move(ownerToken)),
      stop_(ownerToken_) {}

ChapterTail::~ChapterTail() {
    stop_.cancel();
}

void ChapterTail::start() {
    if (running_ || stop_.is_canceled()) return;   // идёт — возьмёт и эту главу; или владелец ушёл
    if (!flow_.needsTail()) return;

    // Знак — до вызова: кадр может кончиться ещё внутри него и сам опустить
    // знак (простой, который отвечает сразу, не вставая, — порции идут подряд
    // до конца главы; или сбой на первом же ожидании); поднятый после вызова,
    // знак затёр бы это и остался бы поднятым навсегда.
    running_ = true;

    // Зовут из получателя — кадр, который его зовёт, на стеке, и задачу под ним
    // не сменить: он сам пойдёт на новый круг.
    if (inReceiver_) {
        startAgain_ = true;
        return;
    }

    loop_ = loop();
}

void ChapterTail::restart() {
    stop_.cancel();
    running_ = false;
    startAgain_ = false;
    stop_ = wxl::async::cancellation_source{ownerToken_};
}

wxl::async::task<> ChapterTail::loop() {
    // Правило кадра: отменённый, он не трогает ни потока страниц, ни
    // получателей — только опускает свои знаки. Ожидание под отменённым
    // токеном бросает отмену, кадр по ней выходит. Другого кадра, чьи знаки
    // он затёр бы, к этому ходу нет: кадр живёт в поле задачи держателя, и
    // новый, присвоенный поверх, прежний останавливает.
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
        try {
            for (;;) {
                co_await scheduler_.idle(stop_.token());
                if (!flow_.advanceTail(kSlice)) break;
            }

            running_ = false;
            inReceiver_ = true;
            completionReceiver_();
            inReceiver_ = false;
        } catch (const wxl::async::operation_canceled_exception&) {
            // Отменённый кадр уходит. Страж получателя снимается и здесь:
            // отмену может бросить и сам получатель, и поднятый страж навсегда
            // обращал бы всякий start() в «ещё круг» кадра, которого нет.
            // Трогать `this` можно: кадр живёт только в поле держателя, и
            // другого кадра, чьи знаки он затёр бы, в этот миг нет — прежний
            // присвоение поверх уже остановило.
            running_ = false;
            startAgain_ = false;
            inReceiver_ = false;
            co_return;
        } catch (...) {
            // Сбой — не отмена: знаки опускаются, иначе всякий следующий start()
            // молча выходил бы и «из …» висело бы до перевёрстки; сам сбой —
            // получателю сбоев, под тем же стражем, что и получатель конца:
            // попросит он досчёт снова (restart и start) — задачу под бегущим
            // кадром не сменить, и кадр идёт на новый круг сам. Трогать `this`
            // здесь можно: не-отмену бросает только неотменённый кадр, а
            // неотменённый кадр — кадр живого держателя: умирая, держатель
            // уносит кадр полем задачи, а бросок идёт в том же ходу потока, что
            // и тело кадра.
            running_ = false;
            startAgain_ = false;
            inReceiver_ = true;
            failureReceiver_(std::current_exception());
            inReceiver_ = false;
        }

        // Получатель перевёрстал и попросил досчёт снова — новый круг под
        // новым источником; не просил — кадр кончился.
        if (!std::exchange(startAgain_, false)) break;
    }
}

}  // namespace bukvitsa::reader
