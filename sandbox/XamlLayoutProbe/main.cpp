// Проба: догонит ли XAML рамку при быстром ресайзе, если заставить его
// пересчитать вёрстку СИНХРОННО в обработчике WM_SIZE (UpdateLayout), не выходя
// из него. Гипотеза: если да — композиторный задник и отдельный визуал не
// нужны, хватит встроенного XAML.
//
// Устройство. Своё окно WS_EX_NOREDIRECTIONBITMAP (как IslandProbe: без
// классовой кисти, WM_ERASEBKGND→1 — белому взяться неоткуда). В нём остров
// DesktopWindowXamlSource во весь клиент. XAML: оранжевый Grid (любой
// недорисованный край виден мгновенно), заставка Image UniformToFill во всё
// окно и синяя плашка, приколотая к правому нижнему углу (по ней видно,
// поспевает ли вёрстка контролов за углом).
//
// Клавиша L включает UpdateLayout прямо в WM_SIZE. Смысл опыта: без него XAML
// откладывает вёрстку на «потом» и содержимое отстаёт от рамки; вопрос — хватит
// ли одного UpdateLayout, чтобы догнать (вёрстка синхронна), или отставание
// остаётся из-за отрисовки/коммита, который UpdateLayout не форсирует.
#include <windows.h>

#include <MddBootstrap.h>

// GetCurrentTime из windows.h — макрос, иначе подставится в Storyboard.
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "coremessaging.lib")

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace mud = winrt::Microsoft::UI::Dispatching;

constexpr const wchar_t* kSplashUri =
    L"file:///M:/worktrees/reader-fixes/Reader/src/Assets/splash-screen-1k.png";

mud::DispatcherQueueController g_uiQueue{nullptr};
xaml::Hosting::WindowsXamlManager g_manager{nullptr};
xaml::Hosting::DesktopWindowXamlSource g_island{nullptr};
xaml::FrameworkElement g_root{nullptr};

HWND g_hwnd = nullptr;
bool g_eager = false;   // звать ли UpdateLayout в WM_SIZE (клавиша L)

winrt::Windows::UI::Color color(uint8_t r, uint8_t g, uint8_t b) { return {255, r, g, b}; }

void applyTitle();   // определена ниже; зовётся из обработчика клавиши в buildXaml

xaml::UIElement buildXaml() {
    xaml::Controls::Grid grid;
    // Оранжевый грунт: если Image или плашка отстанут, оранжевое проступит.
    grid.Background(xaml::Media::SolidColorBrush{color(0xE0, 0x60, 0x10)});

    xaml::Controls::Image image;
    xaml::Media::Imaging::BitmapImage bitmap;
    bitmap.UriSource(winrt::Windows::Foundation::Uri{kSplashUri});
    image.Source(bitmap);
    image.Stretch(xaml::Media::Stretch::UniformToFill);
    grid.Children().Append(image);

    // Плашка приколота к правому нижнему углу: по ней видно, поспевает ли вёрстка
    // контролов за углом окна при растяжке.
    xaml::Controls::Border corner;
    corner.Width(240);
    corner.Height(130);
    corner.Background(xaml::Media::SolidColorBrush{color(0x20, 0x60, 0xE0)});
    corner.HorizontalAlignment(xaml::HorizontalAlignment::Right);
    corner.VerticalAlignment(xaml::VerticalAlignment::Bottom);
    corner.Margin(xaml::Thickness{40, 40, 40, 40});
    grid.Children().Append(corner);

    // Клавиша ловится в XAML, а не в WndProc: у острова фокус, и WM_KEYDOWN до
    // оконной процедуры не доходит. Корень берёт фокус на себя (как полоса
    // читалки) и слушает L на пути вниз, чтобы работало независимо от фокуса.
    grid.IsTabStop(true);
    grid.Loaded([](winrt::Windows::Foundation::IInspectable const& sender,
                   xaml::RoutedEventArgs const&) {
        sender.as<xaml::UIElement>().Focus(xaml::FocusState::Programmatic);
    });
    grid.PreviewKeyDown([](winrt::Windows::Foundation::IInspectable const&,
                           xaml::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::L) {
            g_eager = !g_eager;
            applyTitle();
            args.Handled(true);
        }
    });

    return grid;
}

void applyTitle() {
    ::SetWindowTextW(g_hwnd,
                     g_eager ? L"XamlLayoutProbe — UpdateLayout в WM_SIZE: ВКЛ (жми L — выкл): "
                               L"тяни угол быстро"
                             : L"XamlLayoutProbe — UpdateLayout в WM_SIZE: ВЫКЛ (жми L — вкл): "
                               L"тяни угол быстро");
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && g_island) {
                int const cw = LOWORD(lparam);
                int const ch = HIWORD(lparam);
                g_island.SiteBridge().MoveAndResize({0, 0, cw, ch});
                // Вот он, предмет опыта: заставить XAML пересчитать вёрстку
                // синхронно, не выходя из обработчика.
                if (g_eager && g_root) g_root.UpdateLayout();
            }
            break;

        case WM_KEYDOWN:
            if (wparam == 'L') {
                g_eager = !g_eager;
                applyTitle();
            }
            break;

        case WM_DESTROY:
            g_uiQueue.DispatcherQueue().EnqueueEventLoopExit();
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    winrt::check_hresult(::MddBootstrapInitialize(0x00020000, nullptr, {}));
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    g_uiQueue = mud::DispatcherQueueController::CreateOnCurrentThread();
    g_manager = xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"BukvitsaXamlLayoutProbe";
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    ::RegisterClassExW(&wc);

    g_hwnd = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr, instance,
                               nullptr);
    if (!g_hwnd) return 1;

    g_island = xaml::Hosting::DesktopWindowXamlSource{};
    g_island.Initialize(winrt::Microsoft::UI::WindowId{reinterpret_cast<uint64_t>(g_hwnd)});

    auto const content = buildXaml();
    g_root = content.as<xaml::FrameworkElement>();
    g_island.Content(content);

    RECT client{};
    ::GetClientRect(g_hwnd, &client);
    g_island.SiteBridge().MoveAndResize({0, 0, client.right, client.bottom});
    g_island.SiteBridge().Show();

    ::ShowWindow(g_hwnd, show);
    ::UpdateWindow(g_hwnd);
    applyTitle();

    g_uiQueue.DispatcherQueue().RunEventLoop();
    return 0;
}
