#include <algorithm>
#include <optional>
#include <utility>

// Свой заголовок после стандартных: он ведёт к импорту модулей книги,
// wxl.async и wxl.core, а стандартный заголовок после импорта MSVC уже не
// принимает.
#include "bukvitsa/reader/book_presenter.h"

import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

// Досчёт главы — от своего источника: от него же его отменяет деструктор.
// Получатель «глава досчитана» перерисовывает разворот — «из …» становится
// «из M»; без холста он не зовётся: досчёт заводит только вёрстка, а верстает
// ведущий под холстом, и уход холста досчёт отменяет. Сбои досчёта и очереди
// картинок — владельцу: он скажет о них читателю.
//
// Настройки вида приходят из модели, а не ставятся снаружи: ведущий верстает
// по ним с первой минуты, слушает поля и перевёрстывается сам; то же поле
// двигает ползунок панели, а колесо и клавиши пишут в него же — и панель
// узнаёт о них так же. Интерлиньяж и поля в модели в процентах, у потока
// страниц — множитель и доля.
BookPresenter::BookPresenter(Settings& settings, ThemeList& themes, IdleScheduler& idleScheduler,
                             Actions& actions, wxl::async::cancellation_token ownerToken)
    : settings_(settings),
      themes_(themes),
      actions_(actions),
      stop_(std::move(ownerToken)),
      chapterTail_(flow_, idleScheduler, stop_.token(), [this] { canvas().redraw(); },
                   [this](std::exception_ptr failure) { actions_.reportFailure(std::move(failure)); }),
      pictureQueue_(flow_, idleScheduler, stop_.token(), [this] { canvas().redraw(); },
                    [this](std::exception_ptr failure) { actions_.reportFailure(std::move(failure)); }),
      fontSizeWatch_(settings_.fontSize, *this),
      lineHeightWatch_(settings_.lineHeight, *this),
      marginWatch_(settings_.margin, *this) {
    flow_.setStyle(static_cast<float>(settings_.fontSize.get()),
                   static_cast<float>(settings_.lineHeight.get()) / 100.0f,
                   static_cast<float>(settings_.margin.get()) / 100.0f);

    // Смена темы — своя перекраска; поле своё, его слушатель умрёт вместе с
    // ведущим, снимать нечего.
    static_cast<void>(
        theme_.on_change([this](int themeIndex) noexcept { themeChanged(themeIndex); }));
}

BookPresenter::~BookPresenter() {
    // Холст держит ведущего ссылкой и уходит раньше него: остаться под
    // умирающим ведущим значило бы позвать его потом из своих обработчиков.
    if (canvas_) wxl::core::abort("BookPresenter: ведущий умирает, а холст под ним остался");

    // Ожидание простоя досчёта ведущему больше не нужно: отменяет его
    // владелец, то есть он сам, а стоящие кадры досчёта и очереди картинок
    // останавливают их держатели, уходя членами. Слушатели настроек снимаются
    // сами, первыми из членов.
    stop_.cancel();
}

/* ---------------- холст ---------------- */

BookPresenter::CanvasAttachment::~CanvasAttachment() {
    attachedPresenter_.detachCanvas();
}

BookPresenter::CanvasAttachment BookPresenter::attachCanvas(PageCanvas& canvas) {
    if (canvas_) wxl::core::abort("BookPresenter: второй холст поверх первого");
    canvas_ = &canvas;
    return CanvasAttachment{*this};
}

void BookPresenter::detachCanvas() {
    // Связь неперемещаема и уходит одна на приход, так что сюда без холста не
    // попасть; попали — сломано само устройство связи.
    if (!canvas_) wxl::core::abort("BookPresenter: уходит холст, которого не было");
    canvas_.reset();

    // Сноска жила на ушедшем холсте и ушла вместе с ним; новый холст приходит
    // без неё, и щелчок под ним не должен тратиться на прятанье того, чего нет.
    noteOpen_ = false;

    // Досчёт, досчитав главу, попросил бы перерисовки — у холста, которого
    // больше нет; очередь картинок — так же. Отменённые кадры уйдут на своём
    // ходу очереди, ничего не тронув; новые заведёт вёрстка под новым холстом.
    chapterTail_.restart();
    pictureQueue_.restart();
}

PageCanvas& BookPresenter::canvas() {
    // Рисовать без холста нечем, а промолчать значило бы показать читателю не
    // то, что решено: всякая просьба к холсту стоит после его прихода и до
    // ухода — нарушение этого есть ошибка проводки.
    if (!canvas_) wxl::core::abort("BookPresenter: просьба к холсту, которого нет");
    return **canvas_;
}

/* ---------------- книга и место чтения ---------------- */

