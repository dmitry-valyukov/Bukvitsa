#include "bukvitsa/reader/key_map.h"

namespace bukvitsa::reader {

namespace {

/// Листание и Home/End — общие для полосы с мастером и без него. Ctrl их не
/// меняет: Ctrl+PgDn листает так же, как PgDn.
Command turning(const KeyPress& press, const KeyContext& context) {
    switch (press.key) {
        case Key::PageDown:
        case Key::Right:
        case Key::Down:
        case Key::Space:
            return Command::TurnForward;
        case Key::PageUp:
        case Key::Left:
        case Key::Up:
            return Command::TurnBackward;
        case Key::Home:
            return context.bookOpen ? Command::GoToStart : Command::None;
        case Key::End:
            // Конец книги известен только досчитанной; полоса доводит
            // чистовой набор до него сама, но нужна для этого книга.
            return context.bookOpen ? Command::GoToEnd : Command::None;
        default:
            return Command::None;
    }
}

/// Правила полосы (п. 1 матрицы). `taken` — полоса клавишу берёт, даже если
/// ничего не делает: тогда до приложения она не доходит.
struct StripAnswer {
    bool taken = false;
    Command command = Command::None;
};

StripAnswer strip(const KeyPress& press, const KeyContext& context) {
    if (context.shown != Screen::Book) return {};

    // Клавиша начинается у элемента в фокусе, а туннель приводит её к полосе
    // раньше него. Ползунок панели и поле ввода — поиск, имя обложки в
    // мастере — ходят по клавишам сами; PgUp/PgDn ни ползунок, ни
    // однострочное поле не читают — ими листают из любого фокуса.
    if (context.origin == FocusOrigin::Slider || context.origin == FocusOrigin::TextBox) {
        if (press.key != Key::PageDown && press.key != Key::PageUp) return {};
    }

    const Command turn = turning(press, context);
    const bool isTurning = turn != Command::None || press.key == Key::Home || press.key == Key::End;
    if (isTurning) return {true, turn};

    // Поверх полосы лежит мастер, но листание остаётся. Всё остальное —
    // Enter, Escape, тема — мастера.
    if (context.wizardOpen) return {};

    switch (press.key) {
        case Key::Add:
            return {true, press.control ? Command::FontLarger : Command::None};
        case Key::Subtract:
            return {true, press.control ? Command::FontSmaller : Command::None};
        case Key::Number0:
        case Key::NumberPad0:
            return {true, press.control ? Command::FontReset : Command::None};
        case Key::T:
            // Голая T меняет тему; Ctrl+T — оглавление, его разбирает
            // приложение.
            if (press.control) return {};
            return {true, Command::NextTheme};
        default:
            return {};   // не клавиша полосы: пусть идёт дальше
    }
}

/// Правила приложения (п. 2 матрицы): то, что полоса не взяла.
Command application(const KeyPress& press, const KeyContext& context) {
    // Пока открыт мастер обложек, клавиши экранов молчат: Escape увёл бы с
    // полосы прямо под ним. Дороги из мастера — его кнопки.
    if (context.wizardOpen) return Command::None;

    const bool reading = context.shown == Screen::Book;

    // Полка — отовсюду, а не только из книги: это единственная клавиша,
    // которой не мешает то, что сейчас на экране.
    if (press.control && press.key == Key::L)
        return context.shown != Screen::Library ? Command::ShowLibrary : Command::None;

    if (reading && press.control) {
        switch (press.key) {
            case Key::T: return Command::OpenContents;
            case Key::F: return Command::OpenSearch;
            case Key::B: return Command::OpenBookmarks;
            default:
                // Ctrl с чем-то другим — не наше: кегль разбирает сама полоса.
                return Command::None;
        }
    }

    switch (press.key) {
        case Key::F2:
            // Панель — только над книгой: над заставкой ей нечего показывать.
            return reading ? Command::TogglePanel : Command::None;
        case Key::F11:
            return Command::ToggleFullScreen;
        case Key::Escape:
            if (context.panelOpen) return Command::ClosePanel;
            if (reading && context.noteOpen) return Command::DismissNote;
            if (context.fullScreen) return Command::LeaveFullScreen;
            if (reading) {
                // Обратно туда, откуда книгу открыли: выбравший её на полке
                // ждёт полку, а не заставку.
                return context.cameFrom == Screen::Library ? Command::BackToLibrary
                                                           : Command::BackToStart;
            }
            if (context.shown == Screen::Library) return Command::BackToStart;
            return Command::None;
        default:
            return Command::None;
    }
}

}  // namespace

Command resolve(const KeyPress& press, const KeyContext& context) {
    const StripAnswer answer = strip(press, context);
    if (answer.taken) return answer.command;

    return application(press, context);
}

}  // namespace bukvitsa::reader
