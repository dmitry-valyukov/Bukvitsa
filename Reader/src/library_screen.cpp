#include "library_screen.h"

#include "Bind.h"
#include "look.h"

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

// Обложка раскодируется под высоту карточки, а не в полный размер снимка: в
// книге он бывает в тысячи пикселей, а на полке — сотня. Высота — в
// физических пикселях, вдвое против карточки: с запасом на масштаб экрана до
// 200%. Ширину картинка берёт по своей пропорции, а лишнее срезает
// UniformToFill.
constexpr int32_t kCoverDecodeHeight = 2 * static_cast<int32_t>(kCoverHeight);

/// Карточка на полке — контейнер строки списка: бумага карточки, кромка,
/// скругление и отбивка от соседок; содержимое — во всю ширину. Наведение и
/// нажатие подсвечивает сам контейнер.
constexpr auto shelfCardLook = Preset {
    dsl::background = kCard,
    dsl::borderBrush = kEdge,
    BorderThickness{1},
    CornerRadius{6},
    Margin{0, 6},
    Padding{0},
    dsl::horizontalContentAlignment = HorizontalAlignment::Stretch,
};

/// Обложка книги как источник картинки, раскодированной под карточку. Путь
/// абсолютный, поэтому со схемой: без схемы wxl разрешает его рядом с
/// исполняемым файлом.
ImageSource coverOf(const std::filesystem::path& coverDirectory, const BookEntry& book) {
    if (book.cover.empty()) return {};

    // Имя нарочно не text: одноимённый тег синтаксиса перекрылся бы им. Теги
    // ниже — с квалификатором по той же причине.
    std::u16string full = (coverDirectory / book.cover.wchars()).u16string();
    std::replace(full.begin(), full.end(), u'\\', u'/');
    return BitmapImage {
        dsl::uriSource = Uri{u"file:///" + full},
        dsl::decodePixelHeight = kCoverDecodeHeight,
    };
}

/// Надпись «Пока пусто» видна, пока на полке нет ни одной карточки.
Visibility shownWhenEmpty(uint32_t count) {
    return count == 0 ? Visibility::Visible : Visibility::Collapsed;
}

}  // namespace

// Полка — список карточек реестра. Карточка строится, когда книга попадает на
// экран, и тогда же получает обложку; строка прогресса привязана к полю
// карточки и уходит вместе с ней, когда список отдаёт строку обратно.
LibraryScreen::LibraryScreen(observable_list<intrusive_ptr<ShelfCard> const>& cards,
                             observable<bool>& continueReading, std::filesystem::path covers,
                             Actions& actions)
    : actions_(actions), cards_(cards) {
    // Теги разметки — внутри строителей, не на уровне файла: там они накрыли
    // бы обычные слова (entry, text) и под /W4 каждое стало бы C4459.
    using namespace wxl::dsl;

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

        // Карточку по книге щёлкают: щелчок, наведение, фокус и клавиатуру
        // даёт сам список, выбора у полки нет. Отбивка от краёв окна — внутри
        // прокрутки, полоса прокрутки — у края.
        ListView {
            row = 1,
            Padding{40, 8, 40, 32},
            selectionMode = ListViewSelectionMode::None,
            isItemClickEnabled = true,
            itemContainerStyle = shelfCardLook,
            itemsSource = BindOutput {cards, [coverDirectory = std::move(covers)](intrusive_ptr<ShelfCard> const& card) {
                const BookEntry& book = card->entry;
                return Grid {
                    // Карточка — обложка и надписи в сетке, и чтецу экрана
                    // такая строка безымянна: имя берётся из содержимого,
                    // только когда оно строка. Подсказка при наведении
                    // повторяла бы то, что и так на карточке, — поэтому имя
                    // отдельно, не toolTip.
                    automationName = book.title,
                    columnDefinitions = u"auto,*",
                    columnSpacing = 16,
                    Margin{12},

                    Image {
                        column = 0,
                        source = coverOf(coverDirectory, book),
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
                        // Строка прогресса — поле карточки: пусто, пока файл
                        // состояния не прочитан, и проступает сама, когда его
                        // прочитают, — карточка не перестраивается.
                        TextBlock {
                            fontSize = 13,
                            foreground = kDimInk,
                            Margin{0, 8, 0, 0},
                            text = BindOutput{card->progress},
                        },
                    },
                };
            }},
            onItemClick = method(this, &LibraryScreen::cardClicked),
        },

        TextBlock {
            u"Пока пусто. Добавьте книгу — она останется там, где лежит.",
            row = 1,
            fontSize = 16,
            foreground = kDimInk,
            Margin{40, 24, 40, 0},
            visibility = BindOutput{cards.count(), shownWhenEmpty},
        },

        onLoaded = method(this, &LibraryScreen::loaded),
    };
}

void LibraryScreen::loaded(Grid const& self) {
    self.focus(FocusState::Programmatic);
}

void LibraryScreen::cardClicked(ListView const&, ItemClickEventArgs& args) {
    // Своя книга у каждой карточки: щелчок отдаёт строку списка, строка —
    // карточку реестра, а намерение берёт guid её книги копией.
    if (intrusive_ptr<ShelfCard> const* card = boundItem(cards_, args.clickedItem())) {
        actions_.openBook((*card)->entry.guid);
    }
}

}  // namespace bukvitsa::reader
