// Тесты карты клавиш: матрица «клавиша × что на экране × откуда пришла».
// На экране ошибку клавиш видно только драйвером окна и экранным диктором;
// здесь каждое правило матрицы — строка проверки.

#include <cstdio>
#include <initializer_list>
#include <string>

#include "check.h"

#include "bukvitsa/reader/key_map.h"

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Над открытой книгой, открытой с заставки, ничего поверх, фокус на корне.
KeyContext reading() {
    KeyContext context;
    context.shown = Screen::Book;
    context.cameFrom = Screen::Start;
    context.bookOpen = true;
    return context;
}

KeyContext focusedOn(FocusOrigin origin) {
    KeyContext context = reading();
    context.origin = origin;
    return context;
}

KeyContext withWizard(FocusOrigin origin = FocusOrigin::Root) {
    KeyContext context = focusedOn(origin);
    context.wizardOpen = true;
    return context;
}

KeyContext onScreen(Screen screen) {
    KeyContext context;
    context.shown = screen;
    return context;
}

Command plain(Key key, const KeyContext& context) {
    return resolve(KeyPress{key, false}, context);
}

Command withControl(Key key, const KeyContext& context) {
    return resolve(KeyPress{key, true}, context);
}

constexpr FocusOrigin kOrigins[] = {FocusOrigin::Root, FocusOrigin::Button, FocusOrigin::Slider,
                                    FocusOrigin::TextBox};

const char* nameOf(FocusOrigin origin) {
    switch (origin) {
        case FocusOrigin::Root: return "корень";
        case FocusOrigin::Button: return "кнопка";
        case FocusOrigin::Slider: return "ползунок";
        case FocusOrigin::TextBox: return "поле ввода";
    }
    return "?";
}

std::string about(const char* what, FocusOrigin origin) {
    return std::string(what) + " — " + nameOf(origin);
}

/// PgUp/PgDn листают из любого фокуса: ни ползунок, ни однострочное поле их
/// не читают. Под мастером — тоже, и из поля имени обложки.
void testPagesTurnFromAnyFocus() {
    std::printf("\n=== клавиши: PgUp/PgDn из любого фокуса ===\n");

    for (const FocusOrigin origin : kOrigins) {
        check(plain(Key::PageDown, focusedOn(origin)) == Command::TurnForward,
              about("PgDn листает вперёд", origin));
        check(plain(Key::PageUp, focusedOn(origin)) == Command::TurnBackward,
              about("PgUp листает назад", origin));
        check(plain(Key::PageDown, withWizard(origin)) == Command::TurnForward,
              about("PgDn под мастером листает", origin));
    }

    check(withControl(Key::PageDown, reading()) == Command::TurnForward, "Ctrl+PgDn листает, как PgDn");
    check(plain(Key::PageDown, onScreen(Screen::Start)) == Command::None, "на заставке PgDn — ничего");
    check(plain(Key::PageDown, onScreen(Screen::Library)) == Command::None, "на полке PgDn — ничего");
}

/// Ползунок и поле ввода ходят по клавишам сами: стрелки, пробел, Home/End и
/// буквы — их. Кнопка — нет: из неё полоса берёт своё.
void testSliderAndTextBoxKeepTheirKeys() {
    std::printf("\n=== клавиши: ползунок и поле ввода — их ===\n");

    constexpr Key kTheirs[] = {Key::Right, Key::Left, Key::Down, Key::Up,
                               Key::Space, Key::Home, Key::End,  Key::T};

    for (const FocusOrigin origin : {FocusOrigin::Slider, FocusOrigin::TextBox}) {
        bool allTheirs = true;
        for (const Key key : kTheirs) allTheirs = allTheirs && plain(key, focusedOn(origin)) == Command::None;
        check(allTheirs, about("стрелки, пробел, Home/End и T — не полосы", origin));

        check(withControl(Key::Add, focusedOn(origin)) == Command::None,
              about("Ctrl+«+» пропадает: кегль — тем же ползунком", origin));
    }

    check(plain(Key::T, focusedOn(FocusOrigin::TextBox)) != Command::NextTheme,
          "«т» в поиске не меняет тему");

    check(plain(Key::Space, focusedOn(FocusOrigin::Button)) == Command::TurnForward,
          "пробел на кнопке листает: кнопку нажимает Enter");
    check(plain(Key::Right, focusedOn(FocusOrigin::Button)) == Command::TurnForward,
          "стрелка на кнопке листает");
    check(plain(Key::T, focusedOn(FocusOrigin::Button)) == Command::NextTheme, "T на кнопке — тема");

    // Клавиши приложения ползунок и поле не держат: полоса их пропускает, а
    // приложение берёт.
    KeyContext searching = focusedOn(FocusOrigin::TextBox);
    searching.panelOpen = true;
    check(plain(Key::Escape, searching) == Command::ClosePanel, "Escape из поля поиска закрывает панель");
    check(plain(Key::F2, focusedOn(FocusOrigin::Slider)) == Command::TogglePanel,
          "F2 из ползунка — ящики");
    check(withControl(Key::F, focusedOn(FocusOrigin::TextBox)) == Command::OpenSearch,
          "Ctrl+F из поля поиска — поиск");
}