void BookPresenter::open(not_null<Book> book, uint32_t charOffset) {
    hideNote();   // сноска — из прежней книги

    // Картинки прежней книги больше не нужны: очередь перезаводится, пока
    // поток ещё на ней, — ушедшее раскодирование забывает прежняя книга.
    // Картинки новой попросит вёрстка (relayout).
    pictureQueue_.restart();
    flow_.open(book, charOffset);
    relayout(false);
    position_.set(flow_.position());
}

void BookPresenter::goToCharOffset(uint32_t charOffset) {
    if (!flow_.isOpen()) return;

    // Прыжок по закладке, оглавлению или находке поиска — это разрыв ленты, а
    // не листание: разворот встаёт с колонки, где лежит место, без анимации
    // перехода. Порядок — как в перевороте и перевёрстке: сперва холст своё
    // (доводит перевороты до конца, прячет сноску), потом шаг книги. А
    // columnOf — уже шаг книги, а не справка: он верстает главу до места,
    // может сменить текущую и сбросить разворот.
    canvas().finishTurns();
    hideNote();

    // Нужную главу columnOf верстает до места сам; досчёт прежней главы, если
    // он шёл, просто продолжится по новой текущей.
    const PageFlow::Column targetColumn = flow_.columnOf(charOffset);
    flow_.show(targetColumn);
    chapterTail_.start();      // глава могла смениться — её хвост тоже нужен
    pictureQueue_.request(WindowMove::Jumped);   // разворот сменился — его картинки тоже

    canvas().redraw();
    position_.set(flow_.position());
}

/* ---------------- экран и мера ---------------- */

void BookPresenter::setActive(bool active) {
    canvas().activate(active);

    // Входя, холст показывает страницы, и одеть их надо в нынешний разворот:
    // пока полосы не было на экране, его могли сменить — книгу открыли, тему
    // переключили.
    if (active) canvas().redraw();
}

void BookPresenter::resize(float width, float height, float scale) {
    flow_.resize(width, height);
    scale_ = scale;

    // Пустая мера — окно ещё не показано или свёрнуто: верстать и рисовать
    // некуда.
    if (width <= 0.0f || height <= 0.0f) return;

    // Перевёрстка первым делом доводит до конца всё, что летит: окна кроя
    // листов заданы в прежних числах, и доигрывать перевороты по новым
    // размерам значило бы листать вслепую. Гистерезис числа колонок — только
    // здесь: окно тянут мышью, граница меры проходит под курсором, и без него
    // колонки защёлкают. Знака хватает — дрожь бывает в доли знака, а не в
    // четыре.
    relayout(true);
}

void BookPresenter::relayout(bool windowResize) {
    // Всякая новая вёрстка отменяет досчёт, который шёл: считать книгу по
    // прежней полосе больше незачем. Отменённый кадр уйдёт сам на ходу
    // очереди, новый заведётся ниже.
    chapterTail_.restart();

    // Перевёрстка меняет и корешок, и ширину страницы, а листы могли остаться
    // от прерванных переворотов с окнами кроя, посчитанными по прежним числам.
    // Все идущие перевороты — в покой.
    canvas().finishTurns();

    // Глава до видимого разворота — начисто, тем же кадром; место чтения
    // перевёрстка не двигает (PageFlow::relayout). Без книги или меры вёрстки
    // нет — рисуется приглашение или ничего.
    const bool laidOut = flow_.relayout(windowResize);
    canvas().redraw();

    // Остаток главы — порциями в свободное время потока: с него узнаётся общее
    // число страниц.
    if (!laidOut) return;
    chapterTail_.start();

    // Картинки нового разворота — в очередь; раскодирование, которое уже идёт,
    // перевёрстка не отменяет: место чтения то же, и окно с ним.
    pictureQueue_.request(WindowMove::Stays);
}

/* ---------------- листание ---------------- */

void BookPresenter::turnPage(bool forward) {
    // Следующий разворот ленты от нынешнего места. Лента непрерывна, так что
    // это обычный шаг: границу главы он проходит сам, не прыжком. Место чтения
    // и страницы двигаются сразу, а лист летит вдогонку — поэтому быстрые
    // нажатия в одну сторону пускают несколько листов внахлёст, а не ждут в
    // очереди; встречное холст сперва доводит до конца (turnStarting).
    const std::optional<PageFlow::Column> targetColumn = flow_.turnTarget(forward);
    if (!targetColumn) return;   // нет книги или край книги — листать некуда

    startTurn(*targetColumn, forward);
}

