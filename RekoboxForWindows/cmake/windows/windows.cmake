set(PLATFORM_SOURCES 3rdparty/WinCommander.cpp sys/windows/guihelper.cpp sys/windows/MiniDump.cpp)
set(PLATFORM_LIBRARIES wininet wsock32 ws2_32 user32 rasapi32 iphlpapi)

include(cmake/windows/generate_product_version.cmake)
generate_product_version(
        QV2RAY_RC
        ICON "${CMAKE_SOURCE_DIR}/res/rekobox.ico"
        NAME "rekobox"
        BUNDLE "rekobox"
        COMPANY_NAME "rekobox"
        COMPANY_COPYRIGHT "rekobox"
        FILE_DESCRIPTION "rekobox"
        VERSION_MAJOR ${NKR_RC_MAJOR}
        VERSION_MINOR ${NKR_RC_MINOR}
        VERSION_PATCH ${NKR_RC_PATCH}
)
# 让资源对象依赖生成的 VersionInfo.h，否则改版本号后 ninja 不会重编 .rc，
# exe 属性里的版本会停留在上一次的值
set_source_files_properties(${Q2RAY_RC} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/VersionInfo.h")
add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX)
set(GUI_TYPE WIN32)
if (MINGW)
    if (NOT DEFINED MinGW_ROOT)
        set(MinGW_ROOT "C:/msys64/mingw64")
    endif ()
    # MinGW 需要显式声明目标平台，rasapi32 / iphlpapi 头文件依赖它
    add_definitions(-D_WIN32_WINNT=0x0600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS)
else ()
    add_compile_options("/utf-8")
    add_compile_options("/std:c++17")
    add_definitions(-D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS)
endif ()
