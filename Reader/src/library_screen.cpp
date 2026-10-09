#include "library_screen.h"

#include "Bind.h"
#include "look.h"

// Последним из своих: несёт импорт wxl.core.
#include "bukvitsa/reader/shelf_text.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

// Полка бумажного цвета, как и полоса набора: витрина — часть той же книги,
// а не отдельное приложение. Приглушённый тон (kDimInk) — общий с заставкой,
// он в look.h.
constexpr Color kPaper = rgb(247, 244, 238);
constexpr Color kInk = rgb(32, 30, 28);
constexpr Color kCard = rgb(255, 253, 249);
constexpr Color kEdge = rgb(227, 222, 212);

// Обложка стоит в пропорции 2:3 — так их печатают, и так они не прыгают по
// высоте, когда у одной книги обложка квадратная, а у другой узкая.
constexpr double kCoverWidth = 72;
constexpr double kCoverHeight = 108;

/// Путь к обложке как источник картинки. Путь абсолютный, поэтому со схемой:
/// без схемы wxl разрешает его рядом с исполняемым файлом.
ImageSource coverOf(const std::filesystem::path& coverDirectory, const BookEntry& entry) {
    if (entry.cover.empty()) return {};

    // Имя нарочно не text: одноимённый тег синтаксиса перекрылся бы им.
    std::u16string full = (coverDirectory / entry.cover.wchars()).u16string();
    std::replace(full.begin(), full.end(), u'\\', u'/');
    return ImageSource{u"file:///" + full};
}

/// Надпись «Пока пусто» видна, пока на полке нет ни одной карточки.
Visibility shownWhenEmpty(bool empty) {
    return empty ? Visibility::Visible : Visibility::Collapsed;
}

}  // namespace

LibraryScreen::LibraryScreen(const Library& library, observable<bool>& continueReading,
                             std::filesystem::path covers, Actions& actions)
    : actions_(actions), library_(library), covers_(std::move(covers)) {
    // Теги разметки — внутри строителей, не на уровне файла: там они накрыли
    // бы обычные слова (entry, text) и под /W4 каждое стало бы C4459.
    using namespace wxl::dsl;

    Apply{shelf_, Margin{40, 8, 40, 32}};

    Apply {
        root_,
        isTabStop = true,

        // Тема — светлая, явно: полка бумажная, а остров без своей темы берёт
        // тему приложений Windows, и в тёмной текст кнопок светлел бы на бумаге.
        requestedTheme = ElementTheme::Light,

        background = kPaper,
        rowDefinitions = u"auto,*",

        Grid {
            row = 0,
            Margin{40, 32, 40, 8},
            columnDefinitions = u"*,auto,auto,auto",
            columnSpacing = 12,

            TextBlock {
                u"Моя библиотека",
                column = 0,
                fontSize = 26,
                FontWeight{600},
                foreground = kInk,
                vAlign.center,
            },
            // Галочка привязана к полю настроек в обе стороны: показывает его и
            // пишет в него; запись файла слушает само поле. Место в сетке задано
            // при постройке: тег колонки живёт на самом элементе, а не на сетке.
            CheckBox {
                u"Продолжать чтение при старте",
                column = 1,
                foreground = kInk,
                vAlign.center,
                isChecked = Bind{continueReading},
            },
            Button {
                u"Добавить книгу",
                column = 2,
                onClick = method(&actions_, &Actions::chooseBook),
            },
            Button {
                u"Назад",
                column = 3,
                onClick = method(&actions_, &Actions::back),
            },
        },

        ScrollViewer {
            row = 1,
            content = StackPanel {
                TextBlock {
                    u"Пока пусто. Добавьте книгу — она останется там, где лежит.",
                    fontSize = 16,
                    foreground = kDimInk,
                    Margin{40, 24, 40, 0},
                    visibility = BindOutput{empty_, shownWhenEmpty},
                },
                shelf_,
            },
        },

        onLoaded = method(this, &LibraryScreen::loaded),
    };
}

