# Проба «своё окно + attached-остров» — см. main.cpp. Черновик: собирается,
# чтобы проверяться, в приложение не входит. Устроен как IslandProbe.
find_package(cppwinrt CONFIG REQUIRED)

add_executable(AttachedProbe WIN32 main.cpp)

target_link_libraries(AttachedProbe PRIVATE
    Microsoft::CppWinRT
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib"
)

target_include_directories(AttachedProbe PRIVATE
    "${WXL_CPPWINRT_PROJECTION_DIR}"
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/include"
)

# Загрузчик ищет пакет платформы при старте и обязан лежать рядом — как у Reader.
add_custom_command(TARGET AttachedProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll"
            "$<TARGET_FILE_DIR:AttachedProbe>"
    COMMENT "Копирование загрузчика Windows App Runtime рядом с AttachedProbe")
