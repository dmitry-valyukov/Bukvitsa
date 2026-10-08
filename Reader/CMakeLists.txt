# Reader — читалка на wxl.ui.
#
# Объявляется так же, как всякое приложение на wxl: `wxl_add_executable`
# делает исполняемый файл оконным (его точка входа wWinMain живёт в самой
# wxl), линкует wxl::ui, кладёт рядом с ним всё, что wxl нужно во время
# выполнения, — загрузчик Windows App Runtime, Win2D, WebView2Loader, — и
# выписывает манифест: масштаб по монитору окно держит само, Windows названа
# проверенной. Сверх этого читалке сказать в манифесте нечего, поэтому своего
# у неё нет — имя сборки в нём имя цели.
#
# Доступ к декларативной поверхности не должен стоить приложению разбора
# проекции cppwinrt, и отсутствие её include-пути здесь — то, что это
# гарантирует.

wxl_add_executable(Reader
    src/main.cpp
    src/start_screen.cpp
    src/book_view.cpp
    src/library_screen.cpp
    src/reader_panel.cpp
    src/note_popup.cpp
    src/notices.cpp
    src/skin_wizard.cpp
    src/file_dialog.cpp
    src/keys.cpp
    src/book_view.h
    src/file_dialog.h
    src/keys.h
    src/library_screen.h
    src/reader_panel.h
    src/note_popup.h
    src/notices.h
    src/skin_wizard.h
    src/start_screen.h
    src/pch.h
)

# Модель — bukvitsa::lib, и с ней приходит всё, что ниже: вёрстка с FB3,
# DirectWrite и Direct2D (страницу читалка рисует своими глифами, и рисует их
# та же вёрстка, что их разложила), операции с файлами и корутины wxl::async,
# windowscodecs. Своё у Reader одно: dxguid -- CLSID встроенных эффектов D2D
# (изгиб страницы на подложке).
target_link_libraries(Reader PRIVATE
    bukvitsa::lib
    dxguid
)

# wxl::xml разбирает settings.xml, а память берёт из STA-пула wxl::core --
# он строится в wxl_launched() и переживает последний разбор.
bukvitsa_consumes_wxl(Reader)

# Путь без схемы разрешается рядом с исполняемым файлом, поэтому ресурсы
# должны там лежать. Правилом на файл, а не хвостом линковки: правка картинки
# не повод перелинковывать, а хвост линковки без линковки и не сработал бы.
wxl_target_assets(Reader src/Assets)

# TODO: витрина хранилища, наблюдаемые каталоги, второй вариант листания.
# Мастер обложек сделан -- его план остался в doc/skin-wizard-plan.txt.