void BookPresenter::startTurn(const PageFlow::Column& targetColumn, bool forward) {
    hideNote();   // страница ушла, а сноска на ней осталась бы висеть

    // Порядок держится инвариантом, и он важнее анимации: уходящий разворот
    // холст берёт, пока книга ещё стоит на нём; книга шагает; новый разворот
    // рисуется ровно один раз — туда, где он теперь живёт, — и только потом
    // что-нибудь начинает двигаться. Как выглядит переворот, решает холст — до
    // шага и после; что он делает, решает ведущий.
    canvas().turnStarting(forward);

    // Двигаем книгу на целевой разворот. Целевая колонка может лежать в
    // соседней главе: поток делает её текущей, не теряя вёрстки (лента
    // разложила её как соседнюю на стыке), ставит место чтения и собирает
    // новый разворот.
    flow_.show(targetColumn);
    chapterTail_.start();   // вошли в соседнюю главу — её «из M» тоже надо досчитать

    // Картинки нового разворота — первыми, соседи — сначала по ходу
    // листания. Идущее раскодирование отменяется, только если его картинка
    // была соседом позади и выпала из окна.
    pictureQueue_.request(forward ? WindowMove::TurnedForward : WindowMove::TurnedBackward);

    // Новый разворот — единственный рисунок переворота. Без анимации на этом
    // переворот и кончается — ровно как прыжок (goToCharOffset).
    canvas().redraw();

    canvas().turnStarted(forward);

    // Место чтения — при нажатии, а не при посадке листа.
    position_.set(flow_.position());
}

void BookPresenter::renderingDeviceReplaced() {
    // Битмапы картинок жили на прежнем устройстве; книга отпускает их, а
    // очередь раскодирует картинки окна заново из сжатых байтов — разворот,
    // нарисованный следом, до них стоит с пустыми местами.
    Book* const shownBook = flow_.book();
    if (!shownBook) return;
    shownBook->dropDeviceBitmaps();
    pictureQueue_.request(WindowMove::Stays);
}

/* ---------------- ввод ---------------- */

KeyContext BookPresenter::pointerContext() const {
    return KeyContext {
        .shown = Screen::Book,
        .wizardOpen = previewing(),
        .noteOpen = noteOpen(),
        .bookOpen = isOpen(),
    };
}

void BookPresenter::perform(Command command) {
    switch (command) {
        case Command::TurnForward:
            turnPage(true);
            return;
        case Command::TurnBackward:
            turnPage(false);
            return;

        // Начало и конец карта отдаёт только при открытой книге: конец
        // известен лишь ей, и чистовой набор доводится до самого конца.
        case Command::GoToStart:
        case Command::GoToEnd:
            if (!flow_.isOpen())
                wxl::core::abort("BookPresenter::perform: начало или конец книги без книги");
            goToCharOffset(command == Command::GoToStart ? 0u : flow_.book()->characterCount());
            return;

        // Кегль — в настройки: к ведущему перемена вернётся слушателем, как и
        // от ползунка панели.
        case Command::FontLarger:
            nudgeFontSize(kFontSizeStep);
            return;
        case Command::FontSmaller:
            nudgeFontSize(-kFontSizeStep);
            return;
        case Command::FontReset:
            settings_.fontSize.set(kFontSizeDefault);
            return;

        case Command::NextTheme:
            setTheme(theme_.get() + 1);
            return;
        case Command::DismissNote:
            hideNote();
            return;

        case Command::ScrollNoteLineDown:
            scrollNote(NoteScroll::LineDown);
            return;
        case Command::ScrollNoteLineUp:
            scrollNote(NoteScroll::LineUp);
            return;
        case Command::ScrollNotePageDown:
            scrollNote(NoteScroll::PageDown);
            return;
        case Command::ScrollNotePageUp:
            scrollNote(NoteScroll::PageUp);
            return;
        case Command::ScrollNoteToStart:
            scrollNote(NoteScroll::ToStart);
            return;
        case Command::ScrollNoteToEnd:
            scrollNote(NoteScroll::ToEnd);
            return;

        default:
            break;
    }

    // Экраны, ящики и полный экран — приложения, а `None` исполнять нечего:
    // дошедшая сюда — ошибка проводки, а не клавиша, которую можно пропустить.
    wxl::core::abort("BookPresenter::perform: команда не полосы");
}

bool BookPresenter::wheel(int wheelDelta, bool controlHeld) {
    const Command command = resolveWheel(wheelDelta, controlHeld, pointerContext());
    if (command == Command::None) return false;

    perform(command);
    return true;
}

