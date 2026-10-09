#pragma once
// Отложенные записи читалки: место окна, место чтения, настройки вида.
//
// Одна забота со своим состоянием — таймерами пауз. Окно таскают непрерывно,
// страницы листают подряд, ползунок шлёт перемену на каждый сдвиг, а файл
// настроек и файл состояния книги переписываются целиком: писать на каждую
// перемену незачем. Каждая перемена отодвигает свою запись на паузу; закрытие
// окна пишет всё сразу (`flush`), так что пауза ничего не теряет — она только
// страхует от того, что до закрытия дело не дойдёт.
//
// Что менялось, Autosave узнаёт сам — подписками на окно, полосу и поля
// настроек; чьё оно, ему знать незачем. Запись — корутина, которая ждёт
// рабочее место владельца, поэтому Autosave держит якорь его жизни (`owner`):
// корутина записи берёт якорь первым параметром, и промис `detached_task`
// держит его до конца кадра.

#include <utility>
#include <vector>

#include "CompositionWindow.h"
#include "Object.h"
#include "pch.h"

// Последними: они ведут к модели книги и настройкам, а те импортируют
// wxl.core, после чего стандартный заголовок MSVC уже не принимает.
#include "book_view.h"
#include "bukvitsa/reader/book_places.h"
#include "bukvitsa/reader/theme_list.h"
#include "bukvitsa/reader/workspace.h"

namespace bukvitsa::reader {

/// Отложенные записи. Владеет своими таймерами и подписками (снимает их за
/// собой); окно, рабочее место, список тем, полоса и места книги —
/// владельца `owner`, живут дольше Autosave.
class Autosave : private noncopyable {
public:
    /// Подписывается на место окна и место чтения сразу, на тему — тоже: её
    /// слушатель пишет, только если имена в настройках изменились.
    /// @param owner якорь жизни: его держит корутина каждой записи, пока ждёт
    ///        диска. Тип владельца Autosave не знает.
    /// @param places закладки и guid открытой книги: в файл состояния под
    ///        этим guid закладки уходят вместе с местом чтения.
    Autosave(refcounted& owner, const wxl::CompositionWindow& window, Workspace& workspace,
             const ThemeList& themes, BookView& view, const BookPlaces& places);
    ~Autosave();

    /// Настройки прочитаны — с этих пор их перемены пишутся. Раньше незачем:
    /// прочитанное из файла — не перемена, которую надо записать обратно.
    void watchSettings();

    /// Состояние открытой книги — место чтения и закладки — на диск сейчас:
    /// уходя из книги и поставив закладку, паузы не ждут. Книга закрыта —
    /// писать нечего: место чтения принадлежит ей, а не окну, и ноль
    /// незанятой полосы стёр бы то, что уже записано. Пишется под guid книги
    /// на полосе — его знают её места (`BookPlaces::guid`), — а не последней
    /// книги из настроек: те при открытии меняются раньше полосы.
    void saveState();

    /// Окно закрывают: всё отложенное — сейчас, без пауз.
    void flush();

private:
    /// Перемена отодвигает свою запись на полную паузу.
    static void later(const wxl::DispatcherQueueTimer& timer);

    /// Окно сдвинулось — запись его места отодвигается.
    void windowMoved();

    // Слушатели полей — члены noexcept: событию поля некому отдать
    // исключение. Значение каждому лишь сигнал: запись отодвигается, а пишет
    // она то, что в полях к своему часу.
    void positionChanged(uint32_t position) noexcept;
    void styleChanged(double value) noexcept;
    void continueReadingChanged(bool value) noexcept;

    /// Тема — поле полосы, а в настройках она лежит именем: обложка — своим,
    /// встроенная тема — ключом. Пишется, только если имена изменились.
    void themeChanged(int index) noexcept;

    // Тики таймеров — сами записи.
    void saveWindow();
    void savePosition();
    void saveSettings();

    refcounted& owner_;
    wxl::CompositionWindow window_;
    Workspace& ws_;
    const ThemeList& themes_;
    BookView& view_;
    const BookPlaces& places_;

    wxl::DispatcherQueueTimer windowTimer_;     ///< место окна
    wxl::DispatcherQueueTimer positionTimer_;   ///< место чтения
    wxl::DispatcherQueueTimer settingsTimer_;   ///< настройки вида и тема

    /// Наши подписки — снять за собой: окно, полоса и настройки живут дольше.
    wxl::EventToken geometryWatch_;
    cookie_t positionWatch_;
    cookie_t themeWatch_;
    /// Слушатели полей настроек: пусто, пока настройки не прочитаны.
    std::vector<std::pair<observable<double>*, cookie_t>> styleWatches_;
    std::vector<std::pair<observable<bool>*, cookie_t>> flagWatches_;
};

}  // namespace bukvitsa::reader
