# Проба «догоняет ли XAML рамку при UpdateLayout в WM_SIZE» — см. main.cpp.
# Черновик: собирается, чтобы проверяться, в приложение не входит. Устроен как
# IslandProbe/AttachedProbe.
find_package(cppwinrt CONFIG REQUIRED)

add_executable(XamlLayoutProbe WIN32 main.cpp)

target_link_libraries(XamlLayoutProbe PRIVATE
    Microsoft::CppWinRT
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib"
)

target_include_directories(XamlLayoutProbe PRIVATE
    "${WXL_CPPWINRT_PROJECTION_DIR}"
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/include"
)

add_custom_command(TARGET XamlLayoutProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll"
            "$<TARGET_FILE_DIR:XamlLayoutProbe>"
    COMMENT "Копирование загрузчика Windows App Runtime рядом с XamlLayoutProbe")
