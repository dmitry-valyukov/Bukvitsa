#pragma once
// FieldWatch — наш слушатель чужого наблюдаемого поля, который снимает себя
// сам, умирая.
//
// Поле обычно живёт дольше слушателя (настройки — всё приложение), а
// слушатель держит this своего владельца: забытое снятие оставило бы в поле
// вызов в разрушенный объект. Подписка — в конструкторе держателя, снятие — в
// его деструкторе: между ними держатель и есть подписка, и ни недостроенный
// владелец (его построенные члены разрушаются и при исключении), ни забытая
// строка деструктора её не оставят.
//
// Владение: поле — чужое и живёт дольше держателя; куки подписки — держателя.

#include <utility>

// Импорт последним: поле и его куки — wxl.core.
import wxl.core;

namespace bukvitsa::reader {

/// Подписка на перемены поля на всю жизнь держателя. Неперемещаема: куки
/// снимается ровно один раз, тем, кто её получил.
template <class T>
class FieldWatch : private noncopyable {
public:
    /// @param watchedField поле, за которым следим; живёт дольше держателя.
    /// @param fieldListener слушатель перемен — noexcept, как всякий слушатель
    ///        поля: событию поля некому отдать исключение.
    template <class Listener>
    FieldWatch(observable<T const>& watchedField, Listener&& fieldListener)
        : watchedField_(watchedField),
          watchCookie_(watchedField.on_change(std::forward<Listener>(fieldListener))) {}

    /// Снимает подписку. Не нашлась она в поле — куки снял кто-то другой или
    /// поле подменили: держатель уже не знает, кого слушает и кто слушает его,
    /// и это остановка, а не тихий уход.
    ~FieldWatch() {
        if (!watchedField_.remove_change(watchCookie_))
            wxl::core::abort("FieldWatch: подписки в поле уже нет");
    }

private:
    observable<T const>& watchedField_;
    cookie_t watchCookie_;
};

}  // namespace bukvitsa::reader
