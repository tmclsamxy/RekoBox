# 交接文档 · RekoBox macOS（M2 / arm64）

> 写给下一个开发会话（也写给三个月后的自己）。
> 读完这一页就能接着干，不需要回看聊天记录。
>
> 最后更新：2026-11-20 · 代码在 Windows 上写、打包，开发机换到 MacBook Air M2

---

## 0. 一句话状态

**骨架已搭完，一行都没编译过。** 下一步是拿到 M2 上跑
`bash RekoboxForWindows/libs/build_macos.sh`，把第一次编译的错误清干净，看到窗口。

macOS 端不新建工程：直接给 `RekoboxForWindows/`（桌面共享树）加 `APPLE` 分支，
同一套 Qt 6.7 Widgets 源码编出 `.app`。

**你不在这台机器上写历史？** 项目是用 `git bundle` 打包带过来的（含完整提交历史，
不含构建产物）。克隆方式见 §0.5。

---

## 0.5 项目怎么到手、怎么开工

### 从 Windows 带过来的两个文件

| 文件 | 用途 |
|---|---|
| `Rekobox-mac-dev.bundle` | 完整 git 仓库（含本次所有未发布的改动），**推荐** |
| `Rekobox-mac-dev.zip` | 同一份内容的快照（无 `.git`），只解压就能看代码 |

放在 Mac 的 `~/Downloads` 后：

```bash
cd ~
git clone ~/Downloads/Rekobox-mac-dev.bundle Rekobox
cd Rekobox
git log --oneline -3        # 应看到 "feat(macos): 共享树接入 APPLE 分支 + 交接文档"
git status                  # 应该是 clean
```

纯快照版：

```bash
mkdir -p ~/Rekobox && cd ~/Rekobox && unzip ~/Downloads/Rekobox-mac-dev.zip
```

> bundle 里只有已提交的内容，所以**先在 Windows 侧 commit 再打包**——这一步已经做过，
> 不要指望 bundle 里有未提交的改动。

### 开工顺序

1. 装环境（§3 Step 0）
2. `bash RekoboxForWindows/libs/build_deps_macos.sh`
3. `bash RekoboxForWindows/libs/get_source_local.sh`
4. `bash RekoboxForWindows/libs/build_macos.sh`
5. 撞错 → 查 §4 的表 → 修 → 重跑

### 怎么让 AI 接着干

在项目根目录开一个新会话，把 `NEXT-SESSION-PROMPT.md` 的内容整段贴进去即可
（那份文件就是给这个场景写的，自包含、不依赖之前的聊天记录）。

---

## 1. 这次做完的事

| 文件 | 作用 | 验证程度 |
|---|---|---|
| `docs/06-macos-port.md` | 完整移植方案（564 行）：界面结论、分阶段计划、TUN 权限、签名公证、风险表 | 文档 |
| `RekoboxForMac/mockups/index.html` + 5 张 PNG | 界面效果图（直译版浅/深色、Mac 外壳、菜单栏面板、首选项） | 浏览器渲染通过 |
| `RekoboxForWindows/CMakeLists.txt` | 加 `elseif(APPLE)`、`CMAKE_OSX_ARCHITECTURES=arm64`、部署目标 13.0、bundle 属性 | **未编译** |
| `RekoboxForWindows/cmake/macos/macos.cmake` | mac 平台变量：`sys/macos/MacPlatform.cpp`、四个 framework、`NKR_CPP_USE_APPDATA` | **未编译** |
| `RekoboxForWindows/cmake/macos/Info.plist.in` | bundle 元数据（`io.rekobox.mac`、13.0、`LSUIElement=false`） | 未验证 |
| `RekoboxForWindows/sys/macos/MacPlatform.{hpp,cpp}` | 数据目录、Fusion 外观、shell/AppleScript 转义 | **未编译** |
| `RekoboxForWindows/main/main.cpp` | mac 下数据目录走 `AppDataLocation`；启动时 `ApplyLook()` | **未编译** |
| `RekoboxForWindows/db/ConfigBuilder.cpp` | mac 下选 `vpn-run-root-macos.sh` | **未编译** |
| `RekoboxForWindows/res/vpn/vpn-run-root-macos.sh` | mac 版 TUN 启动脚本（无 iptables），已进 qrc | 语法检查 |
| `RekoboxForWindows/ui/mainwindow.cpp` | osascript 命令做两层转义（路径含空格不会拼坏） | **未编译** |
| `RekoboxForWindows/libs/build_macos.sh` | 一键：图标 → 内核 → configure → 编译 → 装内核 → macdeployqt → ad-hoc 签名 → 可选 dmg | 语法检查，**未执行** |
| `RekoboxForWindows/libs/build_deps_macos.sh` | 源码编 zxing 2.0 / yaml 0.7 / protobuf 21.4（与 Windows 版本对齐） | 语法检查，**未执行** |
| `RekoboxForWindows/libs/make_icns.sh` | png → icns（只能在 mac 跑） | 语法检查，**未执行** |
| `README.md` / `RekoboxForMac/README.md` | 平台表、文档表、状态 | 文档 |

