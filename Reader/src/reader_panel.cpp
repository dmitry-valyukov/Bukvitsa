#include <algorithm>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "reader_panel.h"

#include "book_index.h"

#include "Bind.h"

import wxl.fmt;

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

// Ящик, а не бумага: свои цвета при любой теме страницы. wxl:: — потому что
// rgb() темы страницы (theme.h) даёт цвет Direct2D и здесь заслонил бы этот.
constexpr Color kChrome = wxl::rgba(30, 30, 34, 242 / 255.0);   ///< слегка прозрачный — под ним текст
constexpr Color kInk = wxl::rgb(232, 228, 220);
constexpr Color kDim = wxl::rgb(154, 150, 142);
constexpr Color kEdge = wxl::rgba(255, 255, 255, 0.2);
constexpr Color kActive = wxl::rgba(255, 255, 255, 34 / 255.0);

constexpr double kWidth = 380;

/// Подпись над группой настроек: тише текста и с отбивкой сверху. Не лямбда
/// внутри одного строителя, потому что групп теперь две и собирают их разные
/// функции — вкладка «Вид» и пересборка списка тем.
TextBlock groupCaption(zstring_view said) {
    using namespace wxl::dsl;

    return TextBlock{
        said,
        fontSize = 13,
        foreground = SolidColorBrush{kDim},
        Margin{0, 12, 0, 2},
    };
}

// Выезд: короткий, потому что панель открывают между двумя строчками текста и
// ждать её не должны. Двести миллисекунд — предел, за которым появление
// читается как задержка.
constexpr auto kSlide = 180ms;

/// Пусто ли введённое — пробелы не в счёт.
bool blank(std::u16string_view text) {
    return std::all_of(text.begin(), text.end(), [](char16_t c) { return c == u' ' || c == u'\t'; });
}

}  // namespace

// Отметка темы идёт за полем полосы, а не за нажатием здешней кнопки: тему
// меняют и клавишей T мимо панели. Слушатель ставится в списке инициализации:
// cookie_t не присваивается, а срабатывает он только на смену поля — не раньше,
// чем дерево панели собрано.
ReaderPanel::ReaderPanel(const Compositor& compositor, BookView& view, Settings& settings)
    : compositor_(compositor),
      view_(view),
      prefs_(settings),
      themeWatch_(view.theme.on_change([this](int) noexcept { markTheme(); })) {
    buildTree();
}

ReaderPanel::~ReaderPanel() {
    view_.theme.remove_change(themeWatch_);
}

void ReaderPanel::buildTree() {
    using namespace wxl::dsl;

    tabPages_ = {buildContents(), buildSearch(), buildBookmarks()};

    // Выход на полку — первым, отдельной строкой над вкладками. Вкладки
    // говорят о книге, которая открыта; эта кнопка — о том, чтобы открыть
    // другую, и стоять в одном ряду с ними ей не за что.
    auto shelf = Button{
        row = 0,
        u"←  Моя библиотека",
        hAlign.stretch,
        horizontalContentAlignment = HorizontalAlignment::Left,
        fontSize = 14,
        Margin{12, 12, 12, 0},
        Padding{12, 8},
        foreground = SolidColorBrush{kInk},
        background = SolidColorBrush{colors.transparent},
        borderBrush = SolidColorBrush{kEdge},
        BorderThickness{1},
        CornerRadius{4},
        onClick =
            [this](Object const&, RoutedEventArgs&) {
                if (onLibrary) onLibrary();
            },
    };

    auto strip = StackPanel{
        row = 1,
        Orientation::Horizontal,
        Margin{12, 12, 12, 6},
    };
    for (auto&& [said, tab] : {std::pair{u"Оглавление", Tab::Contents},
                                  std::pair{u"Поиск", Tab::Search},
                                  std::pair{u"Закладки", Tab::Bookmarks}}) {
        auto button = tabButton(said, tab);
        tabButtons_.push_back(button);
        strip.children().append(button);
    }

    pages_ = Grid{row = 2, Margin{12, 6, 12, 12}};
    for (const UIElement& page : tabPages_) {
        pages_.value().children().append(page);
    }

    navigation_ = box(HorizontalAlignment::Left, Grid{
                                                     rowDefinitions = u"auto,auto,*",
                                                     shelf,
                                                     strip,
                                                     pages_.value(),
                                                 });
    settings_ = box(HorizontalAlignment::Right, buildSettings());

    // Оба ящика — в одном холсте с прозрачной, но настоящей кистью: без неё
    // холст не участвует в проверке попадания, а с ней ловит щелчок мимо
    // ящиков и закрывает оба, съедая щелчок, — как свет-дисмисс у Flyout.
    // Сам Flyout не подошёл: попапов со свет-дисмиссом у WinUI в один момент
    // один, второй закрывает первый. Пока панель закрыта, холст свёрнут, и
    // страница между ящиками остаётся страницей.
    root_ = Grid{
        visibility = Visibility::Collapsed,
        background = SolidColorBrush{colors.transparent},
        navigation_.value(),
        settings_.value(),
    };
    root_.value().add_onPointerPressed([this](Object const&, PointerRoutedEventArgs& args) {
        // Сюда доходят только щелчки по самому холсту: щелчки по ящикам они
        // же и гасят.
        close();
        args.handled(true);
    });

    linear_ = compositor_.createLinearEasingFunction();
    navigationVisual_ = slidingVisual(navigation_.value(), -static_cast<float>(kWidth));
    settingsVisual_ = slidingVisual(settings_.value(), static_cast<float>(kWidth));

    showTab(Tab::Contents);
}