void BookPresenter::tap(PageFlow::Point tapPoint, PointerButton pressedButton) {
    const Command command = resolveTap(tapPoint.x, flow_.width(), pressedButton, pointerContext());

    // Поверх полосы лежит мастер: из всего щелчка остаётся листание по
    // третям — ни ящика, ни сносок, ни прятанья открытой. Сюда доходят только
    // щелчки мимо точек сетки: попавшие мастер разобрал сам.
    if (preview_) {
        if (command != Command::None) perform(command);
        return;
    }

    // Правая кнопка — единственная дорога к ящику для того, кто держит мышь:
    // у страницы нет ни полосы меню, ни кнопок, и заводить их ради этого
    // значило бы завесить книгу обстановкой. Панель полосе не принадлежит —
    // её открывает владелец.
    if (command == Command::TogglePanel) {
        actions_.togglePanel();
        return;
    }

    // Знак сноски важнее перелистывания: он мелкий, и промах по нему из-за
    // того, что страница уже перевернулась, читателя злит. Знак под точкой —
    // тот же, над которым указатель становится рукой (pointsAtNoteMark).
    if (const std::optional<PageFlow::NoteHit> noteMark = noteMarkAt(tapPoint)) {
        noteOpen_ = canvas().showNote(*noteMark);
        return;
    }

    // Щелчок мимо знака закрывает то, что открыто поверх полосы: читатель
    // прочёл примечание и вернулся к книге.
    if (noteOpen_) {
        hideNote();
        return;
    }

    if (command != Command::None) perform(command);
}

bool BookPresenter::pointsAtNoteMark(PageFlow::Point hoverPoint) const {
    return noteMarkAt(hoverPoint).has_value();
}

std::optional<PageFlow::NoteHit> BookPresenter::noteMarkAt(PageFlow::Point point) const {
    // Под мастером сносок нет: поверх страницы лежит он, и щелчок по ней —
    // только листание. Без книги поток отвечает пусто сам.
    if (preview_) return std::nullopt;
    return flow_.noteAt(point);
}

void BookPresenter::hideNote() {
    if (!noteOpen_) return;
    noteOpen_ = false;
    canvas().hideNote();
}

void BookPresenter::scrollNote(NoteScroll scroll) {
    // Прокрутку карта отдаёт только при открытой сноске; без неё холсту
    // прокручивать нечего, а промолчать значило бы съесть клавишу, которую
    // карта решила по неправде.
    if (!noteOpen_) wxl::core::abort("BookPresenter::perform: прокрутка сноски без сноски");
    canvas().scrollNote(scroll);
}

/* ---------------- кегль, интерлиньяж, поля ---------------- */

void BookPresenter::nudgeFontSize(double sizeStep) {
    settings_.fontSize.set(
        std::clamp(settings_.fontSize.get() + sizeStep, kFontSizeMin, kFontSizeMax));
}

void BookPresenter::fontSizeChanged(double fontSize) noexcept {
    if (flow_.setStyle(static_cast<float>(fontSize), flow_.lineHeight(), flow_.marginFraction()))
        relayout(false);
}

void BookPresenter::lineHeightChanged(double leadingPercent) noexcept {
    if (flow_.setStyle(flow_.fontSize(), static_cast<float>(leadingPercent) / 100.0f,
                       flow_.marginFraction()))
        relayout(false);
}

void BookPresenter::marginChanged(double marginPercent) noexcept {
    if (flow_.setStyle(flow_.fontSize(), flow_.lineHeight(),
                       static_cast<float>(marginPercent) / 100.0f))
        relayout(false);
}

/* ---------------- тема и предпросмотр ---------------- */

void BookPresenter::setTheme(int themeIndex) {
    // Приводится к списку по кругу и кладётся в поле; всё, что следует за
    // сменой темы, делает слушатель (themeChanged) — так же, как если бы поле
    // поменял кто-то другой. Та же тема, что и была, слушателя не зовёт.
    theme_.set(themes_.normalize(themeIndex));
}

void BookPresenter::themeChanged(int) noexcept {
    hideNote();   // подложка всплывашки покрашена прошлой темой
    canvas().restyle();
    canvas().redraw();
}

void BookPresenter::skinsChanged(int themeIndex) {
    // Другой номер перекрасит и перерисует слушатель темы (themeChanged). На
    // тот же номер слушатель молчит, а кривые под ним могли смениться —
    // пересобрать вид и перерисовать здесь.
    if (themes_.normalize(themeIndex) != theme_.get()) {
        setTheme(themeIndex);
        return;
    }
    canvas().restyle();
    canvas().redraw();
}

void BookPresenter::setPreview(const Skin* previewSkin) {
    if (previewSkin) {
        preview_ = *previewSkin;
    } else {
        preview_.reset();
    }

    // Карта изгиба держит форму прежних кривых, тени — прежнюю бумагу.
    canvas().restyle();
    canvas().redraw();
}

const Theme& BookPresenter::paper() const {
    if (preview_) return kSkinTheme;
    return themes_.paperAt(theme_.get());
}

const Skin* BookPresenter::pageSkin() const {
    if (preview_) return &*preview_;
    return activeSkin();
}

}  // namespace bukvitsa::reader
