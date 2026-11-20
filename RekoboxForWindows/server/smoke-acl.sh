#!/usr/bin/env bash
# 权限组端到端自检：用一个临时数据目录跑真服务，逐条验收「账号只能拉到
# 自己权限组里的节点」以及「权限变更后客户端能立刻看到新结果」。
#
# 用法：
#   BIN=/path/to/rekosync bash smoke-acl.sh
#   bash smoke-acl.sh                 # 默认用同目录下的 ./rekosync
#
# 只依赖 curl 与 sed，不需要 jq/python。全部通过时退出码为 0。

set -u

BIN=${BIN:-./rekosync}
PORT=${PORT:-18099}
BASE="http://127.0.0.1:${PORT}"

# 本地回环必须绕过代理，否则 http_proxy 会把 127.0.0.1 也劫持掉
C() { curl -sS --noproxy '*' "$@"; }
# 从 stdin 的 JSON 里取第一个字符串/数字字段
jstr() { sed -n 's/.*"'"$1"'":"\([^"]*\)".*/\1/p' | head -1; }
jnum() { sed -n 's/.*"'"$1"'":\([0-9][0-9]*\).*/\1/p' | head -1; }
jbool() { sed -n 's/.*"'"$1"'":\(true\|false\).*/\1/p' | head -1; }
login() {
  C -X POST "$BASE/api/v1/auth/login" -H 'Content-Type: application/json' \
    -d "{\"username\":\"$1\",\"password\":\"$2\"}" | jstr token
}

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf '  \033[32m✓\033[0m %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  \033[31m✗\033[0m %s\n' "$1"; }
check_eq() { # 期望 实际 描述
  if [ "$1" = "$2" ]; then ok "$3（$2）"; else bad "$3：期望 $1，实际 $2"; fi
}

[ -x "$BIN" ] || { echo "找不到可执行文件：$BIN（先 go build -o rekosync .）"; exit 2; }

