// Reader — читалка Буквицы на wxl.ui: точка входа.
//
// wWinMain, Windows App Runtime, XAML и пул STA поднимает wxl и потом зовёт
// wxl_launched. Всё, из чего собрана читалка, — один объект (`App`, app.h):
// он рождается со счётчиком 1, и make_refcounted эту ссылку принимает, а не
// добавляет свою. Обработчик Teardown держит его до разборки wxl и отпускает
// до пула STA; сценарии держат его сами, промисом своих корутин. Последние
// записи будит Closed окна (App::close), пока окно ещё есть: Teardown
// приходит уже без окна. Дописать их приложение даёт, умирая: держатели циклов
// записи докручивают их, когда обработчик Teardown его отпускает.

#include "app.h"

// Заголовок вёрстки — после своих: точку расширения остановки
// (setDeviceLossCheck) заполняет запуск.
#include "bukvitsa/typography/system_failure.h"

import wxl.core;

wxl::Teardown wxl_launched() {
    // Отказ Direct2D от потерянного устройства, где бы его ни встретила
    // stopIfFailed — в вёрстке, в модели или здесь, — внутри рисунка
    // поверхности кончает рисунок, а не читалку. Какие коды его называют и
    // потеряно ли устройство на деле, проверяет wxl — она его и заменяет; а
    // вёрстка о wxl.ui не знает, и её точку расширения заполняют здесь, до
    // первой поверхности.
    bukvitsa::typography::setDeviceLossCheck(
        [](HRESULT result) { wxl::DrawingSurface::throwIfDeviceLost(result); });

    auto const app = make_refcounted<bukvitsa::reader::App>();
    app->start();
    return [app](wxl::TeardownReason) {};
}