Border ReaderPanel::box(HorizontalAlignment side, const UIElement& inside) {
    using namespace wxl::dsl;

    // Кромка у ящика одна — та, что смотрит на страницу. Отбивку от стенок
    // правому ящику даёт сам ящик; у левого её несут его ряды.
    // Не left: это имя тега DSL, и локальная переменная его прятала бы.
    const bool onLeft = side == HorizontalAlignment::Left;
    const double pad = onLeft ? 0.0 : 12.0;

    auto border = Border{
        horizontalAlignment = side,
        vAlign.stretch,
        width = kWidth,
        background = SolidColorBrush{kChrome},
        borderBrush = SolidColorBrush{kEdge},
        BorderThickness{onLeft ? 0.0 : 1.0, 0.0, onLeft ? 1.0 : 0.0, 0.0},
        Padding{pad, pad},
        child = inside,
    };

    // Ящик — не страница: щелчок и колесо по его пустому месту здесь и
    // кончаются, иначе они всплыли бы к полосе, и та листала бы книгу под
    // открытой панелью и закрывала её правой кнопкой.
    border.add_onPointerPressed([](Object const&, PointerRoutedEventArgs& args) {
        args.handled(true);
    });
    border.add_onPointerWheelChanged([](Object const&, PointerRoutedEventArgs& args) {
        args.handled(true);
    });
    return border;
}

Visual ReaderPanel::slidingVisual(const UIElement& box, float offscreen) {
    // Выезд идёт по Translation, а не по Offset: Offset — это то, чем XAML
    // расставляет элементы при разметке, и анимация его отобрала бы.
    ElementCompositionPreview::setIsTranslationEnabled(box, true);
    Visual visual = ElementCompositionPreview::getElementVisual(box);
    visual.properties().insertVector3(L"Translation", Vector3{offscreen, 0.0f, 0.0f});
    return visual;
}

void ReaderPanel::slide(Visual& visual, float x) {
    auto animation = compositor_.createVector3KeyFrameAnimation();
    animation.duration(kSlide);
    animation.insertKeyFrame(1.0f, Vector3{x, 0.0f, 0.0f}, linear_.value());
    visual.startAnimation(L"Translation", animation);
}

Button ReaderPanel::tabButton(zstring_view said, Tab tab) {
    using namespace wxl::dsl;

    return Button{
        said,
        fontSize = 14,
        Margin{0, 0, 6, 0},
        Padding{12, 6},
        foreground = SolidColorBrush{kInk},
        background = SolidColorBrush{colors.transparent},
        borderBrush = SolidColorBrush{kEdge},
        BorderThickness{1},
        CornerRadius{4},
        onClick = [this, tab](Object const&, RoutedEventArgs&) { open(tab); },
    };
}

