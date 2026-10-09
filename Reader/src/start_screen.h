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
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Object.h"
#include "pch.h"

namespace bukvitsa::reader {

// TODO: : public wxl::core:noncopyable
class StartScreen {
public:
    /// Строит дерево целиком. Кнопки создаются уже прозрачными и приподнятыми:
    /// поставь прозрачность в Loaded — и они успеют показаться на полную ровно
    /// один кадр, чего хватает, чтобы мигнуть.
    explicit StartScreen(const wxl::Compositor& compositor);

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_; }

    /// Проявление с разбегом. Зовётся, когда приложение готово: до этого
    /// момента на экране одна заставка.
    void reveal();

    /// Книга, которую продолжит большая кнопка: слева обложка, под своей
    /// надписью — название, под названием — автор. Пустое название оставляет
    /// кнопку простой надписью: продолжать пока нечего.
    void setContinueBook(u16_view bookTitle, u16_view bookAuthor,
                         const std::filesystem::path& cover);

    /// Что делают кнопки. Пустой обработчик значит «кнопка на месте, но
    /// делать ей пока нечего» — так и задумано для каталога.
    ///
    /// «Продолжить чтение» — действие по умолчанию, его зовёт Enter;
    /// «Выйти из читалки» — отмена, её зовёт Escape.
    std::function<void()> onContinueReading;
    std::function<void()> onLibrary;
    std::function<void()> onAddBook;
    std::function<void()> onAddFolder;
    std::function<void()> onExit;

private:
    /// Ставит кнопку в очередь проявления — прозрачной и опущенной; reveal()
    /// поднимет её в свой черёд. Вид кнопки — пресеты в разметке (look.h).
    wxl::Button revealLater(wxl::Button button);

    wxl::Compositor compositor_;
    std::vector<wxl::Visual> revealing_;   ///< визуалы кнопок в порядке появления

    /// Большая кнопка и то, чем она наполнена: setContinueBook() зовут на
    /// каждом показе экрана, и одинаковое наполнение не перестраивается.
    wxl::Button continueButton_;
    std::u16string continueKey_;

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
