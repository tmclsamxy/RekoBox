# RekoBox for Android — 改名改造清单

> 基线：`tmclsamxy/NekoBoxForAndroid` @ `a4c5072`（`bump version`，2026-08-22）
> 工作副本：`D:/WorkBuddy/Rekobox/RekoboxForAndroid`
> 变更规模：**260 个文件**（204 重命名 + 47 修改 + 6 新增 + 3 删除）
> 自动化脚本：`tools/rebrand.sh`（幂等，可重复执行）

---

## 0. 一句话结论

对外可见的标识全部换掉了：**应用名 `RekoBox`、applicationId `io.rekobox`、
Kotlin 包名 `io.rekobox.android`、图标、关于页链接、备份文件名、WebDAV 目录、UA、
CI 产物名**。有两类字符串**故意保留**（插件协议、深链 scheme），原因见 §4。

---

## 1. 标识映射表

| 维度 | 原值 | 新值 | 落点 |
|---|---|---|---|
| 应用显示名 | `NekoBoxF` | `RekoBox` | `res/values*/strings.xml` |
| 长名称 | `NekoBoxF for Android` | `RekoBox for Android` | 同上 |
| applicationId | `com.nb4a` | `io.rekobox` | `nb4a.properties` |
| namespace | `io.nekohasekai.sagernet` | `io.rekobox.android` | `app/build.gradle.kts` |
| 主包名 | `io.nekohasekai.sagernet` | `io.rekobox.android` | 204 个 kt/java/aidl 文件 |
| 扩展包名 | `moe.matsuri.nb4a` | `io.rekobox.android.nb4a` | 13 个目录 |
| rootProject | `NB4A` | `RekoBox` | `settings.gradle.kts` |
| APK 产物名 | `NekoBoxF-1.4.2…` | `RekoBox-1.0.0…` | `buildSrc/…/Helpers.kt` |
| CI 产物 | `NekoBoxs` | `RekoBox` | `.github/workflows/build.yml` |
| 备份文件名 | `nekobox_backup_*.json` | `rekobox_backup_*.json` | `BackupFragment.kt` |
| WebDAV 目录 | `NekoBox` | `RekoBox` | `DataStore.kt` + `webdav_preferences.xml` |
| HTTP UA | `NekoBox/Android/<ver>` | `RekoBox/Android/<ver>` | `ktx/Nets.kt` |

---

## 2. 逐文件改动（可直接照做）

### 2.1 构建与元数据

| 文件 | 行 | 改动 |
|---|---|---|
| `nb4a.properties` | 1–4 | `PACKAGE_NAME=io.rekobox`；版本重置 `1.0.0 / pre-1.0.0-1 / 1` |
| `settings.gradle.kts` | 2 | `rootProject.name = "RekoBox"` |
| `app/build.gradle.kts` | 30 | `namespace = "io.rekobox.android"` |
| `buildSrc/src/main/kotlin/Helpers.kt` | 200, 203 | `"NekoBoxF-"` → `"RekoBox-"` |
| `app/proguard-rules.pro` | 4–5 | `-keep class io.rekobox.android.**` / `…android.nb4a.**` |
| `.github/workflows/build.yml` | 87 | 产物名 `RekoBox` |

> `applicationId` 只在 `nb4a.properties` 一处定义，由 `Helpers.kt:144` 的
> `setupApp()` 注入 → **改一处即全局生效**。

### 2.2 字符串与界面文案

| 文件 | 行 | 改动 |
|---|---|---|
| `res/values/strings.xml` | 3–4 | `app_name` / `app_name_long` |
| `res/values-fa/strings.xml` | 3–4 | 同上 |
| `res/values-uk/strings.xml` | 3–4 | 同上 |
| `res/values-ru/strings.xml` | 38–39 | `NekoBoxF для Android` → `RekoBox для Android` |

其余 20 个 locale **不含品牌词**（`app_name` 标了 `translatable="false"`，只有上述 4 个文件重复定义）。

### 2.3 运行期硬编码