**"未编译"是字面意思**：写这份文档的机器是 Windows，没有 Qt/macOS 工具链，
所有 C++/CMake 改动只做过人工审查。第一次 `cmake` 一定有要修的地方，见 §4。

---

## 2. 已经核对过的事实（别再重复踩）

这些是读代码确认的，不是猜的：

1. **控制面走 TCP，不是 unix socket。** 主内核用 `-port <随机端口>` + `core_token`
   （`ui/mainwindow.cpp` 的 `MkPort()` / `setup_grpc()`）。所以 mac 上不存在
   "root 建的 socket 连不上"的问题——这也不需要在 P2 改。
2. **TUN 内核是一个独立的一次性进程，不提供 gRPC。** `WriteVPNSingBoxConfig()` 生成的
   `sing-box-vpn.json` 里没有 `experimental.clash_api`，只有 tun 入站 + 本地 socks 出站。
   root 权限只影响建 utun / 改路由。
3. **内核从 `go/cmd/rekobox_core` 构建**（它 import libneko + `sing-box/neko`），
   不是从 sing-box 树里直接 build。前置：`libs/get_source_local.sh`（拉 sing-box 到仓库根）
   + `libs/apply_neko_patch.sh`（overlay `go/singbox-neko/*.go` → `sing-box/neko`）。
4. **数据目录**：`NKR_CPP_USE_APPDATA` 下原来的兜底是 `AppConfigLocation`，
   在 mac 上是 `~/Library/Preferences`（放配置是错的）。已改成 `AppDataLocation`
   → `~/Library/Application Support/rekobox`。
5. **TUN 脚本要分平台**：原 `vpn-run-root.sh` 里是 `iptables`（Linux 专用），
   mac 版只 `exec` 内核；sing-box 用 `auto_route` 自己写路由表。
6. **`osascript` 原来拼串**：`"bash " + scriptPath`，路径有空格（比如放在 `My Apps/`）就会坏。
   已改成 shell 单引号 + AppleScript 转义。
7. **`IsAdmin()` 在 mac 上恒为 false**（`geteuid()==0`），但 `neko_set_spmode_vpn()` 里
   非 Windows 的提权分支只有 `#ifdef Q_OS_LINUX`，mac 会直接走到 `osascript` 启动，
   **不需要额外改**。
8. **图标**：`main.cpp` 已经设了 `QIcon::setFallbackSearchPaths({":/neko", ":/icon"})` 且
   `themeName` 为空时设成 `breeze`；工具栏用的是 `:/icon/b-*.svg`，mac 上应该能显示，
   但要在深色模式下确认 SVG 没有写死黑描边。
9. **翻译**：`main.cpp` 的 `loadTranslate()` 从 `applicationDirPath()/qtbase_zh_CN.qm` 加载，
   mac 的翻译在 `Contents/Resources/translations/`，会加载失败（界面仍是中文源串，
   但 Qt 自带对话框会是英文）。P2 修：改用 `QLibraryInfo::path(QLibraryInfo::TranslationsPath)`。

