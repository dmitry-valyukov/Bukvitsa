#pragma once
// Витрина хранилища: полка с книгами, которые читалка знает.
//
// Полка строится обычным циклом, а не разметкой с шаблоном элемента: книг
// столько, сколько их в реестре, и это число известно только во время работы,
// тогда как повторители нашего синтаксиса (`repeat`, `iterate`) считают на
// этапе компиляции. Цикл, добавляющий детей в панель, — то же самое, только
// без посредника, и читается он лучше, чем шаблон с привязкой.
//
// Как и остальные экраны, класс не наследуется от визуальных компонент, а
// держит их внутри себя.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "Object.h"
#include "pch.h"

// Последними: реестр и настройки импортируют wxl.core.
#include "bukvitsa/reader/library.h"
#include "bukvitsa/reader/settings.h"
#include "bukvitsa/reader/workspace.h"

namespace bukvitsa::reader {

class LibraryScreen {
public:
    /// @param workspace рабочее место: галочка «продолжать чтение при старте»
    ///        привязана к полю его настроек прямо в разметке, а обложки
    ///        карточек лежат в его кэше.
    explicit LibraryScreen(Workspace& workspace);

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_; }

    /// Перестраивает полку под содержимое реестра. Зовётся каждый раз, когда
    /// витрину показывают: книга могла добавиться, а место чтения — уехать.
    void show(const Library& library);

    /// Ставит на полку ещё одну книгу -- ту, которую только что разобрал обход
    /// каталога. Полка при этом не пересобирается: карточки, которые уже стоят,
    /// остаются как есть вместе со своим прогрессом.
    void appendBook(const BookEntry& entry);

    /// Дописывает на карточке строку прогресса -- ту самую, которой при показе
    /// ещё не было, потому что файл состояния книги читается в фоне.
    ///
    /// Ничего не делает, если полку с тех пор пересобрали: карточки той книги
    /// уже нет, а есть новая, и её прогресс придёт своим чередом.
    void setProgress(u16_view guid, uint32_t charOffset, size_t bookmarks);

    std::function<void(u16_text)> onOpen;   ///< guid выбранной книги
    std::function<void()> onAddBook;
    std::function<void()> onBack;

private:
    wxl::Button shelfItem(const BookEntry& book);

    // Контролы — поля, построенные вместе с витриной: дети выше корня.
    wxl::StackPanel shelf_;
    wxl::TextBlock emptyNote_;
    wxl::Grid root_;

    /// Строка прогресса каждой карточки, по guid книги. Живёт ровно от одного
    /// показа полки до другого -- как и сами карточки.
    std::map<std::u16string, wxl::TextBlock> progress_;

    /// Реестр, по которому построена нынешняя полка. Не владеет: реестр живёт
    /// в приложении и переживает витрину.
    const Library* shown_ = nullptr;
    const Workspace& workspace_;   ///< где лежат обложки книг
};

}  // namespace bukvitsa::reader
