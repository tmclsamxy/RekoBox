package main

// 权限组相关的单元测试与接口级测试。
//
// 覆盖点（与 docs/05 的验收清单一一对应）：
//   · 兼容：旧 users.json（无 groups 字段）+ 默认 open 策略 = 与升级前完全一致
//   · 隔离：账号只能拿到所属权限组的节点，多组取并集，停用节点不下发
//   · 提权：普通账号访问管理接口 403；管理员不受权限组限制
//   · 失效：权限组/归属变更会推高节点池版本，客户端的 ?version= 增量判断必然失效
//   · 错误：重名组 409、未知组 400、未知节点 id 400、删组连带摘掉账号引用

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

type testEnv struct {
	t     *testing.T
	dir   string
	store *Store
	nodes *NodeStore
	acl   *ACLStore
	ts    *httptest.Server
}

func newTestEnv(t *testing.T) *testEnv {
	t.Helper()
	dir := t.TempDir()
	store, err := OpenStore(dir)
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	nodes, err := OpenNodeStore(dir)
	if err != nil {
		t.Fatalf("OpenNodeStore: %v", err)
	}
	acl, err := OpenACLStore(dir)
	if err != nil {
		t.Fatalf("OpenACLStore: %v", err)
	}
	srv := NewServer(store, nodes, acl, "test")
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	return &testEnv{t: t, dir: dir, store: store, nodes: nodes, acl: acl, ts: ts}
}

// seedNodes 录入 n 条可用节点，返回它们的 id（按录入顺序）。
func (e *testEnv) seedNodes(n int) []string {
	e.t.Helper()
	links := make([]string, 0, n)
	for i := 1; i <= n; i++ {
		links = append(links, fmt.Sprintf("tuic://uuid:pass@10.0.0.%d:25247#节点%d", i, i))
	}
	added, rejects, err := e.nodes.AddLinks(links, "", "")
	if err != nil {
		e.t.Fatalf("AddLinks: %v", err)
	}
	if len(rejects) != 0 {
		e.t.Fatalf("录节点被拒：%+v", rejects)
	}
	ids := make([]string, 0, len(added))
	for _, n := range added {
		ids = append(ids, n.ID)
	}
	return ids
}

func (e *testEnv) mustUser(name, pass, role string, groups ...string) *User {
	e.t.Helper()
	u, err := e.store.CreateUser(name, pass, role, groups...)
	if err != nil {
		e.t.Fatalf("CreateUser(%s): %v", name, err)
	}
	return u
}

func (e *testEnv) mustGroup(name string, nodeIDs ...string) *ACLGroup {
	e.t.Helper()
	g, err := e.acl.Create(name, "")
	if err != nil {
		e.t.Fatalf("Create(%s): %v", name, err)
	}
	if len(nodeIDs) > 0 {
		if _, err := e.acl.SetNodes(g.ID, nodeIDs); err != nil {
			e.t.Fatalf("SetNodes(%s): %v", name, err)
		}
		g, _ = e.acl.Get(g.ID)
	}
	return g
}

func (e *testEnv) call(method, path, token string, body any) (int, map[string]any) {
	e.t.Helper()
	var rdr *bytes.Reader
	if body == nil {
		rdr = bytes.NewReader(nil)
	} else {
		b, err := json.Marshal(body)
		if err != nil {
			e.t.Fatalf("marshal: %v", err)
		}
		rdr = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, e.ts.URL+path, rdr)
	if err != nil {
		e.t.Fatalf("NewRequest: %v", err)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatalf("%s %s: %v", method, path, err)
	}
	defer res.Body.Close()
	out := map[string]any{}
	_ = json.NewDecoder(res.Body).Decode(&out)
	return res.StatusCode, out
}

func (e *testEnv) login(name, pass string) string {
	e.t.Helper()
	code, body := e.call(http.MethodPost, "/api/v1/auth/login", "",
		map[string]any{"username": name, "password": pass})
	if code != http.StatusOK {
		e.t.Fatalf("login(%s) = %d %v", name, code, body)
	}
	tok, _ := body["token"].(string)
	if tok == "" {
		e.t.Fatalf("login(%s) 没有返回 token：%v", name, body)
	}
	return tok
}