---

## 3. 在 Mac 上的执行顺序

### Step 0 · 环境（约 20 分钟，含下载）

```bash
xcode-select --install                     # 只需要 Command Line Tools
brew install cmake ninja go

python3 -m pip install --user aqtinstall
python3 -m aqt install-qt mac desktop 6.7.2 clang_64 -O ~/Qt \
        --archives qtbase qtsvg qttools
```

自检：

```bash
~/Qt/6.7.2/macos/bin/qmake -query QT_VERSION   # 6.7.2
go version                                      # darwin/arm64
cmake --version && ninja --version
```

> 为什么锁 6.7.2：Windows 版就是它，QSS 和控件行为要一致。Qt 的 mac 包是 universal。

### Step 1 · 依赖（10 分钟）

```bash
cd <repo>
bash RekoboxForWindows/libs/build_deps_macos.sh
```

产出 `RekoboxForWindows/libs/deps/built`。**不要图省事用 brew 的 protobuf**（已是 v2x/v3x，
带 abseil），和 Windows 端 21.4 不一致，生成代码行为可能不同。

`build_macos.sh` 会自动优先用这个目录，不需要手动传 `NKR_LIBS`。

### Step 2 · 内核（5 分钟，首次要拉源码）

```bash
bash RekoboxForWindows/libs/get_source_local.sh    # 拉 sing-box 到 <repo>/sing-box
bash RekoboxForWindows/libs/apply_neko_patch.sh    # overlay neko 包
```

`build_macos.sh` 会自动检查并调用 patch，所以也可以跳过第二条。

### Step 3 · 编译

```bash
bash RekoboxForWindows/libs/build_macos.sh
```

预期产物：`RekoboxForMac/build/RekoBox.app`，内核在 `Contents/MacOS/rekobox_core`。

第一次大概率不会一次过。**按 §4 的错误表逐条修**，修完重跑（脚本是幂等的）。

### Step 4 · 看窗口（P1 验收 M1）

```bash
open RekoboxForMac/build/RekoBox.app
```

对照 `RekoboxForMac/mockups/index.html` 的 A1 图，逐项看：

- [ ] 工具栏四个按钮（程序 / 首选项 / 服务器 / 更新）图标有显示，不是空白
- [ ] 界面是 Fusion 观感（按钮方正、下拉不是 mac 原生圆角），和 Windows 一致
- [ ] 顶部菜单栏在屏幕顶端（程序 / 首选项 / 服务器）
- [ ] 五列表格：类型 / 地址 / 名称 / 测试结果 / 流量，行高 30
- [ ] 关窗口后进程不退出，Dock 图标还在（`setQuitOnLastWindowClosed(false)` 已生效）
- [ ] 配置文件落在 `~/Library/Application Support/rekobox/config/`
- [ ] 日志里有 `grpc server listening`（说明主内核起来了）

### Step 5 · 系统代理（P2，不需要密码，先跑通这个）

```bash
networksetup -getwebproxy Wi-Fi      # 勾选"系统代理"后应变 Enabled: Yes / 127.0.0.1 / 2080
curl -x http://127.0.0.1:2080 https://ip.sb
```

取消勾选后必须是 `Enabled: No`。**强杀 App 再重开**也要自动清干净——
见 `06-macos-port.md` §6.2 的"服务清单落盘 + 启动自愈"。

### Step 6 · TUN（P2，会弹一次密码）

```bash
netstat -rn | grep utun      # 应有 utunN 且 default 指向它
scutil --dns | head          # DNS 被接管
curl https://ip.sb           # 不带 -x，出口 IP 应该变
```

排查顺序：`~/Library/Application Support/rekobox/config/sing-box-vpn.json` 里
`interface_name` 是不是 `utun9`（建议改空）、`route.auto_detect_interface` 是不是 `true`。
`auto_route` 在 mac 上走 `route add`；`strict_route` / `auto_redirect` 在 mac 无效，别配。