Button ReaderPanel::listItem(zstring_view said, zstring_view under, float indent,
                             std::function<void()> action) {
    using namespace wxl::dsl;

    auto lines = StackPanel{
        TextBlock{
            said,
            fontSize = 14,
            foreground = SolidColorBrush{kInk},
            textWrapping.wrap,
            maxLines = 2,
            textTrimming.characterEllipsis,
        },
    };

    if (!under.empty()) {
        lines.children().append(TextBlock{
            under,
            fontSize = 12,
            foreground = SolidColorBrush{kDim},
            Margin{0, 2, 0, 0},
            maxLines = 1,
            textTrimming.characterEllipsis,
        });
    }

    return Button{
        // Лицо строки — панель из надписей, а не слово, и чтецу экрана кнопка с
        // такой начинкой безымянна: имя кнопки берётся из содержимого, только
        // когда оно строка. Подсказки при наведении строке не нужно — её слова
        // и так на экране, — поэтому имя отдельно, не toolTip.
        automationName = said,
        hAlign.stretch,
        horizontalContentAlignment = HorizontalAlignment::Stretch,
        Margin{indent, 1, 0, 1},
        Padding{8, 6},
        background = SolidColorBrush{colors.transparent},
        borderBrush = SolidColorBrush{colors.transparent},
        BorderThickness{0},
        CornerRadius{4},
        onClick = [action](Object const&, RoutedEventArgs&) { if (action) action(); },
        content = lines,
    };
}

/* ---------------- вкладки ---------------- */

UIElement ReaderPanel::buildContents() {
    using namespace wxl::dsl;

    contentsList_ = StackPanel{};
    return ScrollViewer{
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        content = contentsList_.value(),
    };
}

UIElement ReaderPanel::buildSearch() {
    using namespace wxl::dsl;

    searchBox_ = TextBox{
        row = 0,
        // Подсказка в пустом поле — не имя: чтецу экрана поле зовётся отдельно.
        automationName = u"Поиск по книге",
        placeholderText = u"Что искать",
        Margin{0, 0, 0, 8},
    };

    // Поиск по Enter, а не по каждой букве: искать по одной букве в романе —
    // это тысячи находок, из которых читателю не нужна ни одна.
    searchBox_.value().add_onKeyDown([this](Object const&, KeyRoutedEventArgs& args) {
        if (args.key() != VirtualKey::Enter) return;
        runSearch();
        args.handled(true);
    });

    searchNote_ = TextBlock{
        row = 1,
        u"Введите слово и нажмите Enter.",
        fontSize = 13,
        foreground = SolidColorBrush{kDim},
        textWrapping.wrap,
    };

    searchList_ = StackPanel{};

    return Grid{
        rowDefinitions = u"auto,auto,*",
        searchBox_.value(),
        searchNote_.value(),
        ScrollViewer{
            row = 2,
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            content = searchList_.value(),
        },
    };
}

UIElement ReaderPanel::buildBookmarks() {
    using namespace wxl::dsl;

    bookmarkNote_ = TextBlock{
        row = 1,
        fontSize = 13,
        foreground = SolidColorBrush{kDim},
        textWrapping.wrap,
    };

    bookmarkList_ = StackPanel{};

    return Grid{
        rowDefinitions = u"auto,auto,*",
        Button{
            row = 0,
            u"Заложить эту страницу",
            hAlign.stretch,
            Margin{0, 0, 0, 8},
            foreground = SolidColorBrush{kInk},
            background = SolidColorBrush{kActive},
            borderBrush = SolidColorBrush{kEdge},
            BorderThickness{1},
            CornerRadius{4},
            onClick = [this](Object const&, RoutedEventArgs&) { toggleBookmark(); },
        },
        bookmarkNote_.value(),
        ScrollViewer{
            row = 2,
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            content = bookmarkList_.value(),
        },
    };
}

