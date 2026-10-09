#pragma once
// Витрина хранилища: полка с книгами, которые читалка знает.
//
// Полка — список, привязанный к карточкам реестра (`Library::cards`): книга,
// добавленная обходом каталога, встаёт одной карточкой, прочитанный реестр —
// одним сбросом, и ничего не пересобирается на каждый показ. Строка прогресса
// — поле самой карточки: проступает на ней, когда прочитан файл состояния
// книги, не трогая соседей. Список виртуальный: карточки и их обложки
// строятся только для тех книг, что на экране.
//
// Как и остальные экраны, класс не наследуется от визуальных компонент, а
// держит их внутри себя.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <filesystem>

#include "Object.h"
#include "pch.h"

// Последним: реестр импортирует wxl.core.
#include "bukvitsa/reader/library.h"

// Импорт — последним: намерения экрана — корутины `detached_task`.
import wxl.async;

namespace bukvitsa::reader {

/// Витрина. Владеет своим деревом; карточки реестра, галочка настроек и
/// исполнитель намерений — владельца, живут дольше витрины.
class LibraryScreen : private noncopyable {
public:
    /// Что витрина умеет попросить. Реализует владелец; намерение — корутина:
    /// ждёт ли дело диска, экрану знать незачем.
    class Actions {
    public:
        /// Щелчок по карточке: открыть книгу с этим guid. По значению —
        /// корутина держит его и после первого ожидания.
        virtual wxl::async::detached_task openBook(u16_text guid) = 0;
        /// «Добавить книгу».
        virtual wxl::async::detached_task chooseBook() = 0;
        /// «Назад».
        virtual wxl::async::detached_task back() = 0;

    protected:
        ~Actions() = default;
    };

    /// @param cards карточки реестра: полка привязана к ним и идёт за ними
    ///        сама — и за новыми книгами, и за строкой прогресса каждой.
    /// @param continueReading галочка «продолжать чтение при старте»
    ///        привязана к этому полю настроек прямо в разметке.
    /// @param covers каталог, где лежат обложки книг из реестра.
    LibraryScreen(observable_list<intrusive_ptr<ShelfCard> const>& cards, observable<bool>& continueReading,
                  std::filesystem::path covers, Actions& actions);

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_; }

private:
    /// Корень берёт фокус, как только он в дереве.
    void loaded(wxl::Grid const& self);

    /// Щелчок по карточке: открыть её книгу.
    void cardClicked(wxl::ListView const& sender, wxl::ItemClickEventArgs& args);

    Actions& actions_;

    /// Карточки реестра: по ним щелчок находит свою книгу.
    observable_list<intrusive_ptr<ShelfCard> const>& cards_;

    wxl::Grid root_;
};

}  // namespace bukvitsa::reader
