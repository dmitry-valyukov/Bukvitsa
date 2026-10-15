# Запускает приложение Буквицы, нажимает в нём клавиши и фотографирует окно.
#
# Зачем отдельный скрипт вместо «запусти и посмотри»: окна Буквицы созданы с
# WS_EX_NOREDIRECTIONBITMAP, всё содержимое рисует композитор DWM, и оно не
# попадает ни в GDI, ни в PrintWindow — даже с PW_RENDERFULLCONTENT кадр
# получается пустым. Единственный способ увидеть такое окно — снять область
# экрана, где оно лежит. Отсюда два следствия: окно поднимается на передний
# план перед каждым снимком, и поверх него ничего не должно лежать.
#
# Использование:
#   tools\drive.ps1 -Do "wait 1500; shot start.png"
#   tools\drive.ps1 -Exe build\x64\Reader\Debug\Reader.exe `
#                   -Arguments "FB3\testdata\nightmare_example.fb3" `
#   tools\drive.ps1 -DataDir M:\Temp\reader-data -Do "wait 2500; shot start.png"
#                     данные читалки — в подопытном каталоге, а не в
#                     %LOCALAPPDATA%: туда можно класть битые файлы
#   tools\drive.ps1 -ErrFile reader.err -Do "wait 2500; shot start.png"
#                     stderr приложения — в файл, относительный путь — от
#                     -ShotDir: отчёты CRT в Debug, сторож кадров на выходе; в
#                     конце прогона — строка end: код возврата и размер файла.
#                     С -Keep не сочетается (почему — у параметра)
#
# Команды (-Do — одна строка, команды через ';'):
#   wait <мс>         пауза
#   shot [файл]       PNG окна (по умолчанию shot.png, каталог задаёт -ShotDir)
#   key <клавиши>     SendKeys: {PGDN} {PGUP} {HOME} {END} {ESC} ^{ADD} и т.п.
#   press <клавиши>   то же, но без поднятия окна и без паузы после: для пачек
#                     нажатий, где счёт идёт на десятки миллисекунд (листание
#                     внахлёст) — окно уже поднято предыдущей командой
#   type <текст>      набор текста в фокус
#   click <x> <y>     щелчок в точке от левого верхнего угла окна, в пикселях
#   rclick <x> <y>    то же правой кнопкой
#   size              прямоугольник окна: x;y;ширина;высота
#   resize <ш> <в>    задать окну размер (из максимизации выводит); снимок сразу
#                     за ним ловит просвет растяжки — то, что нарисовано до
#                     того, как остров XAML успел переверстаться
#   title             заголовок окна
#
# Команды быстрых событий — для того, что длится десятки миллисекунд (досчёт
# главы, летящий лист): после них драйвер не ждёт, а штатный снимок с его
# подъёмом окна идёт ~200 мс и такое пропускает. cwheelat, closenow, burst и
# hold окна не поднимают и идут после click/shot/raise или другой команды,
# поднявшей окно; cwheelat и hold нужен ещё и передний план у Reader. Без
# этого они останавливают прогон.
#   raise             поднять окно поверх всех, как перед снимком, — и только
#   stamp [метка]     напечатать время с миллисекундами — отметка в выводе
#   clicknow <x> <y>  щелчок без паузы после: указатель подводится заранее
#                     (WinUI нужно движение), нажатие — и сразу следующая команда
#   cwheelat <x> <y> <деления>
#                     Ctrl+колесо в точке: указатель, Ctrl через keybd_event,
#                     деления (отрицательные — к себе), Ctrl отпускается; без
#                     пауз после. SendKeys тут не годится: Ctrl у колеса
#                     читается из состояния клавиатуры, а не из нажатия
#   closenow <x> <y>  щелчок по точке заголовка (крестик) одним махом и
#                     ожидание выхода: время от щелчка, код возврата; не
#                     вышло за 20 с — зависание на выходе, прогон падает
#   exitwait          ожидание выхода, когда приложение закрывается само; тот
#                     же предел 20 с
#   burst <файл> <кадров> <мс> [x y ш в]
#                     серия снимков полосы окна подряд, через <мс> (0 — так
#                     часто, как отдаёт экран, ~16 мс); по умолчанию — строка
#                     колонцифры внизу по центру; область — все четыре числа
#                     или ни одного. Повторяющиеся подряд кадры выбрасываются,
#                     оставшиеся ложатся в один PNG столбиком с метками +мс от
#                     начала серии
#   hold <клавиша> <мс>
#                     зажать клавишу: нажатие, через 500 мс автоповтор с
#                     периодом 33 мс в среднем (отдельный повтор опаздывает
#                     до шага системного таймера, ~16 мс), отпускание по
#                     истечении <мс>; в итоге — число повторов и их средний
#                     период. Клавиша — pgdn, pgup, left, right, up, down,
#                     space, home, end или код VK с 0x (0x22)
#   mem <мс> <период> <файл>
#                     память процесса каждые <период> мс в течение <мс>:
#                     CSV (мс, Private, WorkingSet в байтах) и итог min/max;
#                     приложение вышло посреди замера — собранное записано,
#                     прогон падает

