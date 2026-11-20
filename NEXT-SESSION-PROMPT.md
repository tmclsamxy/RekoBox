你是 RekoBox 项目的开发助手。当前工作区就是仓库根目录
（能看到 README.md、RekoboxForWindows/、RekoboxForAndroid/、RekoboxForMac/、docs/）。

任务：把 RekoBox 的 macOS 端（Apple Silicon / M2）从"骨架"推进到"能跑"。

先读这两个文件，不要跳：
1. RekoboxForMac/docs/HANDOFF.md   （现状、执行顺序、首次编译的错误表）
2. docs/06-macos-port.md           （完整方案：架构、TUN 权限、签名、风险）

当前状态：P0/P1 骨架已写完，但**一行都没编译过**（代码是在 Windows 上写的，
没有 Qt/macOS 工具链）。所以第一次编译一定会有错，这是预期内的，不是失败。

按这个顺序做：
  bash RekoboxForWindows/libs/build_deps_macos.sh    # zxing 2.0 / yaml 0.7 / protobuf 21.4
  bash RekoboxForWindows/libs/get_source_local.sh    # 拉 sing-box 内核源码
  bash RekoboxForWindows/libs/build_macos.sh         # 编 .app
撞错就查 HANDOFF §4 的表，修完重跑（脚本幂等）。目标是：
RekoboxForMac/build/RekoBox.app 能启动，界面与 RekoboxForMac/mockups/index.html 的
A1 图一致（M1 验收清单见 HANDOFF §3 Step 4）。

约束：
- 共享源码树：不要复制一份 RekoboxForMac 的 C++ 副本，mac 代码走 #ifdef Q_OS_MACOS /
  cmake/macos/ / sys/macos/
- 不许破坏 Windows 端（WIN32 分支、windows.cmake、.rc 资源）
- 改了共用文件（main.cpp、ConfigBuilder.cpp、mainwindow.cpp 这类）要在 HANDOFF 里记一笔
- 每修完一类编译错误，把"现象 → 原因 → 处理"补进 HANDOFF §4 的表
- 装软件（brew / aqtinstall / pip）或需要 sudo 之前先问我，别自己跑