// nodeNames 取 /api/v1/nodes 里的节点名集合。
func nodeNames(body map[string]any) map[string]bool {
	out := map[string]bool{}
	list, _ := body["nodes"].([]any)
	for _, item := range list {
		if m, ok := item.(map[string]any); ok {
			if name, ok := m["name"].(string); ok {
				out[name] = true
			}
		}
	}
	return out
}

// ---- 兼容性 ----

// 升级前写下的 users.json 没有 groups 字段，读进来必须等价于「未分组」，
// 而且在默认 open 策略下仍然能看到全部节点——这是不破坏现有功能的核心保证。
func TestLegacyUsersFileStillSeesAllNodes(t *testing.T) {
	dir := t.TempDir()
	legacy := `{"users":{"alice":{"name":"alice","password":"pbkdf2_sha256$210000$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","created_at":1700000000000}}}`
	if err := os.WriteFile(filepath.Join(dir, "users.json"), []byte(legacy), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := OpenStore(dir)
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	u := store.GetUser("alice")
	if u == nil {
		t.Fatal("旧 users.json 里的账号没读出来")
	}
	if len(u.Groups) != 0 {
		t.Fatalf("旧账号应视为未分组，实际 groups=%v", u.Groups)
	}
	acl, err := OpenACLStore(dir)
	if err != nil {
		t.Fatalf("OpenACLStore: %v", err)
	}
	if acl.Policy() != PolicyOpen {
		t.Fatalf("没有 acl.json 时应默认 open，实际 %q", acl.Policy())
	}
	allow, restricted := acl.Scope(u.Groups)
	if restricted || allow != nil {
		t.Fatalf("open 策略下未分组账号应不受限，实际 allow=%v restricted=%v", allow, restricted)
	}
	if _, err := os.Stat(filepath.Join(dir, "acl.json")); !os.IsNotExist(err) {
		t.Fatal("只读打开不应凭空创建 acl.json")
	}
}

// 权限组、归属都要能落盘并在重启后恢复。
func TestACLPersistsAcrossRestart(t *testing.T) {
	dir := t.TempDir()
	nodes, _ := OpenNodeStore(dir)
	added, _, err := nodes.AddLinks([]string{"vless://u@1.1.1.1:443#n1", "vless://u@1.1.1.2:443#n2"}, "", "")
	if err != nil || len(added) != 2 {
		t.Fatalf("AddLinks: %v %v", added, err)
	}
	acl, _ := OpenACLStore(dir)
	g, err := acl.Create("套餐A", "说明")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := acl.SetNodes(g.ID, []string{added[0].ID}); err != nil {
		t.Fatal(err)
	}
	if err := acl.SetPolicy(PolicyClosed); err != nil {
		t.Fatal(err)
	}
	store, _ := OpenStore(dir)
	if _, err := store.CreateUser("alice", "secret1", "user", g.ID); err != nil {
		t.Fatal(err)
	}

	acl2, err := OpenACLStore(dir)
	if err != nil {
		t.Fatalf("重开权限组失败：%v", err)
	}
	if acl2.Policy() != PolicyClosed {
		t.Fatalf("策略没落盘，读到 %q", acl2.Policy())
	}
	g2, ok := acl2.Get(g.ID)
	if !ok || g2.Name != "套餐A" || len(g2.Nodes) != 1 || g2.Nodes[0] != added[0].ID {
		t.Fatalf("权限组没正确恢复：%+v", g2)
	}
	store2, _ := OpenStore(dir)
	u := store2.GetUser("alice")
	if u == nil || len(u.Groups) != 1 || u.Groups[0] != g.ID {
		t.Fatalf("账号归属没落盘：%+v", u)
	}
}

// ---- 隔离 ----

func TestNodesFilteredByGroup(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(3)
	g := e.mustGroup("套餐A", ids[0], ids[1])
	e.mustUser("alice", "secret1", "user", g.ID)
	e.mustUser("bob", "secret1", "user") // 未分组
	e.mustUser("root", "secret1", "admin")

	alice := e.login("alice", "secret1")
	code, body := e.call(http.MethodGet, "/api/v1/nodes", alice, nil)
	if code != http.StatusOK {
		t.Fatalf("alice /nodes = %d %v", code, body)
	}
	got := nodeNames(body)
	if len(got) != 2 || !got["节点1"] || !got["节点2"] {
		t.Fatalf("alice 应只看到 节点1/节点2，实际 %v", got)
	}
	if body["restricted"] != true {
		t.Fatalf("restricted 应为 true：%v", body)
	}

	// 未分组 + open 策略 = 全量（兼容）
	bob := e.login("bob", "secret1")
	_, bbody := e.call(http.MethodGet, "/api/v1/nodes", bob, nil)
	if n := len(nodeNames(bbody)); n != 3 {
		t.Fatalf("bob（未分组/open）应看到 3 个节点，实际 %d", n)
	}
	if bbody["restricted"] != false {
		t.Fatalf("bob 的 restricted 应为 false：%v", bbody)
	}

	// 管理员永远看全量
	root := e.login("root", "secret1")
	_, rbody := e.call(http.MethodGet, "/api/v1/nodes", root, nil)
	if n := len(nodeNames(rbody)); n != 3 {
		t.Fatalf("管理员应看到 3 个节点，实际 %d", n)
	}
}

func TestClosedPolicyHidesEverythingForUngrouped(t *testing.T) {
	e := newTestEnv(t)
	e.seedNodes(2)
	e.mustUser("alice", "secret1", "user")
	if err := e.acl.SetPolicy(PolicyClosed); err != nil {
		t.Fatal(err)
	}
	tok := e.login("alice", "secret1")
	code, body := e.call(http.MethodGet, "/api/v1/nodes", tok, nil)
	if code != http.StatusOK {
		t.Fatalf("code = %d", code)
	}
	if n := len(nodeNames(body)); n != 0 {
		t.Fatalf("closed 策略下未分组账号应看不到节点，实际 %d", n)
	}
	if body["count"].(float64) != 0 {
		t.Fatalf("count 应为 0：%v", body["count"])
	}
	if body["restricted"] != true {
		t.Fatalf("closed 策略下 restricted 应为 true：%v", body)
	}
}

func TestMultiGroupUnionAndDisabledExcluded(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(4)
	g1 := e.mustGroup("A", ids[0], ids[1])
	g2 := e.mustGroup("B", ids[2])
	e.mustUser("alice", "secret1", "user", g1.ID, g2.ID)

	// 把 ids[1] 停用：它在组里，但不能下发
	if _, err := e.nodes.SetEnabled([]string{ids[1]}, false); err != nil {
		t.Fatal(err)
	}
	tok := e.login("alice", "secret1")
	_, body := e.call(http.MethodGet, "/api/v1/nodes", tok, nil)
	got := nodeNames(body)
	if len(got) != 2 || !got["节点1"] || !got["节点3"] {
		t.Fatalf("应为 节点1 + 节点3（并集，且停用的 节点2 不下发），实际 %v", got)
	}
}

// ---- 版本失效（客户端增量拉取的关键） ----

func TestACLChangeInvalidatesClientVersion(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(3)
	g := e.mustGroup("A", ids[0])
	e.mustUser("alice", "secret1", "user", g.ID)
	e.mustUser("root", "secret1", "admin")

	alice := e.login("alice", "secret1")
	_, body := e.call(http.MethodGet, "/api/v1/nodes", alice, nil)
	v1 := int64(body["version"].(float64))

	// 用同一个版本号再问一次：应命中 unchanged
	_, again := e.call(http.MethodGet, fmt.Sprintf("/api/v1/nodes?version=%d", v1), alice, nil)
	if again["unchanged"] != true {
		t.Fatalf("版本没变时应回 unchanged：%v", again)
	}

	// 管理员把另一个节点加进组
	root := e.login("root", "secret1")
	code, r := e.call(http.MethodPost, "/api/v1/admin/groups/"+g.ID+"/nodes", root,
		map[string]any{"ids": []string{ids[0], ids[1]}})
	if code != http.StatusOK {
		t.Fatalf("配置权限组节点 = %d %v", code, r)
	}

	// 老版本号必须失效，并且能看到新增的那条
	code, after := e.call(http.MethodGet, fmt.Sprintf("/api/v1/nodes?version=%d", v1), alice, nil)
	if code != http.StatusOK {
		t.Fatalf("code = %d", code)
	}
	if after["unchanged"] == true {
		t.Fatal("改权限组后旧版本号仍然回 unchanged，客户端会一直拿不到新节点")
	}
	if n := len(nodeNames(after)); n != 2 {
		t.Fatalf("改组后 alice 应看到 2 个节点，实际 %d", n)
	}
}

func TestUserGroupChangeInvalidatesClientVersion(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(2)
	g := e.mustGroup("A", ids[0])
	e.mustUser("alice", "secret1", "user", g.ID)
	e.mustUser("root", "secret1", "admin")

	alice := e.login("alice", "secret1")
	_, body := e.call(http.MethodGet, "/api/v1/nodes", alice, nil)
	v1 := int64(body["version"].(float64))

	root := e.login("root", "secret1")
	code, r := e.call(http.MethodPost, "/api/v1/admin/users/alice/groups", root,
		map[string]any{"groups": []string{g.ID}})
	if code != http.StatusOK {
		t.Fatalf("改归属 = %d %v", code, r)
	}
	if r["node_count"].(float64) != 1 {
		t.Fatalf("改归属后 node_count 应为 1：%v", r)
	}
	_, after := e.call(http.MethodGet, fmt.Sprintf("/api/v1/nodes?version=%d", v1), alice, nil)
	if after["unchanged"] == true {
		t.Fatal("改账号归属后旧版本号仍然回 unchanged")
	}
}

// ---- 鉴权与错误处理 ----

func TestAdminEndpointsRejectNormalUser(t *testing.T) {
	e := newTestEnv(t)
	g := e.mustGroup("A")
	e.mustUser("alice", "secret1", "user", g.ID)
	tok := e.login("alice", "secret1")

	for _, tc := range []struct{ method, path string }{
		{http.MethodGet, "/api/v1/admin/groups"},
		{http.MethodGet, "/api/v1/admin/users"},
		{http.MethodGet, "/api/v1/admin/nodes"},
	} {
		if code, _ := e.call(tc.method, tc.path, tok, nil); code != http.StatusForbidden {
			t.Fatalf("%s %s 普通账号应 403，实际 %d", tc.method, tc.path, code)
		}
	}
	if code, _ := e.call(http.MethodPost, "/api/v1/admin/users/alice/groups", tok,
		map[string]any{"groups": []string{}}); code != http.StatusForbidden {
		t.Fatalf("普通账号给自己改归属应 403，实际 %d", code)
	}
	if code, _ := e.call(http.MethodGet, "/api/v1/nodes", "", nil); code != http.StatusUnauthorized {
		t.Fatalf("无令牌拉节点应 401，实际 %d", code)
	}
}

func TestGroupErrors(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(1)
	e.mustUser("root", "secret1", "admin")
	root := e.login("root", "secret1")

	// 重名
	if code, _ := e.call(http.MethodPost, "/api/v1/admin/groups", root, map[string]any{"name": "A"}); code != http.StatusOK {
		t.Fatalf("建组失败：%d", code)
	}
	if code, body := e.call(http.MethodPost, "/api/v1/admin/groups", root, map[string]any{"name": "a"}); code != http.StatusConflict {
		t.Fatalf("重名（忽略大小写）应 409，实际 %d %v", code, body)
	}
	// 空名
	if code, _ := e.call(http.MethodPost, "/api/v1/admin/groups", root, map[string]any{"name": "  "}); code != http.StatusBadRequest {
		t.Fatalf("空名应 400，实际 %d", code)
	}
	// 未知节点 id
	if code, body := e.call(http.MethodPost, "/api/v1/admin/groups", root,
		map[string]any{"name": "B", "nodes": []string{"nope"}}); code != http.StatusBadRequest {
		t.Fatalf("未知节点 id 应 400，实际 %d %v", code, body)
	}
	// 不存在的组 → 404
	if code, _ := e.call(http.MethodPost, "/api/v1/admin/groups/deadbeef/nodes", root,
		map[string]any{"ids": []string{ids[0]}}); code != http.StatusNotFound {
		t.Fatalf("不存在的组应 404，实际 %d", code)
	}
	// 给账号分配不存在的组 → 400
	e.mustUser("alice", "secret1", "user")
	if code, body := e.call(http.MethodPost, "/api/v1/admin/users/alice/groups", root,
		map[string]any{"groups": []string{"nope"}}); code != http.StatusBadRequest {
		t.Fatalf("分配不存在的组应 400，实际 %d %v", code, body)
	}
	// 策略值非法 → 400
	if code, _ := e.call(http.MethodPost, "/api/v1/admin/acl/policy", root,
		map[string]any{"default_policy": "whatever"}); code != http.StatusBadRequest {
		t.Fatalf("非法策略应 400，实际 %d", code)
	}
	// 方法不对 → 405
	if code, _ := e.call(http.MethodDelete, "/api/v1/admin/groups", root, nil); code != http.StatusMethodNotAllowed {
		t.Fatalf("DELETE /admin/groups 应 405，实际 %d", code)
	}
}

func TestDeleteGroupDetachesUsers(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(2)
	g := e.mustGroup("A", ids[0])
	e.mustUser("alice", "secret1", "user", g.ID)
	e.mustUser("root", "secret1", "admin")
	root := e.login("root", "secret1")

	code, body := e.call(http.MethodDelete, "/api/v1/admin/groups/"+g.ID, root, nil)
	if code != http.StatusOK {
		t.Fatalf("删组 = %d %v", code, body)
	}
	if body["detached_users"].(float64) != 1 {
		t.Fatalf("应摘掉 1 个账号的引用：%v", body)
	}
	if u := e.store.GetUser("alice"); len(u.Groups) != 0 {
		t.Fatalf("删组后 alice 的归属应清空，实际 %v", u.Groups)
	}
	// 删组 + open 策略 → 回到「全量可见」，与升级前一致
	tok := e.login("alice", "secret1")
	_, nb := e.call(http.MethodGet, "/api/v1/nodes", tok, nil)
	if n := len(nodeNames(nb)); n != 2 {
		t.Fatalf("删组后应回到全量 2 个节点，实际 %d", n)
	}
}

func TestDeleteNodePrunesGroup(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(2)
	g := e.mustGroup("A", ids[0], ids[1])
	e.mustUser("root", "secret1", "admin")
	root := e.login("root", "secret1")

	if code, body := e.call(http.MethodDelete, "/api/v1/admin/nodes/"+ids[0], root, nil); code != http.StatusOK {
		t.Fatalf("删节点 = %d %v", code, body)
	}
	g2, ok := e.acl.Get(g.ID)
	if !ok {
		t.Fatal("权限组不该被删")
	}
	if len(g2.Nodes) != 1 || g2.Nodes[0] != ids[1] {
		t.Fatalf("删节点后组内应只剩 1 个，实际 %v", g2.Nodes)
	}
}

func TestLoginAndMeReportScopedCount(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(3)
	g := e.mustGroup("A", ids[0], ids[1])
	e.mustUser("alice", "secret1", "user", g.ID)

	code, body := e.call(http.MethodPost, "/api/v1/auth/login", "",
		map[string]any{"username": "alice", "password": "secret1"})
	if code != http.StatusOK {
		t.Fatalf("login = %d %v", code, body)
	}
	nodes := body["nodes"].(map[string]any)
	if nodes["count"].(float64) != 2 {
		t.Fatalf("登录响应里的 count 应是 2：%v", nodes)
	}
	user := body["user"].(map[string]any)
	groups := user["groups"].([]any)
	if len(groups) != 1 {
		t.Fatalf("登录响应应带权限组名：%v", groups)
	}
	if g0 := groups[0].(map[string]any); g0["name"] != "A" || g0["id"] != g.ID {
		t.Fatalf("权限组信息不对：%v", g0)
	}

	tok := body["token"].(string)
	code, me := e.call(http.MethodGet, "/api/v1/auth/me", tok, nil)
	if code != http.StatusOK {
		t.Fatalf("me = %d", code)
	}
	if me["nodes"].(map[string]any)["count"].(float64) != 2 {
		t.Fatalf("/me 的 count 应是 2：%v", me["nodes"])
	}
}

// 权限组改名不影响鉴权（按 id 关联），但要能正确回带新名字。
func TestRenameGroupKeepsMembership(t *testing.T) {
	e := newTestEnv(t)
	ids := e.seedNodes(1)
	g := e.mustGroup("旧名", ids[0])
	e.mustUser("alice", "secret1", "user", g.ID)
	e.mustUser("root", "secret1", "admin")
	root := e.login("root", "secret1")

	if code, body := e.call(http.MethodPatch, "/api/v1/admin/groups/"+g.ID, root,
		map[string]any{"name": "新名"}); code != http.StatusOK {
		t.Fatalf("改名 = %d %v", code, body)
	}
	alice := e.login("alice", "secret1")
	_, body := e.call(http.MethodGet, "/api/v1/nodes", alice, nil)
	if n := len(nodeNames(body)); n != 1 {
		t.Fatalf("改名后可见节点应仍是 1，实际 %d", n)
	}
}
