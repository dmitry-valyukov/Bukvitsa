// Проба «attached-остров» + эксперимент про лаг обработки WM_SIZE.
//
// База (доказана прошлыми прогонами): своё окно WS_EX_NOREDIRECTIONBITMAP, а
// содержимое — визуалы Microsoft.UI.Composition, подключённые к этому HWND
// через DesktopAttachedSiteBridge (без дочернего окна рисования; ввод острова
// идёт через InputPointerSource, служебный InputSiteWindowClass — по одному на
// остров). Значит страница читалки переезжает сюда, не меняя стек.
//
// Эксперимент (гипотеза: артефакт заднего фона — от ЗАДЕРЖКИ обработки WM_SIZE).
// Под «плашкой» лежит ядовито-магентовый фон. Плашка кроет всё окно и в WM_SIZE
// подгоняется под новый размер. Клавиша D включает блокирующую паузу ВНУТРИ
// обработчика WM_SIZE — не таймером: таймер отпустил бы WM_SIZE и применил
// размер «потом», а это другой опыт (отложенное применение). Нам нужно честно
// затормозить саму обработку сообщения, как если бы пересчёт занимал это время.
// Пауза — обычный Sleep, не SleepEx: без alertable-пробуждений, чтобы её ничто
// не прервало.
//
// Так проверяется, отстаёт ли содержимое от рамки, когда обработка WM_SIZE
// длится 100 мс. Плашка — цветом, чтобы проба не зависела от загрузки картинки;
// в настоящей читалке на её месте заставка/страница из DrawingSurface, но при
// ресайзе ведёт себя так же.
#include <windows.h>

#include <string>

#include <MddBootstrap.h>

// GetCurrentTime из windows.h — макрос, иначе подставится в Storyboard.
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "coremessaging.lib")

namespace {

namespace muc = winrt::Microsoft::UI::Composition;
namespace content = winrt::Microsoft::UI::Content;
namespace xaml = winrt::Microsoft::UI::Xaml;
namespace mud = winrt::Microsoft::UI::Dispatching;

// Пауза, на которую тормозится обработка WM_SIZE в режиме задержки.
constexpr DWORD kDelayMs = 100;

mud::DispatcherQueueController g_uiQueue{nullptr};
xaml::Hosting::WindowsXamlManager g_xaml{nullptr};

muc::Compositor g_compositor{nullptr};
muc::ContainerVisual g_root{nullptr};
muc::SpriteVisual g_backdrop{nullptr};   // магента-фон
muc::SpriteVisual g_sheet{nullptr};      // плашка — её и подгоняем в WM_SIZE
content::DesktopAttachedSiteBridge g_bridge{nullptr};

HWND g_hwnd = nullptr;

// Тормозим ли обработку WM_SIZE (клавиша D).
bool g_delay = false;

winrt::Windows::UI::Color rgb(uint8_t r, uint8_t g, uint8_t b) { return {255, r, g, b}; }

void applyTitle() {
    if (!g_hwnd) return;
    std::wstring const title =
        g_delay ? L"AttachedProbe — ЗАДЕРЖКА Sleep(100) в WM_SIZE (жми D — убрать): тяни угол "
                  L"быстро и смотри на кромку"
                : L"AttachedProbe — WM_SIZE без задержки (жми D — Sleep 100мс внутри WM_SIZE): "
                  L"тяни угол";
    ::SetWindowTextW(g_hwnd, title.c_str());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                float const width = static_cast<float>(LOWORD(lparam));
                float const height = static_cast<float>(HIWORD(lparam));

                if (g_root) g_root.Size({width, height});
                if (g_backdrop) g_backdrop.Size({width, height});

                // Задержка — блокирующая, ВНУТРИ обработчика. Не таймер: тот
                // отпустил бы WM_SIZE и применил размер «потом». Обычный Sleep,
                // не SleepEx, — чтобы паузу не прервало alertable-пробуждение.
                if (g_delay) ::Sleep(kDelayMs);

                if (g_sheet) g_sheet.Size({width, height});
            }
            break;

        case WM_KEYDOWN:
            if (wparam == 'D') {
                g_delay = !g_delay;
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
    g_xaml = xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"BukvitsaAttachedProbe";
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // без классовой кисти — белому взяться неоткуда
    ::RegisterClassExW(&wc);

    g_hwnd = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr, instance,
                               nullptr);
    if (!g_hwnd) return 1;

    winrt::Microsoft::UI::WindowId const windowId{reinterpret_cast<uint64_t>(g_hwnd)};

    g_compositor = muc::Compositor{};
    g_root = g_compositor.CreateContainerVisual();

    // Магента нарочно ядовитая: любой просвет под плашкой виден мгновенно.
    g_backdrop = g_compositor.CreateSpriteVisual();
    g_backdrop.Brush(g_compositor.CreateColorBrush(rgb(0xE0, 0x20, 0x90)));
    g_root.Children().InsertAtTop(g_backdrop);

    // Плашка кроет всё окно: пока она поспевает, магенты не видно вовсе.
    g_sheet = g_compositor.CreateSpriteVisual();
    g_sheet.Brush(g_compositor.CreateColorBrush(rgb(0x1e, 0x3a, 0x3a)));
    g_root.Children().InsertAtTop(g_sheet);

    RECT client{};
    ::GetClientRect(g_hwnd, &client);
    float const w = static_cast<float>(client.right);
    float const h = static_cast<float>(client.bottom);
    g_root.Size({w, h});
    g_backdrop.Size({w, h});
    g_sheet.Size({w, h});

    content::ContentIsland const island = content::ContentIsland::Create(g_root);
    g_bridge = content::DesktopAttachedSiteBridge::CreateFromWindowId(g_uiQueue.DispatcherQueue(),
                                                                      windowId);
    g_bridge.Connect(island);

    ::ShowWindow(g_hwnd, show);
    ::UpdateWindow(g_hwnd);
    applyTitle();

    g_uiQueue.DispatcherQueue().RunEventLoop();
    return 0;
}
