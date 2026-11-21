# macOS 平台配置（Apple Silicon / M2 起）
#
# 与 windows.cmake / linux.cmake 同级：本文件只产出变量，不碰 target。
# CMakeLists.txt 里 target 创建之后再按 APPLE 设 bundle 属性。

set(PLATFORM_SOURCES
        sys/macos/MacPlatform.cpp
        sys/macos/AutoRunMac.mm
        sys/macos/Gui.mm
)

# SMAppService（登录项）是 ObjC 类，桥接层必须是 ObjC++。
# Gui.mm 要碰 NSApplication / NSAppearance，同理。
# 只在 Apple 分支开这个 language：Windows 端 configure 时没有 ObjC 编译器，开了会直接失败。
enable_language(OBJCXX)
set_source_files_properties(sys/macos/AutoRunMac.mm PROPERTIES LANGUAGE OBJCXX)
set_source_files_properties(sys/macos/Gui.mm PROPERTIES LANGUAGE OBJCXX)

# ---------------------------------------------------------------------------
# 崩溃定位支持（P0-2）
#
# 现状：Release 构建默认 -O3 且不产符号，所以 M2 上那批
#   QThread::exec → activateSocketNotifiers → notifyInternal2 → 读 QThreadData
# 的 SIGBUS 只能看到地址，看不出 QThreadData 属于谁、哪个 QThread 已经析构。
#
# 这里做两件事：
#   1. 打开 dSYM（RelWithDebInfo / Release 都出 .dSYM bundle）——
#      CrashReporter 解析栈帧需要它做符号化，否则全是 ??:0；
#   2. 保留帧指针（-fno-omit-frame-pointer）+ 不内联关键小块（-gline-tables-only
#      不够，要完整 -g），让 lldb 里 bt 能落到具体行。
#
# 优化级别不动（-O3）：目标是"能定位"，不是"好调试"，不牺牲运行性能。
# 出包体积会变大（.app 里多一个 .dSYM 目录），只在非打包构建里默认开，
# 打 dmg 时 build_macos.sh 会把它移到 dist 目录单独保留。
# ---------------------------------------------------------------------------
if (NOT NKR_PACKAGE)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g -fno-omit-frame-pointer")
    set(CMAKE_OBJCXX_FLAGS_RELEASE "${CMAKE_OBJCXX_FLAGS_RELEASE} -g -fno-omit-frame-pointer")
    # dSYM 的 post-build 命令不能写在这里：本文件在 target 创建**之前**被 include，
    # add_custom_command(TARGET ...) 会报 "No TARGET 'rekobox'"。
    # 实际挂载点在 CMakeLists.txt 的 APPLE 分支末尾（见 NKR_MACOS_DSYM）。
    set(NKR_MACOS_ENABLE_DSYM ON)
endif ()

# AppKit / CoreFoundation 用于 ObjC 桥接（P5），Carbon 是 QHotkey 注册全局热键所需。
# CoreGraphics 是「从屏幕导入二维码」的权限探测（CGPreflightScreenCaptureAccess）所需：
# AppKit 虽然会间接带进来，但直接引符号必须显式链接，否则链接期报 undefined symbol。
# ServiceManagement 是登录项（SMAppService.mainAppService）所需。
# SystemConfiguration 是网络变化监听（SCDynamicStore + SetDispatchQueue）所需。
# 注意 CoreServices 并不会间接带进 SCDynamicStoreSetDispatchQueue 这个符号，
# 必须显式链 SystemConfiguration，否则链接期 undefined symbol。
set(PLATFORM_LIBRARIES
        "-framework AppKit"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreServices"
        "-framework Carbon"
        "-framework ServiceManagement"
        "-framework SystemConfiguration"
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
