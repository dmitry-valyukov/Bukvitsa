# Можно ли получить `Microsoft.UI.Xaml.Window` для своего HWND

Исследование, сентябрь 2026. Вопрос: WinUI 3 `Window` создаёт свой HWND сам;
нельзя ли наоборот — отдать ему наш HWND (со своим классом, своей оконной
процедурой, `WS_EX_NOREDIRECTIONBITMAP`) и получить объект `Window`, возможно
через промежуточные звенья вроде `HWND → AppWindow → Window`.

## Коротко

1. **Нет.** Публичного пути от чужого HWND к `Window` нет ни в 1.x, ни в 2.x
   (проверено по справке Windows App SDK 2.0 и по исходникам WinUI на
   `main` от 28.09.2026). У `Window` единственный конструктор без параметров,
   окно он создаёт сам, класс и стили зашиты в код.
2. Цепочка `HWND → WindowId → AppWindow` для своего окна **работает**,
   но на `AppWindow → Window` она обрывается: обратного API нет.
   `Window.AppWindow` внутри — это просто `AppWindow.GetFromWindowId` от
   собственного HWND окна, а не какая-то связь, по которой можно пройти назад.
3. Даже для HWND, созданного самим `Window`, обратного поиска `HWND → Window`
   нет: таблица такая есть, но она закрытая, внутри `DXamlCore`.
4. Главное: **`Window` внутри — это ровно «свой HWND + `DesktopWindowXamlSource`
   + `AppWindow`»** (плюс обёртка `WindowChrome` для заголовка). То есть
   `wxl::CompositionWindow` и `sandbox/IslandProbe` уже собирают то же самое,
   что `Window`, только со своим классом окна. Объект `Window` нам не нужен:
   почти всё, что он даёт, достижимо через `DesktopWindowXamlSource`,
   `XamlRoot`, `AppWindow` и `InputActivationListener` (таблица ниже).

## Как устроен `Window` изнутри

WinUI 3 открыт, и это видно прямо в коде
([microsoft-ui-xaml @ 8139a87][src]).

**Окно создаётся в конструкторе, без вариантов.**
`DesktopWindowImpl::DesktopWindowImpl` вызывает `RegisterDesktopWindowClass()`
и `CreateDesktopWindow()` ([DesktopWindowImpl.cpp:61–88][ctor]). Класс
`WinUIDesktopWin32WindowClass` регистрируется со стилями
`CS_HREDRAW | CS_VREDRAW` и **кистью `COLOR_WINDOW + 1`**, окно создаётся с
`WS_OVERLAPPEDWINDOW` и нулевым расширенным стилем
([DesktopWindowImpl.cpp:1685–1723][create]). Ни параметра, ни фабрики, ни
приватного интерфейса, куда можно было бы передать готовый HWND, нет: в модели
API у `Window` объявлен один `public Window() { }`
([Microsoft.UI.Xaml.cs:3513][model]), а `IWindowPrivate` умеет только
`Show/Hide/MoveWindow` и настройки атласа ([там же, :3303][modelpriv]).

Отсюда же и белый просвет при быстрой растяжке, от которого ушла Буквица
(комментарий в `Reader/src/main.cpp` у `CompositionWindow`): у штатного окна
есть поверхность перенаправления, а класс стирает её белой системной кистью,
пока дочерний HWND острова догоняет рамку.

**Контент — через обычный `DesktopWindowXamlSource`.** В `OnCreate`
([DesktopWindowImpl.cpp:90–155][oncreate]) окно:

- записывает пару `HWND → Window` в `DXamlCore::m_handleToDesktopWindowMap`
  (закрытое поле, [DXamlCore.h:672][map]);
- создаёт `WindowChrome` — корневой элемент, внутри которого живёт наш
  `Content` и который реализует `ExtendsContentIntoTitleBar`/`SetTitleBar`;
- создаёт `DesktopWindowXamlSource` и вызывает у него
  `Initialize(GetWindowIdFromWindow(m_hwnd))` — **ровно то, что делает
  `IslandProbe` в `wWinMain`**;
- берёт `AppWindow::GetFromWindowId` сразу, «чтобы AppWindow сабклассировал
  окно при инициализации, а не когда пользовательский код впервые спросит».

`Window.AppWindow` — это тот же `AppWindow.GetFromWindowId(m_hwnd)`
([DesktopWindowImpl.cpp:2088–2101][appwindow]). В заметках команды WinUI это
сказано прямо: «AppWindow can also wrap an existing HWND (Xaml does this in
DesktopWindowImpl)» ([windowless-xaml-islands.md][windowless]).