/// Листание, Home/End, кегль и тема над книгой без мастера.
void testStripKeys() {
    std::printf("\n=== клавиши: листание, кегль, тема ===\n");

    bool forward = true;
    for (const Key key : {Key::PageDown, Key::Right, Key::Down, Key::Space})
        forward = forward && plain(key, reading()) == Command::TurnForward;
    check(forward, "вперёд: PgDn, →, ↓, пробел");

    bool backward = true;
    for (const Key key : {Key::PageUp, Key::Left, Key::Up})
        backward = backward && plain(key, reading()) == Command::TurnBackward;
    check(backward, "назад: PgUp, ←, ↑");

    check(plain(Key::Home, reading()) == Command::GoToStart, "Home — начало книги");
    check(plain(Key::End, reading()) == Command::GoToEnd, "End — конец книги");

    KeyContext empty = reading();
    empty.bookOpen = false;
    check(plain(Key::Home, empty) == Command::None && plain(Key::End, empty) == Command::None,
          "Home/End без книги — ничего");

    check(withControl(Key::Add, reading()) == Command::FontLarger, "Ctrl+«+» — кегль больше");
    check(withControl(Key::Subtract, reading()) == Command::FontSmaller, "Ctrl+«−» — кегль меньше");
    check(withControl(Key::Number0, reading()) == Command::FontReset, "Ctrl+0 — кегль по умолчанию");
    check(withControl(Key::NumberPad0, reading()) == Command::FontReset,
          "Ctrl+0 цифровой клавиатуры — кегль по умолчанию");
    check(plain(Key::Add, reading()) == Command::None && plain(Key::Number0, reading()) == Command::None,
          "«+» и «0» без Ctrl — ничего");

    check(plain(Key::T, reading()) == Command::NextTheme, "T без Ctrl — следующая тема");
    check(withControl(Key::T, reading()) == Command::OpenContents, "Ctrl+T над книгой — оглавление, не тема");
    check(withControl(Key::F, reading()) == Command::OpenSearch, "Ctrl+F над книгой — поиск");
    check(withControl(Key::B, reading()) == Command::OpenBookmarks, "Ctrl+B над книгой — закладки");

    check(plain(Key::T, onScreen(Screen::Start)) == Command::None, "T на заставке — ничего");
    check(withControl(Key::T, onScreen(Screen::Start)) == Command::None &&
              withControl(Key::F, onScreen(Screen::Library)) == Command::None,
          "Ctrl+T/F/B — только над книгой");
}

/// Под мастером — только листание и Home/End; дороги из мастера — его кнопки.
void testWizardKeepsOnlyTurning() {
    std::printf("\n=== клавиши: под мастером ===\n");

    check(plain(Key::Right, withWizard()) == Command::TurnForward &&
              plain(Key::Space, withWizard()) == Command::TurnForward &&
              plain(Key::Up, withWizard()) == Command::TurnBackward,
          "листание есть");
    check(plain(Key::Home, withWizard()) == Command::GoToStart &&
              plain(Key::End, withWizard()) == Command::GoToEnd,
          "Home/End есть");

    check(plain(Key::Escape, withWizard()) == Command::None, "Escape — ничего: увёл бы из-под мастера");
    check(plain(Key::F2, withWizard()) == Command::None, "F2 — ничего");
    check(plain(Key::F11, withWizard()) == Command::None, "F11 — ничего");
    check(plain(Key::T, withWizard()) == Command::None, "T — ничего");
    check(withControl(Key::L, withWizard()) == Command::None, "Ctrl+L — ничего");
    check(withControl(Key::T, withWizard()) == Command::None, "Ctrl+T — ничего");
    check(withControl(Key::Add, withWizard()) == Command::None, "Ctrl+«+» — ничего");
    check(plain(Key::Space, withWizard(FocusOrigin::TextBox)) == Command::None,
          "пробел в имени обложки — текст");
}