### Step 7 · 打包分发（P3）

```bash
bash RekoboxForWindows/libs/build_macos.sh --dmg
```

产物 `RekoboxForMac/dist/Rekobox-<ver>-mac-arm64.dmg`，ad-hoc 签名（本机能开，
别人下载要右键 → 打开）。正式分发要 Developer ID + 公证，命令在
`06-macos-port.md` §7，需要 Apple 开发者账号（$99/年）。

---

## 4. 首次编译可能撞上的错（按概率排序）

先看 `RekoboxForMac/build/CMakeFiles/CMakeConfigureLog.yaml` 和 ninja 的第一条错误，
不要一次改十个地方。

| 现象 | 原因 | 处理 |
|---|---|---|
| `Unknown CMake command "protobuf_generate"` | 用了 brew 的新版 protobuf（没有该函数/签名变了） | 删掉 CMakeCache 重来，先跑 `build_deps_macos.sh`，确认 `CMAKE_PREFIX_PATH` 里有 `libs/deps/built` |
| `Could not find a package configuration file provided by "ZXing"` | 没装/没传 prefix | `build_deps_macos.sh` 会装到 `libs/deps/built/lib/cmake/ZXing`；把该路径加进 `CMAKE_PREFIX_PATH` |
| `ld: framework not found Carbon` | PLATFORM_LIBRARIES 没生效 | 确认 `cmake/macos/macos.cmake` 被 include（`CMakeLists.txt` 的 `elseif(APPLE)`） |
| `'sys/windows/MiniDump.h' file not found` | 平台选择没生效，走到了 WIN32 分支 | 同上；确认 `cmake` 输出里 `APPLE` 为真 |
| `Undefined symbols: _LSSharedFileListCreate` | `sys/AutoRun.cpp` 的 mac 分支缺 framework | `-framework CoreServices` 已在 macos.cmake；若仍报错，把 `AutoRun.cpp` 里 mac 实现临时换成写 LaunchAgent plist |
| QHotkey 报 `Carbon/Carbon.h` 找不到 | 同上 | 确认 framework 列表 |
| `No rule to make target .../rekobox.icns` | 图标还没生成却被引用 | 先 `bash libs/make_icns.sh`；脚本已做"没有就先生成" |
| 编译通过但启动即退 | 数据目录不可写 / 单实例锁残留 | 看 Console.app 崩溃日志；删 `~/Library/Application Support/rekobox/.instance.lock` |
| 界面能开但工具栏图标空白 | SVG 走 fallback 失败 | 把 `main.cpp` 的 fallback 路径改成绝对资源 `:/icon`，或在 `ThemeManager` 里显式 `QIcon(":/icon/b-*.svg")` |
| `macdeployqt` 报 `codesign failed` | 之前有 ad-hoc 签名残留 | `codesign --remove-signature` 或删掉 `RekoBox.app` 重编 |
| 内核跑不起来 `killed: 9` | 二进制没签名 / 架构不对 | `file Content/MacOS/rekobox_core` 应为 `arm64`；`codesign -s -` 重签 |

修完记得把结论补回这张表——下一个人（包括你）会再撞一次。

---

## 5. 代码地图（改哪里）

```
RekoboxForWindows/                 ← 桌面共享树（Windows 正式版就靠它，别搞坏）
├── CMakeLists.txt                 ← 平台分支 + bundle 属性
├── cmake/
│   ├── windows/windows.cmake      ← 已有，别动
│   ├── linux/linux.cmake          ← 已有，别动
│   └── macos/
│       ├── macos.cmake            ← 平台源文件 / framework / arm64 / 13.0
│       └── Info.plist.in          ← bundle 元数据
├── sys/
│   ├── windows/                   ← 不参与 mac 编译
│   ├── linux/                     ← 同上
│   └── macos/MacPlatform.*        ← 数据目录 / Fusion / 转义；以后加 NSWorkspace、模板图
├── res/vpn/vpn-run-root-macos.sh  ← TUN 启动脚本（qrc 里已登记）
├── db/ConfigBuilder.cpp           ← 生成 tun 配置 + 选脚本（genTunName 在这里）
├── ui/mainwindow.cpp              ← 托盘、TUN 启停、状态栏
├── main/main.cpp                  ← 数据目录、外观、单实例
└── libs/build_macos.sh            ← 一键构建，改这里加步骤

RekoboxForMac/                     ← 只放 mac 专属的东西，不放源码副本
├── README.md
├── docs/HANDOFF.md                ← 本文
├── mockups/                       ← 效果图（index.html + 5 png + shoot.js）
├── build/                         ← 构建产物（gitignore）
└── dist/                          ← dmg（gitignore）
```

