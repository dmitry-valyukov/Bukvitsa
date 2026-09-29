#pragma once

#define NODRAWTEXT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// GetCurrentTime() из windows.h (WinBase.h; разворачивается в GetTickCount())
// иначе подставляется прямо в объявление Storyboard::GetCurrentTime из
// Xaml.Media.
#undef GetCurrentTime
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <unknwn.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11_1.h>

// Раскодирование картинки с диска. LoadedImageSurface тут не годится: он
// живёт в XAML (и в Windows.UI.Xaml, и в Microsoft.UI.Xaml), а XAML в этом
// песочнике не поднят -- отсюда и REGDB_E_CLASSNOTREG. WIC же обычный COM,
// он есть всегда.
#include <wincodec.h>

// Подключаем С++/WinRT
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <windows.ui.composition.interop.h>