UIElement ReaderPanel::buildSettings() {
    using namespace wxl::dsl;

    themesPanel_ = StackPanel{};
    refreshThemes();

    // Ползунок, а не пара кнопок: кегль подбирают, а не выставляют числом, и
    // видеть весь ход сразу удобнее, чем нажимать «плюс» восемь раз.
    //
    // Привязан к полю настроек в обе стороны: показывает поле и пишет в него.
    // В то же поле пишут колесо и клавиши полосы — и ползунок идёт за ними
    // сам, без обработчика и без флага «это мы сами его двигаем». Единицы у
    // ползунка и поля одни (проценты у интерлиньяжа и полей, см. Settings).
    //
    // Подпись над ползунком — его же имя для чтеца экрана: у ползунка нет
    // слова на лице, и без имени Narrator называет его просто ползунком.
    // Одни слова на подпись и имя, поэтому группа собирается здесь целиком.
    auto setting = [](zstring_view said, double low, double high, double step,
                      observable<double>& field) {
        return StackPanel{
            groupCaption(said),
            Slider{
                automationName = said,
                minimum = low,
                maximum = high,
                stepFrequency = step,
                value = Bind{field},
                Margin{0, 0, 0, 4},
            },
        };
    };

    return ScrollViewer{
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        content = StackPanel{
            // Подписи «Тема» и «Обложки» ставит сама полоса набора: она знает,
            // где кончается одна группа и начинается другая, а собирается
            // заново при каждой смене реестра.
            themesPanel_.value(),
            setting(u"Кегль", kFontSizeMin, kFontSizeMax, kFontSizeStep, prefs_.fontSize),
            setting(u"Интерлиньяж", kLineHeightMin, kLineHeightMax, kLineHeightStep,
                    prefs_.lineHeight),
            setting(u"Поля", kMarginMin, kMarginMax, kMarginStep, prefs_.margin),
            TextBlock{
                u"Кегль меняется ещё и Ctrl с колесом, а тема — клавишей T.",
                fontSize = 12,
                foreground = SolidColorBrush{kDim},
                Margin{0, 16, 0, 0},
                textWrapping.wrap,
            },
        },
    };
}

/* ---------------- показ ---------------- */

void ReaderPanel::showTab(Tab tab) {
    tab_ = tab;
    for (size_t i = 0; i < tabPages_.size(); ++i) {
        tabPages_[i].visibility(static_cast<size_t>(tab) == i ? Visibility::Visible
                                                                  : Visibility::Collapsed);
    }
    for (size_t i = 0; i < tabButtons_.size(); ++i) {
        tabButtons_[i].background(
            SolidColorBrush{static_cast<size_t>(tab) == i ? kActive : colors.transparent});
    }
}

void ReaderPanel::open(Tab tab) {
    showTab(tab);

    switch (tab) {
        case Tab::Contents: fillContents(); break;
        case Tab::Bookmarks: fillBookmarks(); break;
        case Tab::Search: break;   // список остаётся от прошлого поиска
    }

    show();

    if (tab == Tab::Search) searchBox_.value().focus(FocusState::Programmatic);
}

void ReaderPanel::toggle() {
    if (open_) {
        close();
    } else {
        open(tab_);
    }
}

void ReaderPanel::show() {
    if (open_) return;
    open_ = true;

    root_.value().visibility(Visibility::Visible);
    slide(navigationVisual_.value(), 0.0f);
    slide(settingsVisual_.value(), 0.0f);
}

void ReaderPanel::close() {
    if (!open_) return;
    open_ = false;

    // Уехавшие ящики надо ещё и спрятать вместе с холстом, иначе они
    // продолжат ловить щелчки: ящики за краями экрана, холст — на странице.
    // Конец анимации узнаётся пакетом, а не таймером: пакет сам скажет, когда
    // последняя из двух анимаций в нём закончилась.
    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);

    slide(navigationVisual_.value(), -static_cast<float>(kWidth));
    slide(settingsVisual_.value(), static_cast<float>(kWidth));

    batch.add_onCompleted([this](Object const&, CompositionBatchCompletedEventArgs&) {
        if (!open_) root_.value().visibility(Visibility::Collapsed);
    });
    batch.end();
}

void ReaderPanel::setState(BookState* state) {
    state_ = state;
    if (tab_ == Tab::Bookmarks) fillBookmarks();
}

/* ---------------- содержимое вкладок ---------------- */