| 文件 | 行 | 改动 |
|---|---|---|
| `ktx/Nets.kt` | 66 | `USER_AGENT = "RekoBox/Android/…"` |
| `utils/CrashHandler.kt` | 65 | 崩溃报告头 `RekoBox for Android …` |
| `database/DataStore.kt` | 326 | WebDAV 默认目录 `RekoBox` |
| `res/xml/webdav_preferences.xml` | 31 | `android:defaultValue="RekoBox"` |
| `ui/BackupFragment.kt` | 119/134/209/380–382/392/576 | `nekobox_backup_` → `rekobox_backup_`（含 3 条正则） |
| `res/xml/shortcuts.xml` | 11/21/31/41 | `android:targetPackage="com.nb4a"` → `"io.rekobox"` |

> ⚠️ `shortcuts.xml` 的 `targetPackage` **写死为旧包名**，不改会导致桌面长按快捷方式点击无效
> —— 这是最容易被漏掉的一处。

### 2.4 关于页 / 外链（已重构为集中配置）

原实现把 4 个 URL 硬编码在 `ui/AboutFragment.kt`。改为：

**新增** `app/src/main/java/io/rekobox/android/Branding.kt`

```kotlin
object Branding {
    const val REPO_OWNER = "tmclsamxy"
    const val REPO_NAME  = "RekoBoxForAndroid"
    const val REPO_URL   = "https://github.com/$REPO_OWNER/$REPO_NAME"
    const val RELEASES_URL = "$REPO_URL/releases"
    const val API_RELEASES_LATEST  = "https://api.github.com/repos/$REPO_OWNER/$REPO_NAME/releases/latest"
    const val API_RELEASES_PREVIEW = "https://api.github.com/repos/$REPO_OWNER/$REPO_NAME/releases/tags/preview"
}
```

`AboutFragment.kt` 的 86 / 186 / 198 / 228 / 230 行改为引用 `Branding.*`。

> **只改这一处**即可切换仓库地址。但注意 `API_RELEASES_*` 指向的仓库必须真实存在，
> 否则「检查更新」会 404（异常被 `runOnIoDispatcher` 吞掉，表现为无反应）。

### 2.5 图标

5 个密度 × 2 张 PNG 全部重绘（`tools/gen_icons.py`，PIL 生成，可复现）：

```
res/mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/ic_launcher.png            48/72/96/144/192
res/mipmap-{...}/ic_launcher_foreground.png                            108/162/216/324/432
res/values/ic_launcher_background.xml      #FFFFFF → #0B1220
```

自适应图标 `mipmap-anydpi-v26/ic_launcher.xml` 结构不变（背景色 + 前景图）。

### 2.6 包名迁移（改动量最大）

```
app/src/main/java/io/nekohasekai/sagernet/  →  app/src/main/java/io/rekobox/android/
app/src/main/java/moe/matsuri/nb4a/         →  app/src/main/java/io/rekobox/android/nb4a/
app/src/main/aidl/io/nekohasekai/sagernet/aidl/ → app/src/main/aidl/io/rekobox/android/aidl/
```

同步替换的三类引用：

1. **Kotlin/Java**：`package` 声明 + `import`（204 文件）
2. **XML**：自定义 View 的 FQN —— `layout_main.xml:32/36/47/99`、`preference_dropdown_reselectable.xml:14`、
   `balancer_preferences.xml:26`、`group_preferences.xml:32/38/48/98`、`route_preferences.xml:15/63`
3. **AndroidManifest.xml**：84 处 `android:name="io.nekohasekai.sagernet.…"`

**AIDL**：5 个 `.aidl` 的 `package` 与 `import` 行同步改（否则 `aidl` 编译器报目录不匹配）。

**Room schema 目录**（首轮漏掉，编译期才暴露）：

```
app/schemas/io.nekohasekai.sagernet.database.SagerDatabase        → app/schemas/io.rekobox.android.database.SagerDatabase
app/schemas/io.nekohasekai.sagernet.database.preference.PublicDatabase → …io.rekobox.android.database.preference.PublicDatabase
app/schemas/moe.matsuri.nb4a.TempDatabase                         → app/schemas/io.rekobox.android.nb4a.TempDatabase
```

不改会报 `Schema '3.json' required for migration was not found at the schema out folder`，
因为 Room 按「当前包名」拼 schema 目录路径。

**JNI 特例**（普通 sed 容易漏）：

```
buildScript/compile-hevtun.sh:63
-APP_CFLAGS=… -DPKGNAME=moe/matsuri/nb4a/hevtun -DCLSNAME=HevTunNative
+APP_CFLAGS=… -DPKGNAME=io/rekobox/android/nb4a/hevtun -DCLSNAME=HevTunNative
```

