#pragma once
// Обстановка читалки — цвета ящиков панели, карточки мастера и полки. Они
// идут за темой бумаги, а тему меняют на ходу, поэтому это не константы, а
// поля observable<Brush> одной модели: экраны привязывают к ним свои кисти
// через Bind{}, и follow() перекрашивает всё разом, никого не держа за ручку.
//
// Модель со счётчиком ссылок из STA-пула: держит её приложение до самого
// конца работы, экраны получают ссылку и не владеют. Заставка сюда не
// смотрит: её карточка лежит на картинке и тёмная при любой теме.

#include "theme.h"

#include "Bind.h"
#include "pch.h"

namespace bukvitsa::reader {

struct Chrome : sta_refcounted {
    static intrusive_ptr<Chrome> create(const Theme& theme);

    observable<wxl::Brush> face;     ///< ящик и карточка над страницей, слегка прозрачные
    observable<wxl::Brush> paper;    ///< фон полки
    observable<wxl::Brush> card;     ///< карточка книги на полке
    observable<wxl::Brush> ink;      ///< текст
    observable<wxl::Brush> dim;      ///< подписи и второстепенное
    observable<wxl::Brush> edge;     ///< кромки и рамки
    observable<wxl::Brush> active;   ///< отметка выбранного: вкладка, тема
    observable<wxl::Brush> subtle;   ///< лицо кнопки отмены: серее остальных

    /// Перекрашивает всё под тему бумаги.
    void follow(const Theme& theme);

private:
    explicit Chrome(const Theme& theme);
};

/// Цвет темы, как его хочет остров: тема хранит цвета так, как их берёт
/// Direct2D. Прозрачность — своя у каждого употребления.
wxl::ARGB argbOf(D2D1_COLOR_F color, uint8_t alpha);

}  // namespace bukvitsa::reader
