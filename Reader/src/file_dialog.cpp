#include "file_dialog.h"

#include <shobjidl.h>

#include <wrl/client.h>

#include "bukvitsa/typography/system_failure.h"

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

namespace {

/// Своё имя папки: диалог помнит по нему, где читатель был в прошлый раз, и не
/// путает читалку с другими приложениями. Одно на оба диалога -- книгу и
/// каталог с книгами ищут в одних и тех же местах.
const GUID kClientId = {
    0x4b1c2f60, 0x8d4a, 0x4b9e, {0x9c, 0x1a, 0x36, 0x0d, 0x2f, 0x77, 0x58, 0x11}};

/// Диалог открытия с тем, что у всех трёх общего: заголовок, своё имя папки и
/// только элементы файловой системы (FOS_FORCEFILESYSTEM) — у выбранного
/// всегда есть путь, и спрашивать его можно без запасного ответа. Отказ
/// здесь — отказ самого диалога, а не ответ читателя: остановка.
ComPtr<IFileOpenDialog> openDialog(const wchar_t* title, FILEOPENDIALOGOPTIONS extraOptions) {
    ComPtr<IFileOpenDialog> dialog;
    typography::stopIfFailed(
        ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)),
        "Диалог выбора: не создался");

    FILEOPENDIALOGOPTIONS options = 0;
    typography::stopIfFailed(dialog->GetOptions(&options), "Диалог выбора: не назвал свои флаги");
    typography::stopIfFailed(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | extraOptions),
                             "Диалог выбора: не принял флаги");
    typography::stopIfFailed(dialog->SetTitle(title), "Диалог выбора: не принял заголовок");
    typography::stopIfFailed(dialog->SetClientGuid(kClientId), "Диалог выбора: не принял имя папки");
    return dialog;
}

/// То общее, что есть у всех: показать диалог и достать из него путь.
std::filesystem::path showAndTake(IFileOpenDialog& dialog, HWND__* owner) {
    // Читатель отказался — Show отвечает ERROR_CANCELLED, и не выбрано ничего;
    // всякий другой отказ — отказ самого диалога.
    const HRESULT showResult = dialog.Show(owner);
    if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    typography::stopIfFailed(showResult, "Диалог выбора: не показался");

    ComPtr<IShellItem> item;
    typography::stopIfFailed(dialog.GetResult(&item), "Диалог выбора: не отдал выбранное");

    PWSTR text = nullptr;
    typography::stopIfFailed(item->GetDisplayName(SIGDN_FILESYSPATH, &text),
                             "Диалог выбора: у выбранного нет пути");

    std::filesystem::path path{text};
    ::CoTaskMemFree(text);
    return path;
}

}  // namespace

std::filesystem::path askForBook(HWND__* owner) {
    ComPtr<IFileOpenDialog> dialog = openDialog(L"Добавить книгу", 0);

    // Два фильтра, а не один: «все файлы» нужен затем, что книга с чужим
    // расширением встречается, а объяснять читателю, почему его файла не
    // видно, — плохой разговор.
    static const COMDLG_FILTERSPEC kFilters[] = {
        {L"Книги FB3", L"*.fb3"},
        {L"Все файлы", L"*.*"},
    };
    typography::stopIfFailed(
        dialog->SetFileTypes(static_cast<UINT>(sizeof(kFilters) / sizeof(kFilters[0])), kFilters),
        "Диалог книги: не принял фильтры");

    return showAndTake(*dialog.Get(), owner);
}

std::filesystem::path askForImage(HWND__* owner) {
    ComPtr<IFileOpenDialog> dialog = openDialog(L"Снимок для обложки", 0);

    // Форматы — те, что раскодирует WIC без расширений: чем открыть снимок,
    // решает imaging.cpp, а фильтр лишь помогает его найти.
    static const COMDLG_FILTERSPEC kFilters[] = {
        {L"Изображения", L"*.png;*.jpg;*.jpeg;*.bmp"},
        {L"Все файлы", L"*.*"},
    };
    typography::stopIfFailed(
        dialog->SetFileTypes(static_cast<UINT>(sizeof(kFilters) / sizeof(kFilters[0])), kFilters),
        "Диалог снимка: не принял фильтры");

    return showAndTake(*dialog.Get(), owner);
}

std::filesystem::path askForFolder(HWND__* owner) {
    // Тот же диалог, что и для книги, с одним поднятым флагом: у Windows
    // выбор папки — это выбор файла, которому разрешили быть папкой.
    ComPtr<IFileOpenDialog> dialog = openDialog(L"Каталог с книгами", FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);

    return showAndTake(*dialog.Get(), owner);
}

}  // namespace bukvitsa::reader
