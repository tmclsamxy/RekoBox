# macOS 平台配置（Apple Silicon / M2 起）
#
# 与 windows.cmake / linux.cmake 同级：本文件只产出变量，不碰 target。
# CMakeLists.txt 里 target 创建之后再按 APPLE 设 bundle 属性。

set(PLATFORM_SOURCES
        sys/macos/MacPlatform.cpp
)

# AppKit / CoreFoundation 用于 ObjC 桥接（P5），Carbon 是 QHotkey 注册全局热键所需
set(PLATFORM_LIBRARIES
        "-framework AppKit"
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework Carbon"
)

# 数据目录走 ~/Library/Application Support，而不是 bundle 内（bundle 只读）
add_definitions(-DNKR_CPP_USE_APPDATA)

# 先只出 arm64；要 universal 时传 -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
if (NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
endif ()

if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
endif ()

# 之后要引入 Objective-C++（菜单栏模板图、NSVisualEffectView 毛玻璃等）时：
#   enable_language(OBJCXX)
#   set_source_files_properties(sys/macos/Xxx.mm PROPERTIES LANGUAGE OBJCXX)
# 现在用不到，先不开，免得 Windows 端 configure 时找不到 ObjC 编译器。
