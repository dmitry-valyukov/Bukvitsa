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
#include <functional>
#include <vector>

#include "Object.h"
#include "pch.h"

// Последними: они ведут к модели книги и реестру, а те импортируют wxl.core,
// после чего стандартный заголовок MSVC уже не принимает.
#include "book_view.h"
#include "bukvitsa/reader/library.h"

namespace bukvitsa::reader {

class ReaderPanel {
public:
    /// Вкладки левого ящика. Вид — не вкладка, а правый ящик целиком.
    enum class Tab { Contents, Search, Bookmarks };

    /// @param view полоса набора: у неё панель и спрашивает книгу, и ей же
    ///        отдаёт переходы. Панель без книги бессмысленна, поэтому связь
    ///        прямая, а не через десяток обработчиков.
    /// @param settings настройки вида: ползунки привязаны к их полям (`Bind`),
    ///        и то же поле двигают колесо и клавиши полосы — панель узнаёт о
    ///        них привязкой, а не флагом «это мы сами».
    ReaderPanel(const wxl::Compositor& compositor, BookView& view, Settings& settings);
    ~ReaderPanel();

    /// Элемент, который кладут поверх полосы набора.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Открывает оба ящика, левый — на этой вкладке. Открытые — переключают
    /// вкладку.
    void open(Tab tab);

    /// Открывает оба ящика на прежней вкладке или закрывает открытые: то, что
    /// делают F2 и правая кнопка по странице.
    void toggle();

    void close();
    bool isOpen() const { return open_; }

    /// Закладки текущей книги. Панель их показывает и меняет, а хранит и
    /// пишет приложение: файл книги — не её забота.
    void setState(BookState* state);

    /// Закладки изменились — пора записать состояние книги.
    std::function<void()> onStateChanged;

    /// Читатель попросился на полку — выбрать другую книгу. Панель этого не
    /// умеет и не должна: смена книги — дело приложения.
    std::function<void()> onLibrary;

    /// Читатель попросил новую обложку. Мастер — оверлей приложения, а не
    /// панели: он ложится поверх всей полосы.
    std::function<void()> onAddSkin;

    /// Шестерёнка у обложки: открыть в мастере правку её кривизны.
    std::function<void(u16_text)> onEditSkin;

    /// Корзина у обложки: убрать её из реестра вместе со снимком. Спросить,
    /// точно ли, — дело приложения: у панели нет ни окна, ни права решать за
    /// читателя, а удаление необратимо.
    std::function<void(u16_text)> onDeleteSkin;

    /// Пересобирает список тем: встроенные плюс обложки из полосы набора.
    /// Зовётся приложением, когда реестр обложек изменился.
    void refreshThemes();

private:
    void buildTree();
    wxl::UIElement buildContents();
    wxl::UIElement buildSearch();
    wxl::UIElement buildBookmarks();
    wxl::UIElement buildSettings();

    /// Ящик у левой или правой стенки окна: тёмный, во всю высоту, с кромкой
    /// со стороны страницы; щелчки и колесо по себе гасит.
    wxl::Border box(wxl::HorizontalAlignment side, const wxl::UIElement& inside);

    /// Визуал ящика с выездом по Translation; ящик ждёт за краем окна, на
    /// `offscreen` от своего места.
    wxl::Visual slidingVisual(const wxl::UIElement& box, float offscreen);

    /// Везёт ящик к смещению `x`: ноль — на место, ±ширина — за край.
    void slide(wxl::Visual& visual, float x);

    /// Показывает оба ящика, если они спрятаны.
    void show();

    /// Кнопка вкладки: заголовок плюс переключение.
    wxl::Button tabButton(zstring_view said, Tab tab);

    /// Строка списка — то, из чего собраны все три списка панели.
    wxl::Button listItem(zstring_view said, zstring_view under, float indent,
                         std::function<void()> action);

    void showTab(Tab tab);

    /// Отмечает кнопку нынешней темы — как отмечена открытая вкладка. Слушатель
    /// поля `BookView::theme`: тему меняют и клавишей T мимо панели.
    void markTheme();

    void fillContents();
    void fillBookmarks();
    void runSearch();
    void toggleBookmark();

    wxl::Compositor compositor_;
    BookView& view_;
    Settings& prefs_;   ///< настройки вида; settings_ ниже — правый ящик

    /// Наш слушатель в поле темы полосы — снять за собой: полоса может пережить
    /// панель.
    cookie_t themeWatch_;
    BookState* state_ = nullptr;

    nullable<wxl::Grid> root_ = nullptr;          ///< холст на оба ящика; щелчок по нему закрывает
    nullable<wxl::Border> navigation_ = nullptr;   ///< левый ящик
    nullable<wxl::Border> settings_ = nullptr;     ///< правый ящик
    nullable<wxl::Visual> navigationVisual_ = nullptr;   ///< для выезда и ухода
    nullable<wxl::Visual> settingsVisual_ = nullptr;
    nullable<wxl::LinearEasingFunction> linear_ = nullptr;   ///< одна на все выезды

    nullable<wxl::Grid> pages_ = nullptr;
    std::vector<wxl::UIElement> tabPages_;
    std::vector<wxl::Button> tabButtons_;
    std::vector<wxl::Button> themeButtons_;
    nullable<wxl::StackPanel> themesPanel_ = nullptr;   ///< пересобирается

    nullable<wxl::StackPanel> contentsList_ = nullptr;
    nullable<wxl::StackPanel> searchList_ = nullptr;
    nullable<wxl::StackPanel> bookmarkList_ = nullptr;
    nullable<wxl::TextBox> searchBox_ = nullptr;
    nullable<wxl::TextBlock> searchNote_ = nullptr;
    nullable<wxl::TextBlock> bookmarkNote_ = nullptr;

    Tab tab_ = Tab::Contents;
    bool open_ = false;
};

}  // namespace bukvitsa::reader