---

## 6. 已定的决策（不要重新讨论）

1. **不新建 Xcode 工程、不复制源码**。共享 `RekoboxForWindows/`，CMake 分支。
   目录改名（→ `RekoboxForDesktop`）等 mac 编过再一次性做，命令写在
   `06-macos-port.md` §3.1。
2. **不碰 Android 那套 UI**。Fragment/View 体系，复用率约 0。
3. **不上 NetworkExtension / App Store**。要沙盒 + 系统扩展，等于重写。
4. **TUN 提权顺序**：`osascript`（现在）→ `SMAppService` 特权助手（P4）。
5. **arm64 only 先发**。universal 二进制（`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`）
   等 P5，Qt 本来就是 universal，只需重编内核并 `lipo`。
6. **界面先做"和 Windows 一样"**（Fusion + 原 `.ui`），Mac 化外壳（侧边栏）是 P5 可选项，
   而且必须拆成 `ui/pages/*` 复用，不能复制主窗口。

---

## 7. 还没做的（按优先级）

**P2（下一会话的主线）**

- [ ] 把第一次编译跑通，更新 §4 表
- [ ] 系统代理退出还原 + 服务清单落盘 + 崩溃自愈（`QvProxyConfigurator.cpp`）
- [ ] TUN 实机验证：`utun` 建起来、路由生效、停掉能恢复
- [ ] `genTunName()` 在 mac 上返回空（让 sing-box 自动挑 `utunN`），避免撞 Tailscale
- [ ] 托盘：菜单栏模板图（`QIcon::setIsMask(true)` + 纯黑透明 PNG）
- [ ] 单实例 + `-tray` 启动 + 关窗不退出 的 mac 行为核对
- [ ] 翻译路径改用 `QLibraryInfo::TranslationsPath`

**P3**

- [ ] Developer ID 签名 + `notarytool` 公证 + `stapler`（要账号）
- [ ] `.github/workflows/build-macos.yml`（macos-14 runner，ad-hoc 产物）
- [ ] `RekoboxForMac/build/`、`dist/`、`res/rekobox.icns` 的 gitignore 策略

**P4**

- [ ] `SMAppService` 登录项（替掉废弃的 `LSSharedFileList`）
- [ ] 特权助手（XPC）替代每次输密码
- [ ] App Nap：`NSProcessInfo beginActivity` 防止后台流量统计停滞
- [ ] 崩溃日志：`qInstallMessageHandler` + `backtrace()` → `~/Library/Logs/Rekobox`
- [ ] 自动更新（Sparkle 2 或 GitHub Releases + dmg）

**P5**

- [ ] 深色 QSS（现有 4 套主题都是浅色）
- [ ] Mac 化外壳（侧边栏 / 状态卡 / 菜单栏弹出面板，见 mockups B1/B2）

---

## 8. 交接给新对话

提示词在仓库根的 [`NEXT-SESSION-PROMPT.md`](../../NEXT-SESSION-PROMPT.md)，整段复制粘贴即可。
内容如下（保持同步）：

```
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
```

---

## 9. 一句话提醒

macOS 上真正难的从来不是界面（Qt 已经解决了 95%），而是 **root 权限下的 TUN 生命周期**：
授权、睡眠唤醒、网络切换、异常退出后的路由残留。P2 的时间要留给它，别耗在像素上。