Оконная процедура (`BaseWindow<T>::WndProc`) хранит указатель на реализацию в
**`GWLP_USERDATA`** ([BaseWindow.h:70–90][basewindow]) — это важно для хаков
ниже.

## Цепочки, звено за звеном

| Звено | API | Для своего HWND |
|---|---|---|
| HWND → `WindowId` | `GetWindowIdFromWindow` (`Microsoft.UI.Interop`) | да |
| `WindowId` → `AppWindow` | `AppWindow.GetFromWindowId` | да, для top-level окна приложения ([Manage app windows][manage]); AppWindow при этом сабклассирует окно |
| `AppWindow` → `Window` | — | **нет API**; справка прямо: «The Windows App SDK doesn't currently provide methods for attaching UI framework content to an AppWindow» ([Manage app windows, Limitations][manage]) |
| `WindowId` → `Window` | — | нет (статических членов для этого у `Window` нет) |
| элемент → `Window` | — | нет публичного; есть `XamlRoot.ContentIslandEnvironment.AppWindowId` → HWND/AppWindow ([Retrieve HWND][hwnd]) |
| `Window` → HWND | `IWindowNative::get_WindowHandle` | (обратное направление) |
| HWND окна `Window` → `Window` | — | только своя таблица; внутренняя `m_handleToDesktopWindowMap` закрыта |