/// Ctrl+L — полка отовсюду: с заставки, из книги, из ползунка и поля.
void testLibraryFromEverywhere() {
    std::printf("\n=== клавиши: Ctrl+L отовсюду ===\n");

    check(withControl(Key::L, onScreen(Screen::Start)) == Command::ShowLibrary, "с заставки");
    for (const FocusOrigin origin : kOrigins)
        check(withControl(Key::L, focusedOn(origin)) == Command::ShowLibrary, about("из книги", origin));

    KeyContext busy = reading();
    busy.panelOpen = true;
    busy.noteOpen = true;
    busy.fullScreen = true;
    check(withControl(Key::L, busy) == Command::ShowLibrary, "с открытой панелью и в полном экране");

    check(withControl(Key::L, onScreen(Screen::Library)) == Command::None, "на самой полке — ничего");
    check(plain(Key::L, reading()) == Command::None, "L без Ctrl — ничего");
}

/// F2 — ящики над книгой; F11 — полный экран везде.
void testFunctionKeys() {
    std::printf("\n=== клавиши: F2 и F11 ===\n");

    check(plain(Key::F2, reading()) == Command::TogglePanel, "F2 над книгой — оба ящика");
    check(plain(Key::F2, onScreen(Screen::Start)) == Command::None &&
              plain(Key::F2, onScreen(Screen::Library)) == Command::None,
          "F2 над заставкой и полкой — ничего");

    check(plain(Key::F11, onScreen(Screen::Start)) == Command::ToggleFullScreen &&
              plain(Key::F11, onScreen(Screen::Library)) == Command::ToggleFullScreen &&
              plain(Key::F11, reading()) == Command::ToggleFullScreen,
          "F11 — полный экран на любом экране");
}

/// Escape — один приоритет на всё приложение: панель → сноска → полный экран →
/// туда, откуда пришли.
void testEscapePriority() {
    std::printf("\n=== клавиши: приоритет Escape ===\n");

    KeyContext context = reading();
    context.panelOpen = true;
    context.noteOpen = true;
    context.fullScreen = true;
    check(plain(Key::Escape, context) == Command::ClosePanel, "панель — первой");

    context.panelOpen = false;
    check(plain(Key::Escape, context) == Command::DismissNote, "сноска — второй");

    context.noteOpen = false;
    check(plain(Key::Escape, context) == Command::LeaveFullScreen, "полный экран — третьим");

    context.fullScreen = false;
    check(plain(Key::Escape, context) == Command::BackToStart, "книга, открытая с заставки, — на заставку");

    context.cameFrom = Screen::Library;
    check(plain(Key::Escape, context) == Command::BackToLibrary, "книга, открытая с полки, — на полку");

    KeyContext shelf = onScreen(Screen::Library);
    check(plain(Key::Escape, shelf) == Command::BackToStart, "с полки — на заставку");

    shelf.noteOpen = true;
    check(plain(Key::Escape, shelf) == Command::BackToStart, "сноска не над книгой не в счёт");

    shelf.fullScreen = true;
    check(plain(Key::Escape, shelf) == Command::LeaveFullScreen, "на полке полный экран — раньше ухода");

    KeyContext start = onScreen(Screen::Start);
    check(plain(Key::Escape, start) == Command::None, "на заставке — ничего");

    start.fullScreen = true;
    check(plain(Key::Escape, start) == Command::LeaveFullScreen, "на заставке — выход из полного экрана");
}

}  // namespace

void runKeyMapTests() {
    testPagesTurnFromAnyFocus();
    testSliderAndTextBoxKeepTheirKeys();
    testStripKeys();
    testWizardKeepsOnlyTurning();
    testLibraryFromEverywhere();
    testFunctionKeys();
    testEscapePriority();
}