void LibraryScreen::loaded(Grid const& self) {
    self.focus(FocusState::Programmatic);
}

void LibraryScreen::appendBook(const BookEntry& entry) {
    shelf_.children().append(shelfItem(entry));
    empty_.set(false);
}

void LibraryScreen::setProgress(u16_view guid, uint32_t charOffset, size_t bookmarks) {
    const auto found = progress_.find(std::u16string(guid.plain()));

    if (found == progress_.end()) return;   // полку успели пересобрать

    const BookEntry* entry = library_.find(guid);

    if (!entry) return;

    // Место чтения лежит в отдельном файле на книгу, и читает его фоновая
    // корутина -- уже после того, как карточка встала на полку. Поэтому у
    // строки два состояния: «ещё не знаем» (пусто, см. shelfItem) и то, что
    // принесли.
    found->second.text(shelfLine(charOffset, entry->characterCount, bookmarks));
}

void LibraryScreen::show() {
    progress_.clear();

    // Полка пересобирается целиком. Сравнивать её с реестром и править
    // разницу было бы дороже во всех смыслах: книг десятки, а не тысячи, и
    // добавление одной — не повод заводить вторую модель того же списка.
    shelf_.children().clear();
    for (const BookEntry& entry : library_.books()) {
        shelf_.children().append(shelfItem(entry));
    }

    empty_.set(library_.books().empty());
}

Button LibraryScreen::shelfItem(const BookEntry& book) {
    using namespace wxl::dsl;

    // Карточка — это кнопка: по книге щёлкают, и всё, что кнопка умеет сама
    // (наведение, нажатие, фокус, клавиатура), достаётся даром.
    u16_text const guid = book.guid;

    // Строка прогресса ставится пустой не просто так: «не открывалась» было бы
    // неправдой, пока файл состояния ещё не прочитан, а карточка обязана
    // появиться раньше, чем он будет прочитан. Настоящий текст приносит
    // setProgress().
    TextBlock progress = TextBlock {
        book.characterCount == 0 ? progressLine(0, 0) : u16_text{},
        fontSize = 13,
        foreground = kDimInk,
        Margin{0, 8, 0, 0},
    };

    progress_.insert_or_assign(guid.plain(), progress);

    return Button {
        // Карточка — обложка и надписи в сетке, и чтецу экрана такая кнопка
        // безымянна: имя берётся из содержимого, только когда оно строка.
        // Подсказка при наведении повторяла бы то, что и так на карточке, —
        // поэтому имя отдельно, не toolTip.
        automationName = book.title,
        hAlign.stretch,
        // Содержимое кнопки по умолчанию стоит по центру -- для карточки это
        // значит текст посреди пустоты. Растянуть его надо явно, и это
        // horizontalContentAlignment, а не hAlign: тот про саму кнопку.
        horizontalContentAlignment = HorizontalAlignment::Stretch,
        Margin{0, 6},
        Padding{0},
        background = kCard,
        borderBrush = kEdge,
        BorderThickness{1},
        CornerRadius{6},
        // Своя книга у каждой карточки — её guid держит замыкание; намерение
        // берёт его копией.
        onClick = [this, guid] { actions_.openBook(guid); },

        content = Grid {
            columnDefinitions = u"auto,*",
            columnSpacing = 16,
            Margin{12},

            Image {
                column = 0,
                source = coverOf(covers_, book),
                width = kCoverWidth,
                height = kCoverHeight,
                stretch = Stretch::UniformToFill,
                vAlign.top,
            },

            StackPanel {
                column = 1,
                vAlign.center,
                TextBlock {
                    book.title,
                    fontSize = 18,
                    FontWeight{600},
                    foreground = kInk,
                    twoLinesLook,
                },
                TextBlock {
                    book.authors,
                    fontSize = 14,
                    foreground = kDimInk,
                    Margin{0, 4, 0, 0},
                    oneLineLook,
                },
                progress,
            },
        },
    };
}

}  // namespace bukvitsa::reader