Тот же ответ дали и в сообществе: [WindowsAppSDK discussion #3463][d3463]
(«Get Back to the Xaml Window from AppWindow» — API нет, храните ссылку сами),
[Microsoft Q&A 1281508][qa].

## Что даёт `Window` и чем это заменяется на своём окне

| `Window` | На своём HWND |
|---|---|
| `Content` | `DesktopWindowXamlSource.Content` |
| `Activate()` | `ShowWindow`/`SetForegroundWindow` или `AppWindow.Show(true)` |
| `Close()`, `Closed` | `DestroyWindow`, `WM_CLOSE`; `AppWindow.Closing`/`Destroying` |
| `Activated` | `WM_ACTIVATE` или `InputActivationListener.GetForWindowId` (только top-level и только поток-владелец, [справка][ial]) |
| `SizeChanged`, `Bounds` | `WM_SIZE`, `GetClientRect`; `AppWindow.Changed` (`DidSizeChange`), `AppWindow.ClientSize`; для дерева — `XamlRoot.Changed` |
| `VisibilityChanged`, `Visible` | `WM_SIZE`/`SIZE_MINIMIZED`; `AppWindow.IsVisible`, `AppWindow.Changed` |
| `Title` | `SetWindowTextW` / `AppWindow.Title` |
| `SystemBackdrop` | `DesktopWindowXamlSource.SystemBackdrop` (в пробе уже так) |
| `ExtendsContentIntoTitleBar`, `SetTitleBar` | `AppWindow.TitleBar.ExtendsContentIntoTitleBar` + `InputNonClientPointerSource.SetRegionRects`; или контрол `TitleBar` (1.7+) — он сам находит окно через `XamlRoot.ContentIslandEnvironment.AppWindowId` ([TitleBar.cpp:912–942][titlebar]) и `Window` не требует |
| `AppWindow` | `AppWindow.GetFromWindowId(GetWindowIdFromWindow(hwnd))` |
| `Compositor`, `DispatcherQueue` | компоновщик острова / `DispatcherQueue.GetForCurrentThread()` |
| `Width`/`Height`/`Min*`/`Max*` (экспериментальные) | `OverlappedPresenter.PreferredMinimumWidth` и соседи (1.7+) |
| `Window.Current`, `CoreWindow`, `Dispatcher` | и у `Window` в десктопе всегда `null` |

## Что теряется без объекта `Window`

Внутри WinUI элементы ищут «свой» `Window` через
`DXamlCore::GetAssociatedWindowNoRef` ([DXamlCore.cpp:295–381][assoc]): от
`XamlRoot` берётся HWND острова, от него — родитель, и родитель ищется в той
самой закрытой таблице. Для нашего окна поиск честно возвращает `nullptr`, и
вызывающие это переносят. Все места в `dxaml/` и `controls/` (по `main`):

- `Popup` — для островов подписка на `Window.Activated` не нужна вовсе,
  закрытие по потере фокуса делает путь острова ([Popup_Partial.cpp:775–783][popup]);
- `DatePicker`, `TimePicker`, `ApplicationBarService` — подписка на
  `Window.Activated` просто не создаётся;
- `Page`/`ScrollContentPresenter` — учёт границ окна и высоты нижнего AppBar;
- `ListViewBase` — старый путь перетаскивания (только когда не включён
  `CoreDragDrop`);
- `Application.Exit()` закрывает все окна из этой таблицы, а если их нет —
  просто выходит из цикла ([FrameworkApplication_Partial.cpp:945–960][exit]):
  своё окно при таком выходе закрывать самим. Так же и выход из цикла по
  закрытию последнего окна: с 1.5 для островов он по умолчанию выключен
  (`Application.DispatcherShutdownMode`, [release notes 1.5][rn15]).

Для Буквицы существенно разве что последнее, и проба уже выходит из цикла
сама — `EnqueueEventLoopExit` по `WM_DESTROY`. В комментарии к
`GetAssociatedWindowNoRef` команда
WinUI пишет, что хочет уйти от этой зависимости от HWND совсем («FUTURE:
Change m_handleToDesktopWindowMap to just be a list of Window objects…»).

## Варианты «своё окно + XAML», от надёжного к экспериментальному

**А. Свой HWND + `DesktopWindowXamlSource` (+ `AppWindow` по надобности).**
Официальный путь, им же пользуется сам `Window`. Это то, что уже сделано в
`CompositionWindow` и `IslandProbe`. Цена — дочерний HWND моста
(`DesktopChildSiteBridge`), который двигается отдельно от рамки.

**Б. Свой HWND + `XamlIsland` + `DesktopChildSiteBridge.Create`.** То же,
что А, только руками (так DWXS и устроен, см. [windowless-xaml-islands.md][windowless]);
даёт прямой доступ к `ContentIsland`. Ограничение из справки: `WebView2` в
`XamlIsland` не поддерживается, для него нужен DWXS ([XamlIsland][xamlisland]).

**В. Свой HWND + `XamlIsland` + `DesktopAttachedSiteBridge.CreateFromWindowId`
(1.7+).** Мост *прикрепляется к существующему окну, не создавая дочернего*
([release notes 1.7][rn17]) — XAML рисуется прямо на нашем top-level HWND.
Ближе всего к «подать своё окно», но:
- документирован как мост для `ContentIsland` на **системных**
  (`Windows.UI.Composition`) визуалах;
- в исходниках WinUI нет ни одного его использования или теста;
- [microsoft-ui-xaml#10455][i10455] (открыт с марта 2025): с таким мостом
  флайауты роняют приложение, потому что `DesktopAttachedSiteBridge` не
  наследует `DesktopSiteBridge`, а часть кода на это рассчитывает.
Для Буквицы с её ящиками и сносками — пока нет; годится как проба.

**Г. Системный `ContentIsland` + `DesktopAttachedSiteBridge` +
`ChildSiteLink.CreateForSystemVisual` + `XamlIsland`.** Сцена целиком на
системном компоновщике (как задник `CompositionWindow`), XAML — вложенный
остров без единого дочернего HWND. Именно этот сценарий 1.7 описывает как
«постепенный переход на WinUI поверх Windows.UI.Composition» ([release notes
1.7][rn17]). Но в справке `ChildSiteLink` есть оговорка о несовместимости
островов на разных типах `Visual`, сформулированная двусмысленно
([ChildSiteLink][csl]) — проверять только пробой.

**Д. Хаки над штатным `Window`** — не рекомендуются, перечислены, чтобы не
возвращаться к ним:

- *Сабкласс его HWND* через `SetWindowSubclass` — технически можно (так делает
  и AppWindow), но `GWLP_USERDATA` занят WinUI, а подмена `GWLP_WNDPROC` его
  ломает.
- *`SetClassLongPtr(GCLP_HBRBACKGROUND, nullptr)`* — уберёт белую кисть у
  класса (для всех окон процесса), но не поверхность перенаправления.
- *Хук `WH_CBT` на `HCBT_CREATEWND`* для класса `WinUIDesktopWin32WindowClass`
  с правкой `CREATESTRUCT` (например, добавить `WS_EX_NOREDIRECTIONBITMAP`) —
  теоретически возможно, не проверено, WinUI этого не ожидает.
- *Заранее зарегистрировать класс `WinUIDesktopWin32WindowClass` самим* —
  WinUI регистрирует класс, только если его ещё нет
  ([DesktopWindowImpl.cpp:1695][create]), но тогда окно получит нашу оконную
  процедуру вместо `BaseWindow::WndProc`, и `Window` не инициализируется.
- *`SetParent` окна `Window` в своё как `WS_CHILD`* — известные поломки:
  пропадают Mica/Acrylic, рушится композиция, едут координаты (разбор в
  [zencrop][zencrop]).

## Официальный статус и перспективы

- [microsoft-ui-xaml#10704][i10704] (открыт, август 2025): создавать
  top-level окно `Window` через `AppWindow`. Ответов команды нет.
- [microsoft-ui-xaml#10326][i10326] (закрыт): «почему `Window` не использует
  окно `AppWindow`» — без объяснения.
- [microsoft-ui-xaml#10050][i10050] (открыт): «ThousandIslands» — окно WinUI
  с промежуточным Win32-окном для чужих дочерних HWND.
- Направление команды — `XamlIsland` вместо DWXS и меньше зависимости XAML
  от HWND ([windowless-xaml-islands.md][windowless]). Конструктора
  `Window(HWND)` или `Window(AppWindow)` в планах не видно.

## Что из этого следует для Буквицы

- Держаться варианта А: своё окно + DWXS — это и есть `Window`, собранный
  правильно. Свой класс окна с `WS_EX_NOREDIRECTIONBITMAP` и без кисти штатным
  `Window` недостижим никаким публичным путём.
- Для оконных возможностей, которые хочется «как у `Window`»
  (полный экран, заголовок, место окна), брать
  `AppWindow.GetFromWindowId(hwnd)`; активацию — `WM_ACTIVATE` или
  `InputActivationListener`.
- Связь HWND ↔ наша обёртка держать у себя (своя таблица или `SetPropW`), но
  не через `GWLP_USERDATA` чужих окон.
- Если захочется убрать дочерний HWND моста (и его отставание при растяжке),
  пробовать В и Г в этой песочнице — оба пока экспериментальны.

[src]: https://github.com/microsoft/microsoft-ui-xaml/tree/8139a874afdc5a452fad71de3e2e53ab530ab8a1
[ctor]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/DesktopWindowImpl.cpp#L61-L88
[create]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/DesktopWindowImpl.cpp#L1685-L1723
[oncreate]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/DesktopWindowImpl.cpp#L90-L155
[appwindow]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/DesktopWindowImpl.cpp#L2088-L2101
[map]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/DXamlCore.h#L672
[assoc]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/DXamlCore.cpp#L295-L381
[basewindow]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/components/WindowChrome/inc/BaseWindow.h#L70-L90
[model]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/tools/XCPTypesAutoGen/XamlOM/Model/Microsoft.UI.Xaml.cs#L3513
[modelpriv]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/tools/XCPTypesAutoGen/XamlOM/Model/Microsoft.UI.Xaml.cs#L3303
[exit]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/FrameworkApplication_Partial.cpp#L945-L960
[rn15]: https://learn.microsoft.com/windows/apps/windows-app-sdk/release-notes-archive/stable-channel-1.5
[popup]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/dxaml/xcp/dxaml/lib/Popup_Partial.cpp#L775-L783
[titlebar]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/controls/dev/TitleBar/TitleBar.cpp#L912-L942
[windowless]: https://github.com/microsoft/microsoft-ui-xaml/blob/8139a874afdc5a452fad71de3e2e53ab530ab8a1/docs/design-notes/xaml-islands/windowless-xaml-islands.md
[manage]: https://learn.microsoft.com/windows/apps/develop/ui/manage-app-windows
[hwnd]: https://learn.microsoft.com/windows/apps/develop/ui/retrieve-hwnd
[ial]: https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.input.inputactivationlistener.getforwindowid
[xamlisland]: https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.xamlisland
[csl]: https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.content.childsitelink
[rn17]: https://learn.microsoft.com/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-1-7
[d3463]: https://github.com/microsoft/WindowsAppSDK/discussions/3463
[qa]: https://learn.microsoft.com/answers/questions/1281508/winui-3-any-way-to-retrieve-winrt-microsoft-ui-xam
[i10455]: https://github.com/microsoft/microsoft-ui-xaml/issues/10455
[i10704]: https://github.com/microsoft/microsoft-ui-xaml/issues/10704
[i10326]: https://github.com/microsoft/microsoft-ui-xaml/issues/10326
[i10050]: https://github.com/microsoft/microsoft-ui-xaml/issues/10050
[zencrop]: https://github.com/melody0709/zencrop/blob/master/doc/WinUI3_Reparenting_Fix_zh.md
