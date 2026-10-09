#pragma once
// Приложение: окно, рабочее место, модели и экраны — один объект, и он же
// посредник между экранами.
//
// Экран не знает ни приложения, ни другого экрана, ни рабочего места: он
// показывает свою модель и сообщает о намерениях читателя через свой
// интерфейс `Actions`. Приложение реализует все `Actions` разом — забытое
// намерение не собирается — и знает всех: что чем сменяется, куда ведёт
// клавиша, что записать по дороге. Предметная логика — в библиотеке
// (`Workspace`, `ThemeList`, `WarmSlot`, `KeyMap`, `SkinEditor`, `PageFlow`),
// здесь только связки, поэтому методы тонкие.
//
// Намерение и есть сценарий: метод `Actions` — корутина-член `detached_task`,
// и жизнь приложения на время её кадра держит промис (wxl.async: первый
// аргумент, считающий ссылки, — у члена это сам объект). Шаг, который ждёт и
// отдаёт значение, — `task<T>`-член; продолжений-колбэков нет.

#include <filesystem>
#include <vector>

#include "CompositionWindow.h"
#include "Object.h"
#include "pch.h"
#include "skin_wizard.h"
#include "start_screen.h"

// Последними: они ведут к модели книги и реестру, а те импортируют wxl.core,
// после чего стандартный заголовок MSVC уже не принимает.
#include "autosave.h"
#include "book_view.h"
#include "library_screen.h"
#include "notices.h"
#include "reader_panel.h"
#include "bukvitsa/reader/book_places.h"
#include "bukvitsa/reader/book_search.h"
#include "bukvitsa/reader/key_map.h"
#include "bukvitsa/reader/library.h"
#include "bukvitsa/reader/skins.h"
#include "bukvitsa/reader/theme_list.h"
#include "bukvitsa/reader/warm_slot.h"
#include "bukvitsa/reader/workspace.h"

// Импорт — последним: сценарии — корутины wxl.async.
import wxl.async;

