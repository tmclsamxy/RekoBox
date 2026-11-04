#!/usr/bin/env bash
# RekoBox rebrand script — 幂等，重复执行安全
# 用法: bash tools/rebrand.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OLD_NS='io.nekohasekai.sagernet'
NEW_NS='io.rekobox.android'
OLD_EXTRA='moe.matsuri.nb4a'
NEW_EXTRA='io.rekobox.android.nb4a'

echo ">> 1/6 冻结第三方插件协议字面量（不改，保持与 SagerNet 插件 APK 兼容）"
find app/src/main libcore -type f \( -name '*.kt' -o -name '*.java' -o -name '*.xml' -o -name '*.aidl' -o -name '*.pro' \) -print0 \
 | xargs -0 sed -i \
   -e 's|"io\.nekohasekai\.sagernet\.plugin\."|"__PLUGIN_AUTH_PREFIX__"|g' \
   -e 's|"io\.nekohasekai\.sagernet\.plugin\.id"|"__PLUGIN_META_ID__"|g' \
   -e 's|"io\.nekohasekai\.sagernet\.plugin\.trojan_go"|"__PLUGIN_TROJAN_GO__"|g' \
   -e 's|"io\.nekohasekai\.sagernet\.plugin\.ACTION_NATIVE_PLUGIN"|"__PLUGIN_ACTION__"|g'

echo ">> 2/6 重命名 Kotlin/AIDL 包路径"
if [ -d "app/src/main/java/io/nekohasekai/sagernet" ]; then
  mkdir -p app/src/main/java/io/rekobox
  git mv app/src/main/java/io/nekohasekai/sagernet app/src/main/java/io/rekobox/android
  rmdir app/src/main/java/io/nekohasekai 2>/dev/null || true
fi
if [ -d "app/src/main/java/moe/matsuri/nb4a" ]; then
  mkdir -p app/src/main/java/io/rekobox/android
  git mv app/src/main/java/moe/matsuri/nb4a app/src/main/java/io/rekobox/android/nb4a
  rmdir app/src/main/java/moe/matsuri app/src/main/java/moe 2>/dev/null || true
fi
if [ -d "app/src/main/aidl/io/nekohasekai/sagernet" ]; then
  mkdir -p app/src/main/aidl/io/rekobox/android
  git mv app/src/main/aidl/io/nekohasekai/sagernet/aidl app/src/main/aidl/io/rekobox/android/aidl
  rmdir app/src/main/aidl/io/nekohasekai/sagernet app/src/main/aidl/io/nekohasekai 2>/dev/null || true
fi

echo ">> 3/6 替换全仓库 FQN"
grep -rl --exclude-dir=.git --exclude-dir=tools -e "$OLD_EXTRA" -e "$OLD_NS" . \
 | grep -E '\.(kt|java|xml|aidl|pro|kts|txt|md|yml|properties|json|gradle)$' \
 | xargs -r sed -i -e "s|$OLD_EXTRA|$NEW_EXTRA|g" -e "s|$OLD_NS|$NEW_NS|g"

echo ">> 4/6 恢复插件协议字面量"
find app/src/main -type f \( -name '*.kt' -o -name '*.xml' \) -print0 \
 | xargs -0 sed -i \
   -e 's|"__PLUGIN_AUTH_PREFIX__"|"io.nekohasekai.sagernet.plugin."|g' \
   -e 's|"__PLUGIN_META_ID__"|"io.nekohasekai.sagernet.plugin.id"|g' \
   -e 's|"__PLUGIN_TROJAN_GO__"|"io.nekohasekai.sagernet.plugin.trojan_go"|g' \
   -e 's|"__PLUGIN_ACTION__"|"io.nekohasekai.sagernet.plugin.ACTION_NATIVE_PLUGIN"|g'

echo ">> 5/6 对外文案 / 标识"
sed -i \
  -e 's|<string name="app_name" translatable="false">NekoBoxF</string>|<string name="app_name" translatable="false">RekoBox</string>|' \
  -e 's|<string name="app_name_long" translatable="false">NekoBoxF for Android</string>|<string name="app_name_long" translatable="false">RekoBox for Android</string>|' \
  app/src/main/res/values/strings.xml app/src/main/res/values-fa/strings.xml app/src/main/res/values-uk/strings.xml
sed -i \
  -e 's|<string name="app_name">NekoBoxF</string>|<string name="app_name">RekoBox</string>|' \
  -e 's|<string name="app_name_long">NekoBoxF для Android</string>|<string name="app_name_long">RekoBox для Android</string>|' \
  app/src/main/res/values-ru/strings.xml

sed -i 's|targetPackage="com.nb4a"|targetPackage="io.rekobox"|g' app/src/main/res/xml/shortcuts.xml
sed -i 's|android:defaultValue="NekoBox"|android:defaultValue="RekoBox"|' app/src/main/res/xml/webdav_preferences.xml
sed -i 's|?: "NekoBox"|?: "RekoBox"|' app/src/main/java/io/rekobox/android/database/DataStore.kt
sed -i 's|"NekoBox/Android/"|"RekoBox/Android/"|' app/src/main/java/io/rekobox/android/ktx/Nets.kt
sed -i 's|"NekoBoxF for Android \${SagerNet|"RekoBox for Android ${SagerNet|' app/src/main/java/io/rekobox/android/utils/CrashHandler.kt
sed -i 's|nekobox_backup_|rekobox_backup_|g; s|nekobox_backup\.json|rekobox_backup.json|g' app/src/main/java/io/rekobox/android/ui/BackupFragment.kt
sed -i 's|"NekoBoxF-"|"RekoBox-"|g; s|"NekoBoxF-\$versionName"|"RekoBox-$versionName"|g' buildSrc/src/main/kotlin/Helpers.kt
sed -i 's|name: NekoBoxs|name: RekoBox|' .github/workflows/build.yml

echo ">> 6/6 校验残留"
echo "-- 残留 io.nekohasekai / moe.matsuri（应仅为插件协议行）:"
grep -rn --exclude-dir=.git --exclude-dir=tools -e 'io\.nekohasekai' -e 'moe\.matsuri' . | grep -v '\.plugin' | head -20 || true
echo "-- 残留 NekoBoxF:"
grep -rn --exclude-dir=.git --exclude-dir=tools 'NekoBoxF' . | head -20 || true
