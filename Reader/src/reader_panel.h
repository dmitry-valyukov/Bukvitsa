#pragma once
// Панель читалки: два ящика поверх страницы, выезжающие вместе — слева
// навигация (оглавление, поиск, закладки и выход на полку), справа вид (тема,
// обложки, кегль, интерлиньяж, поля).
//
// Два ящика, а не четыре всплывашки и не один на всё, потому что дело у них
// общее: читатель на секунду отвлёкся от текста, чтобы куда-то перейти или
// что-то подкрутить, и хочет вернуться. Поэтому они появляются и уходят
// вместе, и один Escape закрывает оба, как и щелчок мимо любого из них: это
// всплывашки, а не окна. А навигация и вид разведены по
// сторонам, потому что это разные занятия: слева ходят по книге, справа её
// обставляют, и ни одно не ждёт своей вкладки.
//
// Ящики выезжают поверх страницы, а не отодвигают её: полоса набора при смене
// ширины перевёрстывается целиком, и открывать оглавление было бы дороже, чем
// перелистнуть. Поверх — бесплатно, и место чтения не дёргается.
//
// Панель — часть обстановки, а не бумаги, поэтому она тёмная при любой теме
// страницы: светлый ящик поверх ночной страницы слепит, а тёмный поверх
// дневной читается как ящик, чем он и является.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <vector>

#include "Object.h"
#include "pch.h"

// Последними: они ведут к модели книги и реестру, а те импортируют wxl.core,
// после чего стандартный заголовок MSVC уже не принимает.
#include "book_view.h"
#include "bukvitsa/reader/book_places.h"
#include "bukvitsa/reader/book_search.h"
#include "bukvitsa/reader/theme_list.h"

// Импорт — последним: намерения панели — корутины `detached_task`.
import wxl.async;

namespace bukvitsa::reader {

/// Два ящика поверх полосы. Владеет своим деревом и анимацией выезда; полоса,
/// настройки, места книги, список тем, поиск и исполнитель намерений —
/// владельца, живут дольше панели.
class ReaderPanel : private noncopyable {
public:
    /// Вкладки левого ящика. Вид — не вкладка, а правый ящик целиком.
    enum class Tab { Contents, Search, Bookmarks };

    /// Что панель умеет попросить. Реализует владелец; намерение — корутина:
    /// ждёт ли дело диска или вопроса читателю, панели знать незачем.
    class Actions {
    public:
        /// «← Моя библиотека»: выбрать другую книгу. Смена книги — дело
        /// приложения, не панели.
        virtual wxl::async::detached_task showLibrary() = 0;

        /// «Заложить эту страницу»: поставить закладку на место чтения или
        /// снять ту, что там стоит. Состояние книги — владельца; панель
        /// показывает его, а не меняет.
        virtual wxl::async::detached_task toggleBookmark() = 0;

        /// «Добавить обложку…». Мастер — оверлей приложения, а не панели: он
        /// ложится поверх всей полосы.
        virtual wxl::async::detached_task addSkin() = 0;

        /// Шестерёнка у обложки: открыть в мастере правку её кривизны.
        virtual wxl::async::detached_task editSkin(u16_text name) = 0;

        /// Корзина у обложки: убрать её из реестра. Спросить, точно ли, —
        /// дело приложения: у панели нет ни окна, ни права решать за
        /// читателя, а удаление необратимо. Имя — по значению: корутина
        /// держит его, пока спрашивает.
        virtual wxl::async::detached_task deleteSkin(u16_text name) = 0;

    protected:
        ~Actions() = default;
    };

    /// @param view полоса набора: у неё панель и спрашивает книгу, и ей же
    ///        отдаёт переходы. Панель без книги бессмысленна, поэтому связь
    ///        прямая, а не через десяток обработчиков.
    /// @param settings настройки вида: ползунки привязаны к их полям (`Bind`),
    ///        и то же поле двигают колесо и клавиши полосы — панель узнаёт о
    ///        них привязкой, а не флагом «это мы сами».
    /// @param places оглавление и закладки открытой книги: списки вкладок и
    ///        подписи над ними привязаны к ним.
    /// @param themes темы и обложки одним списком — строки правого ящика.
    /// @param search модель поиска: поле, подпись и находки привязаны к ней.
    ReaderPanel(const wxl::Compositor& compositor, BookView& view, Settings& settings,
                BookPlaces& places, ThemeList& themes, BookSearch& search, Actions& actions);
    ~ReaderPanel();

