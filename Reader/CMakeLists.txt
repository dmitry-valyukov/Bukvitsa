# Reader — читалка на wxl.ui.
#
# Объявляется так же, как всякое приложение на wxl: `wxl_add_executable`
# делает исполняемый файл оконным (его точка входа wWinMain живёт в самой
# wxl), линкует wxl::ui и кладёт рядом с ним всё, что wxl нужно во время
# выполнения, — загрузчик Windows App Runtime, Win2D, WebView2Loader. Манифест
# остаётся свой: он назван Bukvitsa.Reader, и wxl свой не выписывает.
#
# Доступ к декларативной поверхности не должен стоить приложению разбора
# проекции cppwinrt, и отсутствие её include-пути здесь — то, что это
# гарантирует.

wxl_add_executable(Reader
    src/main.cpp
    src/start_screen.cpp
    src/book.cpp
    src/book_view.cpp
    src/library_screen.cpp
    src/reader_panel.cpp
    src/note_popup.cpp
    src/book_index.cpp
    src/imaging.cpp
    src/io.cpp
    src/library.cpp
    src/settings.cpp
    src/skins.cpp
    src/skin_wizard.cpp
    src/store.cpp
    src/file_dialog.cpp
    src/book.h
    src/book_view.h
    src/file_dialog.h
    src/imaging.h
    src/io.h
    src/library.h
    src/library_screen.h
    src/reader_panel.h
    src/note_popup.h
    src/book_index.h
    src/settings.h
    src/skins.h
    src/skin_wizard.h
    src/start_screen.h
    src/store.h
    src/theme.h
    src/pch.h
    src/app.manifest
)

# bukvitsa::typography тянет за собой bukvitsa::fb3, DirectWrite и Direct2D:
# страницу читалка рисует своими глифами, и рисует их та же вёрстка, что их
# разложила. wxl::async — петля ввода-вывода и корутины (io.h), wxl::xml и
# wxl::fmt — файлы читалки. windowscodecs -- декодирование картинок книги
# (WIC), dxguid -- CLSID встроенных эффектов D2D (изгиб страницы на подложке).
target_link_libraries(Reader PRIVATE
    wxl::async
    wxl::xml
    wxl::core
    wxl::fmt
    bukvitsa::typography
    windowscodecs
    dxguid
)

# wxl::xml разбирает settings.xml, а память берёт из STA-пула wxl::core --
# он строится в wxl_launched() и переживает последний разбор.
bukvitsa_consumes_wxl(Reader)

# Путь без схемы разрешается рядом с исполняемым файлом, поэтому ресурсы
# должны там лежать. Правилом на файл, а не хвостом линковки: правка картинки
# не повод перелинковывать, а хвост линковки без линковки и не сработал бы.
wxl_target_assets(Reader src/Assets)

if(BUKVITSA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# TODO: витрина хранилища, наблюдаемые каталоги, второй вариант листания.
# Мастер обложек сделан -- его план остался в doc/skin-wizard-plan.txt.
