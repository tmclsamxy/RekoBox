# 从这里开始（macOS 开发机）

这份代码是从 Windows 上打包带过来的快照。**macOS 端骨架已写完，但从未编译过**——
第一次 `cmake` 报错是预期内的，不是失败。

---

## 0. 文件怎么传到 Mac

包在 Windows 的：

```
C:\Users\Mrcheng\WorkBuddy\spaces\2026_11_20_16-03-25\rekobox\dist\
├── Rekobox-mac-dev.bundle   完整 git 仓库，9.6 MB，744 个提交        ← 推荐
└── Rekobox-mac-dev.zip      纯快照，5.6 MB，无 .git（只解压就能看代码）
```

任选一种方式搬过去：U 盘 / 微信文件传输助手 / 局域网直接下载（见下）。

局域网下载（Windows 上开一个临时 HTTP 服务，Mac 浏览器打开）：

```bash
# Windows（PowerShell 或 Git Bash 都行）
cd "C:/Users/Mrcheng/WorkBuddy/spaces/2026_11_20_16-03-25/rekobox/dist"
python -m http.server 8000
```

然后在 Mac 浏览器访问 `http://<Windows 的局域网 IP>:8000/`，点文件名下载。
用完 Ctrl+C 关掉。

---

## 1. 落地

**优先：直接从 GitHub 拉**（仓库已推送，含全部历史；Mac 有网就用这个，最干净）

```bash
cd ~
git clone https://github.com/tmclsamxy/RekoBox.git Rekobox
cd Rekobox
git log --oneline -1         # 应看到 docs: 补充从 GitHub 直接 clone 的方式
```

没有网/不想等，用本地包：

```bash
cd ~
git clone ~/Downloads/Rekobox-mac-dev.bundle Rekobox     # bundle
# 或者
mkdir -p ~/Rekobox && cd ~/Rekobox && unzip ~/Downloads/Rekobox-mac-dev.zip
```

---

## 2. 装环境（约 20 分钟，含下载）

```bash
xcode-select --install                  # 只需要 Command Line Tools
brew install cmake ninja go

python3 -m pip install --user aqtinstall
python3 -m aqt install-qt mac desktop 6.7.2 clang_64 -O ~/Qt \
        --archives qtbase qtsvg qttools
```

自检：

```bash
~/Qt/6.7.2/macos/bin/qmake -query QT_VERSION   # 6.7.2
go version                                      # darwin/arm64
```

> Qt 锁 6.7.2 是为了和 Windows 版一致（QSS、控件行为）。官方 mac 包是 universal。

---

## 3. 编译（三条命令）

```bash
cd ~/Rekobox

bash RekoboxForWindows/libs/build_deps_macos.sh   # zxing 2.0 / yaml 0.7 / protobuf 21.4，约 10 分钟
bash RekoboxForWindows/libs/get_source_local.sh   # 拉 sing-box 内核源码
bash RekoboxForWindows/libs/build_macos.sh        # 编 .app
```

产物：`RekoboxForMac/build/RekoBox.app`。

- 8 GB 内存的 Air：protobuf 那步建议 `JOBS=4`（脚本默认取 CPU 核数，可能偏大）
- 编译期间别合盖：`caffeinate -dimsu` 或接电源
- 脚本是幂等的，报错修完直接重跑

---

## 4. 看结果

```bash
open RekoboxForMac/build/RekoBox.app
```

对照 `RekoboxForMac/mockups/index.html`（浏览器打开）的 **A1 图**逐项检查，
清单在 `RekoboxForMac/docs/HANDOFF.md` §3 Step 4。

---

## 5. 出问题

先查 `RekoboxForMac/docs/HANDOFF.md` **§4 首次编译可能撞上的错**——那张表列了
`protobuf_generate`、`ZXing`、`framework not found`、图标、签名等 10 类现象的修法。

修完请把新的"现象 → 原因 → 处理"补回那张表，下一个人（包括你自己）会再撞一次。

---

## 6. 让 AI 接着干

在 `~/Rekobox` 目录下开一个新的会话，把 **`NEXT-SESSION-PROMPT.md`** 整段贴进去。
那份提示词是自包含的：它会让 AI 先读交接文档，再按顺序编译、修错、验收。

如果 AI 有终端权限，它可以直接跑上面第 3 步；没有的话它会让你跑、你贴输出。

---

## 7. 以后要重新打包（在 Windows 上）

```bash
cd <repo>
git bundle create dist/Rekobox-mac-dev.bundle --all
```

只用 bundle，不要用 `git archive` 出 zip：这台 Windows 的 `core.autocrlf=true`，
导出的文本可能在解压后带 CRLF，`bash` 会报 `$'\r': command not found`。
bundle 由 Mac 侧 `git clone` 落地，换行符一定是 LF。

打包前记得先 commit——bundle 只包含已提交的内容。