namespace bukvitsa::reader {

/// Читалка целиком: окно, рабочее место, модели и экраны — поля по значению,
/// в порядке жизни (строятся сверху вниз, умирают снизу вверх). Одно
/// выделение из STA-пула, счётчик без interlocked — поток один. Создаётся
/// `make_refcounted<App>()`; нигде не передаётся по значению, поля хранят
/// голый `this` — сильная ссылка из своего же поля была бы кольцом.
class App final : public sta_refcounted,
                  private noncopyable,
                  private StartScreen::Actions,
                  private LibraryScreen::Actions,
                  private BookView::Actions,
                  private ReaderPanel::Actions,
                  private SkinWizard::Actions {
public:
    /// Строит окно, модели и экраны и связывает их. Диска не трогает и
    /// сценариев не запускает: окно невидимо, рабочее место пусто.
    App();

    /// Запуск: настройки, реестр, первый экран и только потом — показ окна.
    /// Корутина: идёт сама, и её промис держит приложение, пока она ждёт.
    wxl::async::detached_task start();

    /// Окно закрывают: отложенные записи — сейчас. Его зовёт Closed окна,
    /// пока окно ещё есть: место окна спрашивается у него самого.
    void close();

private:
    using detached_task = wxl::async::detached_task;

    // ---- намерения экранов ----
    //
    // Каждое — корутина-член. Синхронное намерение — такая же корутина без
    // ожиданий: кадр из пула, цена на щелчок — ничто, а экрану всё равно,
    // ждёт ли дело диска.

    // StartScreen::Actions (showLibrary и chooseBook — общие с полкой и панелью)
    detached_task continueReading() override;
    detached_task showLibrary() override;
    detached_task chooseBook() override;
    detached_task chooseFolder() override;
    detached_task quit() override;

    // LibraryScreen::Actions
    detached_task openBook(u16_text guid) override;
    detached_task back() override;

    // BookView::Actions
    detached_task togglePanel() override;

    // ReaderPanel::Actions
    detached_task toggleBookmark() override;
    detached_task addSkin() override;
    detached_task editSkin(u16_text name) override;
    detached_task deleteSkin(u16_text name) override;

    // SkinWizard::Actions
    detached_task saveSkin(Skin skin, std::filesystem::path photo) override;
    detached_task chooseAnotherImage() override;
    detached_task leaveWizard() override;

    // ---- сценарии и шаги ----

    /// Открывает книгу: от байтов на диске до страницы на экране. Им
    /// кончаются продолжение, полка, выбор файла и перетаскивание в окно.
    detached_task open(std::filesystem::path path);

    /// Греет книгу под кнопкой «Продолжить чтение», пока читатель смотрит
    /// на заставку.
    detached_task warmBook(std::filesystem::path path);

    /// Достраивает полку прогрессом книг — по файлу состояния на книгу.
    detached_task fillProgress();

    /// Приносит полосе снимок подложки, который ей теперь нужен.
    detached_task loadBackdrop(std::filesystem::path file);

    /// Годится ли снимок для мастера: читается и раскодируется. Нет —
    /// читателю сказано, что именно не так.
    wxl::async::task<bool> checkImage(std::filesystem::path image);

    // ---- навигация, клавиши, связки ----

    /// Экран становится текущим: что показано — для клавиш и тема диалогов
    /// (screenTheme_ следует за shown_), содержимое окна, задник, сообщения,
    /// ждавшие экрана.
    void show(Screen next);

    /// Клавиши, общие для всех экранов: на пути вниз, у корней трёх экранов.
    void onKey(wxl::Object const& sender, wxl::KeyRoutedEventArgs& args);

    /// Пауза заставки кончилась: проступают кнопки.
    void revealStart();

    /// Обложки из реестра — в список тем, полосу и панель; снимок подложки —
    /// заново: пересохранённая обложка могла сменить его.
    void setSkins();

    // Слушатели полей — члены noexcept: событию поля некому отдать
    // исключение. Член, который показывает значение, берёт его; где значение
    // лишь сигнал — член назван по событию.

    /// Тема полосы сменилась: снимок подложки, может быть, нужен другой.
    void themeChanged(int index) noexcept;

    /// Предпросмотр мастера: полоса рисуется с этими кривыми. Слушатель
    /// правимой обложки мастера.
    void preview(Skin const& skin) noexcept;

    /// Какой снимок подложки нужен полосе сейчас — у предпросмотра его, у
    /// обложки свой, у темы никакого, — и, если другой, принести его.
    void showBackdrop();

    // ---- поля: в порядке жизни ----

    /// Окно — первым: переживает всех, у него композиторы экранов и полосы.
    /// Тегами в скобках; Closed пишет отложенное.
    wxl::CompositionWindow window_;

    /// Рабочее место: настройки, реестр, обложки и все файлы. Пустое: его
    /// наполнит запуск, и наполнит асинхронно — в этом потоке к диску не
    /// обращаются вовсе. Экраны берут его настройки и реестр.
    Workspace ws_;

    /// Темы и обложки одним списком — номер темы полосы ↔ имена в настройках.
    /// Заполняется там же и тем же, что список полосы (setSkins).
    ThemeList themes_;

    /// Слот прогретой книги: в него запуск кладёт ту, что стоит на кнопке
    /// «Продолжить чтение», разобрав её заранее.
    WarmSlot warm_;

    /// Что показано, — для клавиш и полки; откуда открыли книгу, — для
    /// Escape: читатель, выбравший её на полке, ждёт полку обратно, а не
    /// заставку. Три экрана — три содержимого одного окна; второго окна нет
    /// намеренно: оно завело бы вторую кнопку на панели задач.
    observable<Screen> shown_{Screen::Start};
    observable<Screen> cameFrom_{Screen::Start};

    /// Тема XAML показанного экрана: полоса тёмная, полка и заставка светлые —
    /// так их корни и задают. Следует за shown_. Диалог — не часть дерева
    /// экрана и его темы не наследует: сообщения и вопрос об удалении обложки
    /// берут её отсюда.
    observable<wxl::ElementTheme> screenTheme_{wxl::ElementTheme::Light};

    /// Сообщения читателю и обработчик сбоев сценариев.
    Notices notices_;

    /// Оглавление и закладки открытой книги — списки, к которым привязана
    /// панель; запись берёт из них закладки, а место — у полосы. Новая книга
    /// меняет их содержимое раньше, чем прежняя уйдёт с полосы.
    BookPlaces places_;

    /// Поиск по открытой книге: панель к нему привязана; новая книга его
    /// сбрасывает.
    BookSearch search_;

    // Экраны — в прежнем порядке создания. Полоса выше панели: панель держит
    // ссылку на неё и снимает слушателя с её темы. Экраны неперемещаемы —
    // держат `this` в своих подписках — и строятся прямо на месте; намерения
    // им отдаёт `*this` — базы строятся раньше полей, а зовут их не раньше,
    // чем конструктор кончится.
    StartScreen start_;
    BookView view_;
    LibraryScreen shelf_;
    SkinWizard wizard_;   ///< оверлей поверх полосы и панели
    ReaderPanel panel_;   ///< ящики поверх страницы — внутри полосы, а не рядом с ней

    /// Отложенные записи — после всего, за чем следят: снимают свои подписки,
    /// пока окно, полоса и настройки живы.
    Autosave autosave_;

    /// Пауза заставки — таймером очереди, а не сном: поток окна свободен.
    wxl::DispatcherQueueTimer splash_;

    /// Снимок подложки, который у полосы есть или заказан; пусто — тема без
    /// снимка. По нему видно, тот же нужен файл или другой: две обложки могут
    /// делить снимок, и читать его заново на каждую смену темы незачем.
    std::filesystem::path backdrop_;
};

}  // namespace bukvitsa::reader
