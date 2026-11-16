# Записывает в файл блокировки packages.lock.json хеш пакета WinXL, который
# лежит в локальном каталоге пакетов feed/.
#
# Пакет WinXL Буквица не скачивает, а собирает из дерева wxl сама: в CI —
# задания winxl.yml из wxl на коммите версии, которую называет файл
# блокировки, на машине — из своего дерева wxl. Сборка пакета не
# воспроизводится побайтно: две упаковки одного и того же каталога дают
# разные файлы, а NuGet сверяет хеш пакета с файлом блокировки при всяком
# восстановлении, не только в режиме блокировки (NU1403). Поэтому файл
# блокировки закрепляет версию WinXL и всё, от чего он зависит, с их хешами, а
# хеш самого WinXL — это хеш пакета, собранного здесь; скрипт записывает его
# после публикации и до настройки:
#
#   cmake -DWXL_PUBLISH_PACKAGE=<WinXL.<версия>.nupkg> -DWXL_PUBLISH_FEED=feed -P <wxl>\tools\package\publish.cmake
#   tools\winxl-lock.ps1
#   tools\build.ps1
#
# Скрипт останавливается, если в feed/ нет WinXL той версии, что записана в
# файле блокировки, и если глобальная папка пакетов NuGet уже держит эту
# версию другой сборки: восстановление взяло бы ту, а не опубликованную. В
# файле меняется одна строка — contentHash WinXL; остальное, в том числе
# версия, остаётся как было.

[CmdletBinding()]
param(
    # Локальный каталог пакетов, источник local из nuget.config.
    [string]$Feed = (Join-Path (Split-Path -Parent $PSScriptRoot) 'feed'),
    [string]$LockFile = (Join-Path (Split-Path -Parent $PSScriptRoot) 'packages.lock.json')
)

$ErrorActionPreference = 'Stop'

$text = [IO.File]::ReadAllText($LockFile)
$lock = $text | ConvertFrom-Json
$entries = @(foreach ($target in $lock.dependencies.PSObject.Properties) {
    foreach ($package in $target.Value.PSObject.Properties) {
        if ($package.Name -eq 'WinXL') { $package.Value }
    }
})
if ($entries.Count -ne 1) { throw "$LockFile записывает WinXL $($entries.Count) раз, а не один." }
$version = $entries[0].resolved
if ($version -notmatch '^[0-9]+(\.[0-9]+){2,3}$') { throw "$LockFile записывает WinXL в версии '$version'." }

$published = Join-Path $Feed "winxl/$version/winxl.$version.nupkg.sha512"
if (-not (Test-Path $published)) {
    throw "В $Feed нет WinXL $version, версии из $LockFile ($published). Опубликуйте пакет этой версии tools/package/publish.cmake дерева wxl."
}
$hash = (Get-Content $published -Raw).Trim()

# Глобальная папка пакетов, в которую восстановление распаковывает пакет:
# NUGET_PACKAGES, иначе папка по умолчанию (nuget.config Буквицы её не задаёт).
$global = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget/packages' }
$metadata = Join-Path $global "winxl/$version/.nupkg.metadata"
if (Test-Path $metadata) {
    $extracted = (Get-Content $metadata -Raw | ConvertFrom-Json).contentHash
    if ($extracted -ne $hash) {
        throw "Глобальная папка пакетов уже держит другую сборку WinXL $version ($(Split-Path -Parent $metadata)), и восстановление возьмёт её, а не пакет из $Feed. Удалите этот каталог."
    }
}

# Одна строка contentHash внутри записи WinXL; запись WinXL одна (проверено выше).
$pattern = '("WinXL"\s*:\s*\{[^{}]*?"contentHash"\s*:\s*")[^"]*(")'
$found = [regex]::Matches($text, $pattern)
if ($found.Count -ne 1) { throw "В $LockFile не найдена строка contentHash записи WinXL." }
$updated = [regex]::Replace($text, $pattern, { param($m) $m.Groups[1].Value + $hash + $m.Groups[2].Value })
if ($updated -ne $text) {
    [IO.File]::WriteAllText($LockFile, $updated, (New-Object Text.UTF8Encoding $false))
    "WinXL ${version}: в $LockFile записан хеш пакета из $Feed"
} else {
    "WinXL ${version}: хеш в $LockFile уже тот же, что у пакета из $Feed"
}