void ReaderPanel::fillContents() {
    contentsList_.value().children().clear();

    const sta_vector<ContentsEntry> contents = contentsOf(view_.blocks());
    if (contents.empty()) {
        contentsList_.value().children().append(listItem(u"В этой книге нет заголовков", {}, 0,
                                                         {}));
        return;
    }

    for (const ContentsEntry& entry : contents) {
        const uint32_t offset = entry.charOffset;
        contentsList_.value().children().append(
            // Заголовок оглавления — вид в текст блока, без нуля за ним: в разметку
            // он идёт строкой WinRT, которую мы и делаем сами.
            listItem(hstring{entry.title}, {}, std::min<float>(entry.level, 4) * 14.0f,
                     [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::fillBookmarks() {
    bookmarkList_.value().children().clear();

    if (!state_ || state_->bookmarks.empty()) {
        bookmarkNote_.value().text(u"Закладок пока нет.");
        return;
    }

    bookmarkNote_.value().text({});

    for (const Bookmark& mark : state_->bookmarks) {
        const uint32_t offset = mark.charOffset;
        bookmarkList_.value().children().append(
            listItem(mark.hint.empty() ? zstring_view{u"Закладка"} : zstring_view{mark.hint}, {}, 0,
                     [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::runSearch() {
    searchList_.value().children().clear();

    // Текст поля — чужой: пришёл из контрола строкой WinRT и в поиск идёт
    // проверенным. Строка держится, пока жив вид на неё.
    const hstring typed = searchBox_.value().text();
    const std::optional<u16_view> needle = unicode::checked(std::u16string_view(typed));
    if (!needle || needle->empty() || blank(needle->plain())) {
        searchNote_.value().text(u"Введите слово и нажмите Enter.");
        return;
    }

    const sta_vector<SearchHit> hits = searchBook(view_.blocks(), *needle);
    if (hits.empty()) {
        searchNote_.value().text(core::format(u"«{}» в книге не нашлось.", *needle));
        return;
    }

    searchNote_.value().text(core::format(u"Нашлось: {}", hits.size()));

    for (const SearchHit& hit : hits) {
        const uint32_t offset = hit.charOffset;
        searchList_.value().children().append(
            listItem(hit.context, {}, 0, [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::toggleBookmark() {
    if (!state_ || !view_.isOpen()) return;

    const uint32_t here = view_.readingPosition();

    const auto found = std::find_if(state_->bookmarks.begin(), state_->bookmarks.end(),
                                    [here](const Bookmark& mark) {
                                        return mark.charOffset == here;
                                    });
    if (found != state_->bookmarks.end()) {
        state_->bookmarks.erase(found);
    } else {
        // Закладки лежат по порядку книги: так их и читают, и так список не
        // приходится сортировать при показе.
        Bookmark mark{here, hintAt(view_.blocks(), here)};
        const auto after = std::lower_bound(state_->bookmarks.begin(), state_->bookmarks.end(),
                                            here, [](const Bookmark& mark, uint32_t offset) {
                                                return mark.charOffset < offset;
                                            });
        state_->bookmarks.insert(after, std::move(mark));
    }

    fillBookmarks();
    if (onStateChanged) onStateChanged();
}

void ReaderPanel::refreshThemes() {
    using namespace wxl::dsl;

    themesPanel_.value().children().clear();
    themeButtons_.clear();

    // Индексы тем сквозные: сперва встроенные, затем обложки — ровно так их
    // считает и полоса набора. markTheme() ходит по кнопкам тем же счётом.
    auto themeButton = [this](zstring_view said, int index, Thickness gap) {
        return Button{
            said,
            fontSize = 13,
            Margin{gap},
            Padding{12, 6},
            foreground = SolidColorBrush{kInk},
            background = SolidColorBrush{colors.transparent},
            borderBrush = SolidColorBrush{kEdge},
            BorderThickness{1},
            CornerRadius{4},
            onClick =
                [this, index](Object const&, RoutedEventArgs&) { view_.setTheme(index); },
        };
    };

    // Тема — это ровный цвет бумаги, и таких три. Они коротки и помещаются в
    // строчку; фотография среди них не стоит больше — снимок носит обложка.
    themesPanel_.value().children().append(groupCaption(u"Тема"));

    auto builtins = StackPanel{Orientation::Horizontal};
    for (int index = 0; index < kThemeCount; ++index) {
        auto button = themeButton(kThemes[index].name, index, Thickness{0, 0, 6, 0});
        themeButtons_.push_back(button);
        builtins.children().append(button);
    }
    themesPanel_.value().children().append(builtins);

    // Кнопка-глиф рядом с обложкой. Их две, и обе одинаковы во всём, кроме
    // глифа, подсказки и того, что делают, — поэтому одно описание на двоих,
    // а не два одинаковых подряд.
    //
    // Кнопка без текста обязана иметь тултип (правило дизайна) — и он
    // называет конкретную обложку, а не действие вообще.
    auto iconButton = [](zstring_view glyph, const u16_text& tip, auto action) {
        return Button{
            glyph,
            fontFamily = FontFamily{u"Segoe Fluent Icons"},
            toolTip = tip,
            fontSize = 13,
            Margin{6, 6, 0, 0},
            Padding{8, 6},
            foreground = SolidColorBrush{kDim},
            background = SolidColorBrush{colors.transparent},
            borderBrush = SolidColorBrush{kEdge},
            BorderThickness{1},
            CornerRadius{4},
            onClick = [action](Object const&, RoutedEventArgs&) { action(); },
        };
    };

    // Обложки — по строке на каждую: имя даёт читатель, и в строчку они не
    // помещаются. Сперва системные, приехавшие с программой, потом заведённые
    // читателем; порядок задаёт не панель, а сам список (BookView::setSkins).
    //
    // Рядом с каждой шестерёнка: обложку не только выбирают, но и правят, и
    // дорога к правке стоит у самой обложки. Корзина — только у своей:
    // системной в реестре нет, удалять нечего, и кнопка вела бы к тому, чего
    // не бывает.
    //
    // Спросить «точно ли» панель не может и не должна: окна у неё нет, а
    // удаление необратимо — вопрос задаёт приложение, которому принадлежат и
    // окно, и реестр.
    themesPanel_.value().children().append(groupCaption(u"Обложки"));

    const std::vector<Skin>& skins = view_.skins();
    for (size_t index = 0; index < skins.size(); ++index) {
        auto button = themeButton(skins[index].name, kThemeCount + static_cast<int>(index),
                                  Thickness{0, 6, 0, 0});
        themeButtons_.push_back(button);

        const u16_text skinName = skins[index].name;

        auto line = StackPanel{
            Orientation::Horizontal,
            button,
            iconButton(u"",   // шестерёнка Segoe Fluent Icons
                       core::format(u"Настроить подложку «{}»", skinName),
                       [this, skinName] {
                           if (onEditSkin) onEditSkin(skinName);
                       }),
        };

        if (!skins[index].system) {
            line.children().append(iconButton(u"",   // корзина оттуда же
                                             core::format(u"Удалить обложку «{}»", skinName),
                                             [this, skinName] {
                                                 if (onDeleteSkin) onDeleteSkin(skinName);
                                             }));
        }

        themesPanel_.value().children().append(line);
    }

    // Дорога в мастер — последней строкой, после всех тем.
    themesPanel_.value().children().append(Button{
        u"Добавить обложку…",
        fontSize = 13,
        Margin{0, 6, 0, 0},
        Padding{12, 6},
        foreground = SolidColorBrush{kDim},
        background = SolidColorBrush{colors.transparent},
        borderBrush = SolidColorBrush{kEdge},
        BorderThickness{1},
        CornerRadius{4},
        onClick =
            [this](Object const&, RoutedEventArgs&) {
                if (onAddSkin) onAddSkin();
            },
    });

    markTheme();
}

void ReaderPanel::markTheme() {
    // Тема — выбор из трёх, а выбор видно только тогда, когда выбранное
    // отмечено. Отмечается тем же цветом, что и открытая вкладка: одна
    // и та же мысль — «вот это сейчас».
    for (int index = 0; index < static_cast<int>(themeButtons_.size()); ++index) {
        themeButtons_[static_cast<size_t>(index)].background(
            SolidColorBrush{index == view_.theme.get() ? kActive : colors.transparent});
    }
}

}  // namespace bukvitsa::reader