[CmdletBinding()]
param(
    [string]$Exe = 'build\x64\Reader\Debug\Reader.exe',
    [string]$Arguments = '',
    [string]$DataDir = '',
    [string]$Do = 'wait 1500; shot shot.png',
    [string]$ShotDir = '.',
    [switch]$Keep,
    [int]$LaunchTimeoutSec = 30,

    # stderr приложения — в этот файл (относительный путь — от ShotDir). Туда
    # же идут отчёты CRT в Debug, в том числе сторож кадров на выходе: пустой
    # файл — выход без жалоб. В конце прогона печатаются код возврата и размер
    # файла. С -Keep не сочетается: с перенаправлением Start-Process запускает
    # приложение сам, через CreateProcess с наследованием дескрипторов, и
    # Reader получает stdout драйвера; вызывающий, читающий вывод драйвера из
    # канала, не увидит его конца, пока Reader жив, а с -Keep тот переживает
    # драйвер.
    [string]$ErrFile = ''
)

$ErrorActionPreference = 'Stop'
if ($Keep -and $ErrFile) {
    throw '-Keep и -ErrFile вместе нельзя: оставленный Reader держал бы открытым stdout драйвера.'
}
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int index);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Exe))     { $Exe = Join-Path $root $Exe }
if (-not [System.IO.Path]::IsPathRooted($ShotDir)) { $ShotDir = Join-Path $root $ShotDir }
if (-not (Test-Path $Exe)) { throw "Не найден исполняемый файл: $Exe" }
New-Item -ItemType Directory -Force $ShotDir | Out-Null

$startArgs = @{ FilePath = $Exe; PassThru = $true }
if ($Arguments) {
    # Путь к книге может быть относительным — приложение запускается из корня дерева.
    $startArgs.ArgumentList = $Arguments
    $startArgs.WorkingDirectory = $root
}
# Подопытные данные: приложение берёт каталог из BUKVITSA_DATA. Переменную
# видит дочерний процесс, выставляем её только на время запуска и возвращаем
# как было — иначе она пережила бы скрипт в сеансе PowerShell, и следующий
# запуск без -DataDir молча шёл бы на тех же подопытных данных.
$dataWas = $env:BUKVITSA_DATA
if ($DataDir) {
    if (-not [System.IO.Path]::IsPathRooted($DataDir)) { $DataDir = Join-Path $root $DataDir }
    New-Item -ItemType Directory -Force $DataDir | Out-Null
    $env:BUKVITSA_DATA = $DataDir
}
if ($ErrFile) {
    if (-not [System.IO.Path]::IsPathRooted($ErrFile)) { $ErrFile = Join-Path $ShotDir $ErrFile }
    $startArgs.RedirectStandardError = $ErrFile
}
try {
    $process = Start-Process @startArgs
    # Нужно в Windows PowerShell 5.1: без раннего обращения к Handle процесс,
    # запущенный Start-Process, после выхода отдаёт пустой ExitCode — .NET не
    # успевает взять его дескриптор. pwsh 7 берёт дескриптор в самом
    # Start-Process, там обращение безвредно.
    $null = $process.Handle
}
finally { if ($DataDir) { $env:BUKVITSA_DATA = $dataWas } }

