#pragma once
// Стартовый экран: заставка, а поверх неё справа — кнопки.
//
// Отдельным окном заставку не делаем намеренно: второе окно завело бы вторую
// кнопку на панели задач, которая появляется и исчезает. Это просто
// содержимое главного окна, которое потом сменится полосой набора.
//
// Класс не наследуется от визуальных компонент, а держит их внутри себя,
// включая корневой UIElement: описание остаётся декларативным, а поведение —
// обычным событийным кодом на честных событиях WinUI.

#include <filesystem>
#include <vector>

#include "Object.h"
#include "pch.h"

// Импорт — последним: намерения экрана — корутины `detached_task`.
import wxl.async;

namespace bukvitsa::reader {

/// Стартовый экран. Владеет своим деревом и анимацией проявления; исполнитель
/// намерений — владельца, живёт дольше экрана.
class StartScreen : private noncopyable {
public:
    /// Что заставка умеет попросить — по намерению на кнопку. Реализует
    /// владелец; намерение — корутина: ждёт ли дело диска, экрану знать
    /// незачем.
    class Actions {
    public:
        /// «Продолжить чтение» — действие по умолчанию, его же зовёт Enter.
        virtual wxl::async::detached_task continueReading() = 0;
        /// «Моя библиотека».
        virtual wxl::async::detached_task showLibrary() = 0;
        /// «Добавить книгу».
        virtual wxl::async::detached_task chooseBook() = 0;
        /// «Добавить каталог».
        virtual wxl::async::detached_task chooseFolder() = 0;
        /// «Выйти из читалки» — отмена, её же зовёт Escape.
        virtual wxl::async::detached_task quit() = 0;

    protected:
        ~Actions() = default;
    };

    /// Строит дерево целиком. Кнопки создаются уже прозрачными и приподнятыми:
    /// поставь прозрачность в Loaded — и они успеют показаться на полную ровно
    /// один кадр, чего хватает, чтобы мигнуть.
    StartScreen(const wxl::Compositor& compositor, Actions& actions);

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_; }

    /// Проявление с разбегом. Зовётся, когда приложение готово: до этого
    /// момента на экране одна заставка.
    void reveal();

    /// Книга, которую продолжит большая кнопка: слева обложка, под своей
    /// надписью — название, под названием — автор. Пустое название оставляет
    /// кнопку такой, какой она была: продолжать нечего.
    void setContinueBook(u16_view bookTitle, u16_view bookAuthor,
                         const std::filesystem::path& cover);

private:
    /// Ставит кнопку в очередь проявления — прозрачной и опущенной; reveal()
    /// поднимет её в свой черёд. Вид кнопки — пресеты в разметке (look.h).
    wxl::Button revealLater(wxl::Button button);

    /// Корень берёт фокус, как только он в дереве: раньше элемент тихо
    /// отказывает.
    void loaded(wxl::Grid const& self);

    /// Enter — «Продолжить чтение», Escape — «Выйти из читалки».
    void keyDown(wxl::Object const& sender, wxl::KeyRoutedEventArgs& args);

    Actions& actions_;
    wxl::Compositor compositor_;
    std::vector<wxl::Visual> revealing_;   ///< визуалы кнопок в порядке появления

    /// Книга на большой кнопке — к ним привязана её разметка (`BindOutput`).
    /// Пустое название — книги нет, кнопка остаётся простой надписью.
    /// Одинаковое значение поле не объявляет, так что показ экрана с той же
    /// книгой ничего не перестраивает.
    observable<u16_text> bookTitle_;
    observable<u16_text> bookAuthor_;
    observable<std::filesystem::path> bookCover_;

    /// Обёртка карточки и её визуал. Проступает обёртка вместе с кнопками, а
    /// не сама карточка: у той заняты фасадные свойства (подъём по Z несёт
    /// тень), а мешать их с handout-визуалом нельзя — ломается попадание
    /// мыши. Подробности — у построения карточки в .cpp.
    wxl::Grid cardShell_;
    wxl::Visual cardVisual_;

    wxl::Grid root_;

    bool revealed_ = false;
};

}  // namespace bukvitsa::reader