DATA=$(mktemp -d) || exit 1
SRV_PID=""
cleanup() {
  [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
  [ -n "$SRV_PID" ] && wait "$SRV_PID" 2>/dev/null
  [ -d "$DATA" ] && rm -rf -- "$DATA"
}
trap cleanup EXIT

echo "== 准备数据目录 =="
"$BIN" -data "$DATA" -addadmin root  -password 'admin-pass' >/dev/null
"$BIN" -data "$DATA" -adduser  alice -password 'user-pass'  >/dev/null
"$BIN" -data "$DATA" -adduser  bob   -password 'user-pass'  >/dev/null
ok "建好 root（管理员）/ alice / bob"

"$BIN" -addr "127.0.0.1:${PORT}" -data "$DATA" >"$DATA/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 50); do
  C "$BASE/api/v1/health" >/dev/null 2>&1 && break
  sleep 0.1
done
C "$BASE/api/v1/health" >/dev/null || { echo "服务没起来，日志："; cat "$DATA/server.log"; exit 1; }
ok "服务已启动（$BASE）"

echo "== 录入 3 个节点 =="
ROOT=$(login root admin-pass)
ADD=$(C -X POST "$BASE/api/v1/admin/nodes" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' \
  -d '{"links":"tuic://u1:p@10.0.0.1:25247#n1\ntuic://u2:p@10.0.0.2:25247#n2\ntuic://u3:p@10.0.0.3:25247#n3"}')
N1=$(grep -o '"id":"[^"]*"' <<<"$ADD" | sed -n '1s/.*"id":"\([^"]*\)"/\1/p')
N2=$(grep -o '"id":"[^"]*"' <<<"$ADD" | sed -n '2s/.*"id":"\([^"]*\)"/\1/p')
N3=$(grep -o '"id":"[^"]*"' <<<"$ADD" | sed -n '3s/.*"id":"\([^"]*\)"/\1/p')
if [ -n "$N1" ] && [ -n "$N2" ] && [ -n "$N3" ]; then
  ok "3 个节点 id 已获取"
else
  bad "节点录入失败：$ADD"; exit 1
fi

echo "== 升级兼容：未分组账号仍是全量 =="
BOB=$(login bob user-pass)
check_eq 3 "$(C "$BASE/api/v1/nodes" -H "Authorization: Bearer $BOB" | jnum count)" \
  "bob（未分组 + 默认 open）可见全部节点"

echo "== 建权限组、配节点、分配账号 =="
GID=$(C -X POST "$BASE/api/v1/admin/groups" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' -d '{"name":"套餐A"}' | jstr id)
if [ -n "$GID" ]; then ok "权限组已创建（$GID）"; else bad "建组失败"; exit 1; fi

check_eq 2 "$(C -X POST "$BASE/api/v1/admin/groups/$GID/nodes" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' -d "{\"ids\":[\"$N1\",\"$N2\"]}" | jnum node_count)" \
  "权限组配置了 2 个节点"

check_eq 2 "$(C -X POST "$BASE/api/v1/admin/users/alice/groups" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' -d "{\"groups\":[\"$GID\"]}" | jnum node_count)" \
  "alice 分配后可见 2 个节点"

echo "== 按权限组鉴权 =="
ALICE=$(login alice user-pass)
BODY=$(C "$BASE/api/v1/nodes" -H "Authorization: Bearer $ALICE")
check_eq 2 "$(jnum count <<<"$BODY")" "alice 拉取只拿到 2 个节点"
check_eq true "$(jbool restricted <<<"$BODY")" "响应标记 restricted"
grep -q "\"$N3\"" <<<"$BODY" && bad "越权：alice 拿到了组外节点 $N3" || ok "组外节点 $N3 没有下发"
check_eq 3 "$(C "$BASE/api/v1/nodes" -H "Authorization: Bearer $BOB" | jnum count)" \
  "bob（仍未分组）依旧 3 个"

echo "== 权限变更后客户端立即失效重拉 =="
V=$(jnum version <<<"$BODY")
check_eq true "$(C "$BASE/api/v1/nodes?version=$V" -H "Authorization: Bearer $ALICE" | jbool unchanged)" \
  "同版本号查询命中 unchanged"

C -X POST "$BASE/api/v1/admin/groups/$GID/nodes" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' -d "{\"ids\":[\"$N1\"]}" >/dev/null
AFTER=$(C "$BASE/api/v1/nodes?version=$V" -H "Authorization: Bearer $ALICE")
grep -q '"unchanged":true' <<<"$AFTER" && bad "改权限组后旧版本号仍然 unchanged" || ok "旧版本号已失效"
check_eq 1 "$(jnum count <<<"$AFTER")" "alice 重新拉取后只剩 1 个节点"

echo "== 收紧兜底策略 =="
C -X POST "$BASE/api/v1/admin/acl/policy" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' -d '{"default_policy":"closed"}' >/dev/null
check_eq 0 "$(C "$BASE/api/v1/nodes" -H "Authorization: Bearer $BOB" | jnum count)" \
  "closed 后 bob（未分组）拉不到节点"
check_eq 1 "$(C "$BASE/api/v1/nodes" -H "Authorization: Bearer $ALICE" | jnum count)" \
  "alice 不受影响"

echo "== 越权与错误处理 =="
check_eq 403 "$(C -o /dev/null -w '%{http_code}' "$BASE/api/v1/admin/groups" \
  -H "Authorization: Bearer $ALICE")" "普通账号访问管理接口"
check_eq 401 "$(C -o /dev/null -w '%{http_code}' "$BASE/api/v1/nodes")" "无令牌拉节点"
check_eq 409 "$(C -o /dev/null -w '%{http_code}' -X POST "$BASE/api/v1/admin/groups" \
  -H "Authorization: Bearer $ROOT" -H 'Content-Type: application/json' -d '{"name":"套餐a"}')" \
  "重名权限组（忽略大小写）"
check_eq 400 "$(C -o /dev/null -w '%{http_code}' -X POST "$BASE/api/v1/admin/groups/$GID/nodes" \
  -H "Authorization: Bearer $ROOT" -H 'Content-Type: application/json' -d '{"ids":["deadbeef"]}')" \
  "给权限组配置不存在的节点"

echo "== 删组连带摘掉归属 =="
C -X POST "$BASE/api/v1/admin/acl/policy" -H "Authorization: Bearer $ROOT" \
  -H 'Content-Type: application/json' -d '{"default_policy":"open"}' >/dev/null
C -X DELETE "$BASE/api/v1/admin/groups/$GID" -H "Authorization: Bearer $ROOT" >/dev/null
check_eq 3 "$(C "$BASE/api/v1/nodes" -H "Authorization: Bearer $ALICE" | jnum count)" \
  "删组后 alice 回到未分组（open 下全量）"
"$BIN" -data "$DATA" -users | grep -q 'alice.*(未分组)' && ok "账号上的组引用已摘掉" \
  || bad "账号上仍残留已删除的组"

echo "== 重启后状态保持 =="
[ -f "$DATA/acl.json" ] && ok "data/acl.json 已写出" || bad "没有 acl.json"
kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""
"$BIN" -addr "127.0.0.1:${PORT}" -data "$DATA" >>"$DATA/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 50); do C "$BASE/api/v1/health" >/dev/null 2>&1 && break; sleep 0.1; done
ALICE2=$(login alice user-pass)
[ -n "$ALICE2" ] && ok "重启后账号仍可登录" || bad "重启后登录失败"
check_eq open "$(C -X POST "$BASE/api/v1/admin/acl/policy" -H "Authorization: Bearer $(login root admin-pass)" \
  -H 'Content-Type: application/json' -d '{"default_policy":"open"}' | jstr default_policy)" \
  "策略在重启后依然可读"

echo
printf '通过 %d 项，失败 %d 项\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