# MainWindowHandle появляется не сразу и, что важнее, появляется раньше, чем
# окно готово показать содержимое: композитору нужен первый кадр.
$deadline = [DateTime]::UtcNow.AddSeconds($LaunchTimeoutSec)
while ($process.MainWindowHandle -eq [IntPtr]::Zero) {
    if ($process.HasExited) { throw "Приложение завершилось до появления окна (код $($process.ExitCode))." }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Окно не появилось за отведённое время.' }
    Start-Sleep -Milliseconds 100
    $process.Refresh()
}
$hwnd = $process.MainWindowHandle

# Приложение, которого не стало, выглядит для остальных команд как окно
# нулевого размера -- и жаловались они именно на размер, пряча настоящую
# новость. Спрашиваем сам процесс: код возврата разом отвечает, вышло оно само
# (0 или свой код) или упало (0xC0000005 и подобные).
function Assert-Alive {
    $process.Refresh()
    if (-not $process.HasExited) { return }

    $code = $process.ExitCode
    throw ("Приложение завершилось: код {0} (0x{1:X8})." -f $code, $code)
}

function Get-WindowRect {
    Assert-Alive
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($hwnd, [ref]$r)
    return @{ X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

# Окно, которое сейчас за приложение отвечает: само окно или модальный диалог
# поверх него. Пока диалог открыт, главное окно ввод не принимает -- поднимать
# и слать клавиши надо диалогу, иначе набранное уходит в никуда (проверено на
# диалоге открытия книги).
function Get-ActiveWindow {
    $popup = [Win]::GetLastActivePopup($hwnd)
    if ($popup -ne [IntPtr]::Zero -and $popup -ne $hwnd -and [Win]::IsWindowVisible($popup)) {
        return $popup
    }
    return $hwnd
}

function Move-Pointer([int]$x, [int]$y) {
    # MOVE|ABSOLUTE, а не SetCursorPos, и по той же причине, что в перетаскивании
    # ниже: SetCursorPos переставляет курсор, но событий указателя для
    # современного стека ввода не рождает. WinUI такого перемещения не видит,
    # а не увидев его — не считает последующее нажатие своим: кнопка не
    # подсвечивается и Click не приходит.
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $ax = [uint32]([double]$x * 65535 / ($bounds.Width - 1))
    $ay = [uint32]([double]$y * 65535 / ($bounds.Height - 1))
    [Win]::mouse_event(0x8001, $ax, $ay, 0, [UIntPtr]::Zero)
}

function Raise-Window {
    [void][Win]::ShowWindow($hwnd, 5)   # SW_SHOW

    # SetForegroundWindow не всесилен: Windows не отдаёт передний план окну
    # чужого процесса, если пользователь в это время работает в своём. А снимок
    # берётся с экрана, и чужое окно поверх испортит его молча. Поэтому окно
    # объявляется поверх всех — этому запрет не мешает — и возвращается обратно
    # в конце прогона (Lower-Window), чтобы не мешать самому пользователю.
    [void][Win]::SetWindowPos($hwnd, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0013)  # HWND_TOPMOST
    [void][Win]::SetForegroundWindow((Get-ActiveWindow))
    Start-Sleep -Milliseconds 150
}

function Lower-Window {
    [void][Win]::SetWindowPos($hwnd, [IntPtr]::new(-2), 0, 0, 0, 0, 0x0013)  # HWND_NOTOPMOST
}

# Быстрые команды окна не поднимают — подъём стоит 150 мс — и полагаются на
# HWND_TOPMOST, оставленный прежним Raise-Window: без него снимок молча снял
# бы чужое окно, а щелчок ушёл бы ему. Клавиатура идёт не верхнему окну, а
# переднему плану, поэтому нажатиям нужен ещё и передний план у Reader (у его
# окна или диалога) — иначе клавиши ушли бы туда, где работает пользователь.
function Assert-Raised([string]$what, [switch]$Keyboard) {
    Assert-Alive
    if (([Win]::GetWindowLongW($hwnd, -20) -band 0x8) -eq 0) {   # GWL_EXSTYLE, WS_EX_TOPMOST
        throw "${what}: окно не поднято — перед быстрыми командами нужен click, shot или raise."
    }
    if ($Keyboard) {
        $owner = [uint32]0
        [void][Win]::GetWindowThreadProcessId([Win]::GetForegroundWindow(), [ref]$owner)
        if ($owner -ne $process.Id) { throw "${what}: передний план не у Reader — нажатия ушли бы чужому окну." }
    }
}

function Save-Shot([string]$file) {
    Raise-Window
    $rect = Get-WindowRect
    if ($rect.W -le 0 -or $rect.H -le 0) { throw 'У окна нулевой размер.' }

    $bitmap = New-Object System.Drawing.Bitmap $rect.W, $rect.H
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.X, $rect.Y, 0, 0, (New-Object System.Drawing.Size $rect.W, $rect.H))
    $graphics.Dispose()

    $path = Join-Path $ShotDir $file
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Output "shot: $path ($($rect.W)x$($rect.H))"
}

function Get-Stamp { [DateTime]::Now.ToString('HH:mm:ss.fff') }

# Выход ждётся по самому процессу: время — от вызова, код — его собственный.
# Не вышел за 20 с — это зависание на выходе, нарушенный инвариант, а не исход
# прогона: прогон падает.
function Wait-Exit([System.Diagnostics.Stopwatch]$watch, [string]$what) {
    $exited = $process.WaitForExit(20000)
    $watch.Stop()
    if (-not $exited) { throw "Процесс жив через 20 с ${what}: зависание на выходе." }
    $code = '{0} (0x{0:X8})' -f $process.ExitCode
    Write-Output "exit: $($watch.ElapsedMilliseconds) мс $what, код $code"
}

# Сон до срока по секундомеру. Start-Sleep спит шагами системного таймера
# (15,6 мс), и сон «33 мс» длится ~47; сон до срока, который сдвигается на
# период, опаздывает лишь до шага и опозданий не копит.
function Wait-Until([System.Diagnostics.Stopwatch]$watch, [long]$at) {
    $left = $at - $watch.ElapsedMilliseconds
    if ($left -gt 0) { Start-Sleep -Milliseconds $left }
}

# Аргументы команды — ровно в одном из допустимых количеств: недостающий не
# подменяется молча умолчанием, лишний не теряется.
function Split-Arguments([string]$what, [string]$text, [int[]]$counts) {
    $values = @(if ($text) { $text -split '\s+' })
    if ($values.Count -notin $counts) {
        throw "${what}: аргументов нужно $($counts -join ' или '), дано $($values.Count): '$text'."
    }
    return ,$values
}

function Save-Burst([string]$file, [int]$count, [int]$gap, [string[]]$area) {
    if ($count -lt 1) { throw "burst: кадров нужно не меньше 1, дано $count." }
    if ($gap -lt 0) { throw "burst: промежуток между кадрами отрицательный: $gap." }

    # Подъёма окна внутри серии нет: он стоит 150 мс, а серия ловит то, что
    # длится десятки. Окно должны были поднять предыдущие команды.
    Assert-Raised 'burst'
    $r = Get-WindowRect
    if ($area.Count -eq 4) {
        $x = $r.X + [int]$area[0]; $y = $r.Y + [int]$area[1]; $w = [int]$area[2]; $h = [int]$area[3]
    } else {
        # Умолчание — строка колонцифры при масштабе 100 %: кегль 12 по центру
        # нижнего поля в 50 DIP, полоса 30 пикселей с верхом в 46 от низа окна
        # берёт её с запасом, 560 — шире самой длинной колонцифры.
        $w = [Math]::Min(560, $r.W); $h = 30
        $x = $r.X + [int](($r.W - $w) / 2); $y = $r.Y + $r.H - 46
    }

    $frames = New-Object System.Collections.Generic.List[object]
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $started = Get-Stamp
    for ($k = 0; $k -lt $count; $k++) {
        $bitmap = New-Object System.Drawing.Bitmap $w, $h
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
        $graphics.Dispose()
        $frames.Add(@($watch.ElapsedMilliseconds, $bitmap))
        if ($gap -gt 0) { Start-Sleep -Milliseconds $gap }
    }
    $span = $watch.ElapsedMilliseconds

    # Кадр, повторяющий предыдущий, ничего не сообщает: остаются смены.
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $kept = New-Object System.Collections.Generic.List[object]
    $previous = ''
    foreach ($frame in $frames) {
        $stream = New-Object System.IO.MemoryStream
        $frame[1].Save($stream, [System.Drawing.Imaging.ImageFormat]::Bmp)
        $key = [Convert]::ToBase64String($md5.ComputeHash($stream.ToArray()))
        $stream.Dispose()
        if ($key -ne $previous) { $kept.Add($frame) } else { $frame[1].Dispose() }
        $previous = $key
    }
    $md5.Dispose()

    $label = 60
    $sheet = New-Object System.Drawing.Bitmap ($w + $label), ($h * $kept.Count)
    $graphics = [System.Drawing.Graphics]::FromImage($sheet)
    $graphics.Clear([System.Drawing.Color]::White)
    $font = New-Object System.Drawing.Font 'Consolas', 9
    for ($k = 0; $k -lt $kept.Count; $k++) {
        $graphics.DrawImage($kept[$k][1], $label, $k * $h)
        $graphics.DrawString("+$($kept[$k][0])", $font, [System.Drawing.Brushes]::Red, 1, ($k * $h + [Math]::Max(0, ($h - 14) / 2)))
        $kept[$k][1].Dispose()
    }
    $font.Dispose()
    $graphics.Dispose()
    $path = Join-Path $ShotDir $file
    $sheet.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    Write-Output "burst: $path с $started, $span мс, кадров $count, смен $($kept.Count - 1)"
}

function Get-VirtualKey([string]$name) {
    $keys = [ordered]@{ pgdn = 0x22; pgup = 0x21; left = 0x25; up = 0x26; right = 0x27; down = 0x28
                        space = 0x20; home = 0x24; end = 0x23 }
    $key = $name.ToLower()
    if ($keys.Contains($key)) { return [byte]$keys[$key] }
    # Код — только с 0x: без префикса «f2» прочлось бы как 0xF2, а «d» — как
    # 0x0D, Enter.
    if ($key -match '^0x([0-9a-f]{1,2})$') {
        $code = [Convert]::ToInt32($Matches[1], 16)
        if ($code -ne 0) { return [byte]$code }
    }
    throw "Клавиша '$name' не знакома: $($keys.Keys -join ', ') или код VK вида 0x22."
}

try {
    foreach ($command in ($Do -split ';')) {
        $parts = $command.Trim() -split '\s+', 2
        $name = $parts[0].ToLower()
        $rest = if ($parts.Count -gt 1) { $parts[1] } else { '' }

        switch ($name) {
            ''      { }
            'wait'  { Start-Sleep -Milliseconds ([int]$rest) }
            'shot'  { Save-Shot ($(if ($rest) { $rest } else { 'shot.png' })) }
            'title' {
                Assert-Alive
                $sb = New-Object System.Text.StringBuilder 512
                [void][Win]::GetWindowTextW($hwnd, $sb, $sb.Capacity)
                Write-Output "title: $($sb.ToString())"
            }
            'size'  { $r = Get-WindowRect; Write-Output "size: $($r.X);$($r.Y);$($r.W);$($r.H)" }
            'resize' {
                $wh = $rest -split '\s+'
                [void][Win]::ShowWindow($hwnd, 9)   # SW_RESTORE: максимизированное окно размера не примет
                # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE: только размер.
                [void][Win]::SetWindowPos($hwnd, [IntPtr]::Zero, 0, 0, [int]$wh[0], [int]$wh[1], 0x0016)
            }
            'key'   { Raise-Window; [System.Windows.Forms.SendKeys]::SendWait($rest); Start-Sleep -Milliseconds 120 }
            'press' { [System.Windows.Forms.SendKeys]::SendWait($rest) }
            'type'  { Raise-Window; [System.Windows.Forms.SendKeys]::SendWait($rest); Start-Sleep -Milliseconds 120 }
            'click' {
                Raise-Window
                $xy = $rest -split '\s+'
                $r = Get-WindowRect
                Move-Pointer ($r.X + [int]$xy[0]) ($r.Y + [int]$xy[1])
                Start-Sleep -Milliseconds 120
                [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
                Start-Sleep -Milliseconds 150
            }
            'drag' {
                # Перетаскивание: нажать в первой точке, доехать до второй,
                # отпустить. Дорога проходится шагами, потому что приложение
                # ведёт перетаскиваемое по событиям движения, а не по концам.
                # Движение — mouse_event MOVE|ABSOLUTE, а не SetCursorPos: тот
                # переставляет курсор, но событий указателя для современного
                # стека ввода не рождает, и WinUI перетаскивания не видит.
                Raise-Window
                $xy = $rest -split '\s+'
                $r = Get-WindowRect
                $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
                $x1 = $r.X + [int]$xy[0]; $y1 = $r.Y + [int]$xy[1]
                $x2 = $r.X + [int]$xy[2]; $y2 = $r.Y + [int]$xy[3]
                $move = {
                    param([double]$x, [double]$y)
                    $ax = [uint32]($x * 65535 / ($bounds.Width - 1))
                    $ay = [uint32]($y * 65535 / ($bounds.Height - 1))
                    [Win]::mouse_event(0x8001, $ax, $ay, 0, [UIntPtr]::Zero)   # MOVE|ABSOLUTE
                }
                & $move $x1 $y1
                Start-Sleep -Milliseconds 80
                [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                Start-Sleep -Milliseconds 80
                $steps = 16
                for ($i = 1; $i -le $steps; $i++) {
                    & $move ($x1 + ($x2 - $x1) * $i / $steps) ($y1 + ($y2 - $y1) * $i / $steps)
                    Start-Sleep -Milliseconds 25
                }
                [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
                Start-Sleep -Milliseconds 150
            }
            'rclick' {
                # Правая кнопка — не роскошь драйвера: ею читалка открывает
                # свой ящик, и проверить это иначе нечем.
                Raise-Window
                $xy = $rest -split '\s+'
                $r = Get-WindowRect
                Move-Pointer ($r.X + [int]$xy[0]) ($r.Y + [int]$xy[1])
                Start-Sleep -Milliseconds 120
                [Win]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)   # RIGHTDOWN
                [Win]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)   # RIGHTUP
                Start-Sleep -Milliseconds 150
            }
            'raise' { Raise-Window }
            'stamp' { Write-Output $(if ($rest) { "stamp $rest $(Get-Stamp)" } else { "stamp $(Get-Stamp)" }) }
            'clicknow' {
                $xy = Split-Arguments 'clicknow' $rest 2
                Raise-Window
                $r = Get-WindowRect
                Move-Pointer ($r.X + [int]$xy[0]) ($r.Y + [int]$xy[1])
                Start-Sleep -Milliseconds 120
                Write-Output "clicknow $(Get-Stamp)"
                [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
            }
            'cwheelat' {
                # Аргументы разбираются до всякого ввода: команда с ошибкой в
                # них не шлёт ничего.
                $wheelArgs = Split-Arguments 'cwheelat' $rest 3
                $pointX = [int]$wheelArgs[0]; $pointY = [int]$wheelArgs[1]; $notches = [int]$wheelArgs[2]
                if ($notches -eq 0) { throw 'cwheelat: делений 0 — колесу нечего делать.' }
                $delta = if ($notches -lt 0) { [uint32]::MaxValue - 119 } else { [uint32]120 }   # WHEEL_DELTA со знаком
                Assert-Raised 'cwheelat' -Keyboard
                $r = Get-WindowRect
                Move-Pointer ($r.X + $pointX) ($r.Y + $pointY)
                Write-Output "cwheelat $(Get-Stamp)"
                # Ctrl держится 15 мс до и после колеса: без запаса деление
                # иногда приходит раньше Ctrl и листает вместо кегля.
                # Отпускание — в finally: и Ctrl+C посреди не оставит Ctrl
                # нажатым в системе.
                try {
                    [Win]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)      # VK_CONTROL вниз
                    Start-Sleep -Milliseconds 15
                    for ($k = 0; $k -lt [Math]::Abs($notches); $k++) {
                        [Win]::mouse_event(0x0800, 0, 0, $delta, [UIntPtr]::Zero)   # WHEEL
                    }
                    Start-Sleep -Milliseconds 15
                }
                finally {
                    [Win]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)      # VK_CONTROL вверх
                }
            }
            'closenow' {
                # Без подъёма окна и с короткой паузой между движением и
                # нажатием (15 мс, а не 120, как у click): кнопки заголовка
                # рисует Windows, долгого наведения, как контролам WinUI, им
                # не нужно.
                $xy = Split-Arguments 'closenow' $rest 2
                Assert-Raised 'closenow'
                $r = Get-WindowRect
                Move-Pointer ($r.X + [int]$xy[0]) ($r.Y + [int]$xy[1])
                Start-Sleep -Milliseconds 15
                Write-Output "closenow $(Get-Stamp)"
                $watch = [System.Diagnostics.Stopwatch]::StartNew()
                [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
                Wait-Exit $watch 'после щелчка'
            }
            'exitwait' {
                $null = Split-Arguments 'exitwait' $rest 0
                Wait-Exit ([System.Diagnostics.Stopwatch]::StartNew()) 'ожидания'
            }
            'burst' {
                $burstArgs = Split-Arguments 'burst' $rest 3, 7
                Save-Burst $burstArgs[0] ([int]$burstArgs[1]) ([int]$burstArgs[2]) ($burstArgs | Select-Object -Skip 3)
            }
            'hold' {
                # Своего автоповтора у впрыснутого нажатия нет — его делает
                # клавиатура. Повтор — то же нажатие без отпускания, как шлёт
                # драйвер: задержка 500 мс, затем ~30 в секунду. Сроки повторов
                # отсчитываются от нажатия через 33 мс (Wait-Until): отдельный
                # повтор опаздывает до шага системного таймера, но средний
                # период — 33 мс, а не ~47, как у Start-Sleep 33.
                $holdArgs = Split-Arguments 'hold' $rest 2
                $vk = Get-VirtualKey $holdArgs[0]
                $total = [int]$holdArgs[1]
                if ($total -lt 0) { throw "hold: время удержания отрицательное: $total." }
                $flags = if ($vk -in 0x21..0x28) { 1 } else { 0 }   # KEYEVENTF_EXTENDEDKEY у стрелок и листания
                Assert-Raised 'hold' -Keyboard
                $repeats = 0
                $firstRepeatAt = 0; $lastRepeatAt = 0
                Write-Output "hold $($holdArgs[0]) $(Get-Stamp)"
                $watch = [System.Diagnostics.Stopwatch]::StartNew()
                # Нажатие — уже внутри try: отпускание в finally идёт и при
                # Ctrl+C посреди удержания, иначе клавиша осталась бы нажатой
                # в системе.
                try {
                    [Win]::keybd_event($vk, 0, $flags, [UIntPtr]::Zero)
                    for ($due = [Math]::Min(500, $total); $due -lt $total; $due += 33) {
                        Wait-Until $watch $due
                        [Win]::keybd_event($vk, 0, $flags, [UIntPtr]::Zero)
                        $lastRepeatAt = $watch.ElapsedMilliseconds
                        if ($repeats -eq 0) { $firstRepeatAt = $lastRepeatAt }
                        $repeats++
                        # Отставание на целый период не догоняется пачкой
                        # нажатий подряд: клавиатура пропущенных повторов
                        # не шлёт.
                        if ($lastRepeatAt - $due -ge 33) { $due = $lastRepeatAt }
                    }
                    Wait-Until $watch $total
                }
                finally {
                    [Win]::keybd_event($vk, 0, $flags -bor 2, [UIntPtr]::Zero)   # KEYEVENTF_KEYUP
                }
                $meanPeriod = if ($repeats -ge 2) { ', средний период {0:N1} мс' -f (($lastRepeatAt - $firstRepeatAt) / ($repeats - 1)) } else { '' }
                Write-Output "hold: $($watch.ElapsedMilliseconds) мс, повторов $repeats$meanPeriod"
            }
            'mem' {
                $memArgs = Split-Arguments 'mem' $rest 3
                $total = [int]$memArgs[0]; $period = [int]$memArgs[1]
                if ($total -lt 0) { throw "mem: время замера отрицательное: $total." }
                if ($period -lt 1) { throw "mem: период нужен не меньше 1 мс, дано $period." }
                $path = Join-Path $ShotDir $memArgs[2]
                $lines = New-Object System.Collections.Generic.List[string]
                $lines.Add('ms,private,workingset')
                $samples = New-Object System.Collections.Generic.List[object]
                Assert-Alive
                $watch = [System.Diagnostics.Stopwatch]::StartNew()
                # Выход приложения посреди замера роняет прогон, но собранное
                # до него сперва ложится в файл: оно и показывает, как шла
                # память перед выходом. ASCII: в CSV только латиница, цифры и
                # запятые, а файл одинаков в Windows PowerShell 5.1 и pwsh 7,
                # без BOM.
                try {
                    while ($watch.ElapsedMilliseconds -le $total) {
                        Assert-Alive
                        $at = $watch.ElapsedMilliseconds
                        $lines.Add("$at,$($process.PrivateMemorySize64),$($process.WorkingSet64)")
                        $samples.Add(@($process.PrivateMemorySize64, $process.WorkingSet64))
                        Start-Sleep -Milliseconds $period
                    }
                }
                finally { Set-Content -Path $path -Value $lines -Encoding ascii }
                $mb = { param($v) '{0:N1}' -f ($v / 1MB) }
                $private = $samples | ForEach-Object { $_[0] } | Measure-Object -Minimum -Maximum
                $working = $samples | ForEach-Object { $_[1] } | Measure-Object -Minimum -Maximum
                Write-Output ('mem: {0}, замеров {1}; Private {2}–{3} МБ, WS {4}–{5} МБ' -f $path, $samples.Count,
                    (& $mb $private.Minimum), (& $mb $private.Maximum), (& $mb $working.Minimum), (& $mb $working.Maximum))
            }
            default { throw "Неизвестная команда: $name" }
        }
    }
}
finally {
    # Приложение могло закрыться само — «Выйти из читалки» именно это и
    # делает, — и тогда любому из вызовов ниже не с кем разговаривать. Это
    # не ошибка прогона.
    $killed = $false
    try {
        if (-not $process.HasExited) { Lower-Window }

        if (-not $Keep -and -not $process.HasExited) {
            # Сначала вежливо: у приложения есть что сохранить (место чтения).
            [void]$process.CloseMainWindow()
            if (-not $process.WaitForExit(3000)) {
                # Kill только начинает завершение: HasExited и ExitCode верны
                # лишь после ожидания.
                $process.Kill()
                $killed = $true
                $process.WaitForExit()
            }
        }
    } catch {}

    # Итог для прогонов со stderr: жалобы CRT и сторожа видны размером файла.
    # Пропавший файл — не «0 байт»: пустой значил бы выход без жалоб.
    if ($ErrFile) {
        $stderr = if (Test-Path -LiteralPath $ErrFile) { 'stderr {0} байт' -f (Get-Item -LiteralPath $ErrFile).Length } else { 'stderr: файла нет' }
        if ($process.HasExited) {
            $how = if ($killed) { ', убит драйвером через 3 с' } else { '' }
            Write-Output ('end: код {0} (0x{0:X8}){1}, {2}' -f $process.ExitCode, $how, $stderr)
        } else {
            # Жив он здесь, только если закрыть его не вышло: -Keep с -ErrFile
            # запрещён с самого начала.
            Write-Output "end: процесс жив, $stderr"
        }
    }
}
