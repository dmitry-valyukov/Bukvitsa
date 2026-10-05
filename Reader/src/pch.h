#pragma once

// Всё, что читалке нужно от wxl, и ни одного заголовка winrt: доступ к
// декларативной поверхности не должен стоить потребителю разбора проекции.
//
// Обёртки генератора подключаются как <wxl/X.h>: их каталога нет в путях
// поиска заголовков, так требует wxl.ui.

#include "aliases.h"
#include "Card.h"
#include <wxl/Members.h>
#include <wxl/Microsoft.UI.Composition.h>
#include <wxl/Microsoft.UI.Xaml.Media.Enums.h>
#include <wxl/Microsoft.UI.Dispatching.h>
#include <wxl/Microsoft.UI.Input.h>
#include <wxl/Microsoft.UI.Windowing.h>
#include <wxl/Microsoft.UI.Xaml.Controls.h>
#include <wxl/Microsoft.UI.Xaml.Hosting.h>
#include <wxl/styles.h>
#include "launch.h"