    /// Элемент, который кладут поверх полосы набора.
    const wxl::UIElement& root() const { return root_; }

    /// Открывает оба ящика, левый — на этой вкладке. Открытые — переключают
    /// вкладку.
    void open(Tab tab);

    /// Открывает оба ящика на прежней вкладке или закрывает открытые: то, что
    /// делают F2 и правая кнопка по странице.
    void toggle();

    void close();
    bool isOpen() const { return open_; }

private:
    void buildTree();
    wxl::UIElement buildContents();
    wxl::UIElement buildSearch();
    wxl::UIElement buildBookmarks();
    wxl::UIElement buildSettings();

    /// Визуал ящика с выездом по Translation; ящик ждёт за краем окна, на
    /// `offscreen` от своего места.
    wxl::Visual slidingVisual(const wxl::UIElement& box, float offscreen);

    /// Везёт ящик к смещению `x`: ноль — на место, ±ширина — за край.
    void slide(wxl::Visual& visual, float x);

    /// Показывает оба ящика, если они спрятаны.
    void show();

    void showTab(Tab tab);

    /// Номер темы как выбор в списке тем: строка `n` — тема `n`; номер за
    /// краем строк — без выбора.
    int selectionOf(int theme) const;

    // Слушатели полей — члены noexcept: событию поля некому отдать
    // исключение.

    /// Тема полосы сменилась — выбор в списке тем идёт за ней: тему меняют и
    /// клавишей T мимо панели. Слушатель поля `BookView::theme`.
    void themeChanged(int index) noexcept;

    /// Строки тем собраны заново (сохранили или удалили обложку): список снял
    /// выбор — вернуть его из номера темы полосы, а не из списка. Слушатель
    /// строк списка тем.
    void choicesChanged(list_change const& change) noexcept;

    /// Читатель выбрал строку в списке тем. -1 — список снял выбор сам
    /// (строки собраны заново), и тема от этого не меняется. Слушатель
    /// выбора `chosen_`.
    void themeChosen(int index) noexcept;

    /// Поиск по Enter, а не по каждой букве: искать по одной букве в романе —
    /// это тысячи находок, из которых читателю не нужна ни одна.
    void searchKeyDown(wxl::TextBox const& sender, wxl::KeyRoutedEventArgs& args);

    /// Щелчок по холсту мимо ящиков закрывает оба.
    void canvasPressed(wxl::Object const& sender, wxl::PointerRoutedEventArgs& args);

    // Щелчок по строке списка вкладки — переход к её месту в книге.
    void headingClicked(wxl::ListView const& sender, wxl::ItemClickEventArgs& args);
    void hitClicked(wxl::ListView const& sender, wxl::ItemClickEventArgs& args);
    void bookmarkClicked(wxl::ListView const& sender, wxl::ItemClickEventArgs& args);

    Actions& actions_;
    wxl::Compositor compositor_;
    BookView& view_;
    Settings& prefs_;   ///< настройки вида; settings_ ниже — правый ящик
    BookPlaces& places_;
    ThemeList& themes_;
    BookSearch& search_;

    /// Наши слушатели в чужих полях — снять за собой: полоса и список тем
    /// живут дольше панели.
    cookie_t themeWatch_;
    cookie_t choicesWatch_;

    /// Выбранная строка списка тем — к нему привязан выбор списка. Своё поле,
    /// а не номер темы полосы: собрав строки заново, список снимает выбор и
    /// пишет -1, а тема полосы от этого меняться не должна. Идёт за темой
    /// полосы (themeChanged, choicesChanged), сам ставит её (themeChosen).
    observable<int> chosen_;

    // Контролы — поля, построенные вместе с панелью: дети выше ящиков, ящики
    // выше холста, визуалы — у построенных ящиков. Поле поиска держится ради
    // фокуса при открытии вкладки.
    wxl::TextBox searchBox_;
    std::vector<wxl::UIElement> tabPages_;
    std::vector<wxl::Button> tabButtons_;

    wxl::Grid pages_;
    wxl::Border navigation_;   ///< левый ящик
    wxl::Border settings_;     ///< правый ящик
    wxl::Grid root_;           ///< холст на оба ящика; щелчок по нему закрывает
    wxl::Visual navigationVisual_;   ///< для выезда и ухода
    wxl::Visual settingsVisual_;
    wxl::LinearEasingFunction linear_;   ///< одна на все выезды

    Tab tab_ = Tab::Contents;
    bool open_ = false;
};

}  // namespace bukvitsa::reader