`libcore/` 是 Go 模块，`gomobile bind` 产出的 Java 类固定为 `libcore.Libcore` / `go.Seq`，
**不受 Kotlin 包名影响，无需改动**。

### 2.7 README

标题 → `# RekoBox for Android`；badge / 下载链接 / Telegram / Contributors 改指向新仓库；
顶部加 GPL-3.0 上游声明；Credits 增加 `MatsuriDayo/NekoBoxForAndroid` 条目。

---

## 3. 执行方式

```bash
cd D:/WorkBuddy/Rekobox/RekoboxForAndroid
bash tools/rebrand.sh          # 幂等，可重复跑
python tools/gen_icons.py      # 只在需要重绘图标时跑
git add -A && git commit -m "rebrand: NekoBox -> RekoBox"
```

脚本第 6 步自带残留校验，输出为空即干净。

---

## 4. 故意**不**改的东西（重要）

| 保留项 | 位置 | 原因 |
|---|---|---|
| `io.nekohasekai.sagernet.plugin.*` | `nb4a/plugin/Plugins.kt:15-20`、`AndroidManifest.xml:46` | 这是**第三方插件 APK 的真实包名/action**，改了官方 SagerNet 插件就装不上、连不通 |
| `moe.matsuri.exe.*`、`moe.matsuri.lite` | `fmt/PluginEntry.kt`、`Plugins.kt` | 同上，外部插件包名 |
| `sn://` / `clash://` 深链 scheme | `AndroidManifest.xml` | 订阅链接与分享链接的既有格式，改了历史链接全部失效 |
| `SagerNet` 类名、`Theme.SagerNet` 样式、`ISagerNetService` AIDL 名 | 全局 | 纯内部符号，用户不可见；改名收益低、回归风险高 |
| `libcore` Go 模块 | `libcore/` | 内核与上游共用，改名会破坏与 sing-box 的 `//go:linkname` |
| `AUTHORS` / `LICENSE` | 根目录 | GPL-3.0 要求保留原版权声明 |

如果确实要连插件协议一起改，需同时发布配套插件包，属于另一个工程。

---

## 5. 合规提示（别跳过）

上游是 **GPL-3.0**。再发行必须做到：

1. 保留 `LICENSE`、`AUTHORS`，不得删除原版权头；
2. 修改过的源码**必须公开**（你 fork 的仓库本身就是履行方式）；
3. 二进制分发时提供对应源码的获取途径；
4. 不得使用上游的商标/图标暗示官方背书 —— 这也是本次全面改名的另一重理由。

---

## 6. 验收清单

```bash
# 1) 无残留旧包名（除插件协议）
grep -rn "io\.nekohasekai" . --exclude-dir=.git | grep -v "\.plugin"

# 2) 无残留旧应用名
grep -rn "NekoBoxF" . --exclude-dir=.git

# 3) package 声明与目录一致（脚本化校验，应输出 0）
python - <<'PY'
import os,io,re
bad=0
for root,_,fs in os.walk('app/src/main/java'):
    for f in fs:
        if f.endswith(('.kt','.java')):
            p=os.path.join(root,f); rel=os.path.relpath(root,'app/src/main/java').replace(os.sep,'.')
            pkg=re.search(r'^package\s+([\w.]+)',io.open(p,encoding='utf-8',errors='ignore').read(3000),re.M)
            if pkg and pkg.group(1)!=rel: bad+=1; print(p,rel,pkg.group(1))
print('mismatch:',bad)
PY

# 4) 资源 XML 里的自定义 View FQN 都能解析到源文件（应输出 missing: []）

# 5) 实机验证
./gradlew app:assembleOssDebug
# 安装后检查：桌面图标名、应用信息里的包名、长按快捷方式、关于页链接、
#            备份文件名、崩溃报告头、设置里的 WebDAV 默认目录
```

> 本次改造**未执行 Gradle 编译**（需 NDK + Go + gomobile 生成 `libcore.aar`，
> 且 `preBuild` 依赖 `buildHevTun` 会调用 ndk-build）。上面的静态校验已全部通过：
> 包目录一致性 0 处不匹配、XML 自定义 View 引用 0 处缺失、旧包名残留仅剩插件协议行。
