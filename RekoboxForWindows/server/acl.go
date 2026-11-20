package main

// 权限组（节点 ACL）：把节点池切成若干个「可访问集合」，账号通过所属权限组
// 拿到属于自己的那部分节点。
//
// 存储：data/acl.json
//   {
//     "default_policy": "open",        // 未分组账号的兜底策略，见下
//     "updated_at": 1794473678040,
//     "groups": [
//       {"id":"a1b2c3d4e5f6","name":"套餐A","description":"",
//        "nodes":["节点id", ...],"created_at":...,"updated_at":...}
//     ]
//   }
//
// 生效规则（Server.nodeScope）：
//   role=admin            → 不受限制，可见全部启用节点（否则进不了管理界面）
//   有权限组              → 各组节点并集 ∩ 启用节点（一个节点可属于多个组）
//   无权限组 + open       → 全部启用节点（**升级前的账号保持原行为**）
//   无权限组 + closed     → 空集合
//
// 「未分组 = 全部可见」是有意为之的兼容默认值：升级前所有账号都没有 groups 字段，
// 直接改成 closed 会让线上所有客户端在下次拉取时清空节点列表。管理员在控制台把
// 策略切成 closed 即完成收紧（不分组 = 无节点）。
//
// 每次权限组/归属变更都会把节点池 version 推一格，这样客户端用 ?version=N 做的
// 增量判断必然失效，会重新拉一次——权限变化不需要客户端改代码就能生效。

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

var (
	// ErrGroupExists 权限组重名
	ErrGroupExists = errors.New("权限组名称已存在")
	// ErrNoGroup 权限组不存在
	ErrNoGroup = errors.New("权限组不存在")
)

// 未分组账号的兜底策略。
const (
	// PolicyOpen 未分组账号可见全部启用节点（默认，兼容旧部署）
	PolicyOpen = "open"
	// PolicyClosed 未分组账号不可见任何节点
	PolicyClosed = "closed"
)

// ACLGroup 是一个权限组：一组节点 id 的集合，外加给管理员看的名称与备注。
type ACLGroup struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Description string   `json:"description,omitempty"`
	Nodes       []string `json:"nodes"`
	CreatedAt   int64    `json:"created_at"`
	UpdatedAt   int64    `json:"updated_at"`
}

func (g *ACLGroup) clone() *ACLGroup {
	cp := *g
	cp.Nodes = append([]string(nil), g.Nodes...)
	if cp.Nodes == nil {
		cp.Nodes = []string{}
	}
	return &cp
}

// ACLStore 是权限组的全量内存副本 + 原子落盘，语义与 Store / NodeStore 一致。
type ACLStore struct {
	mu        sync.RWMutex
	dir       string
	policy    string
	updatedAt int64
	groups    map[string]*ACLGroup
}

func OpenACLStore(dir string) (*ACLStore, error) {
	a := &ACLStore{
		dir:    dir,
		policy: PolicyOpen,
		groups: map[string]*ACLGroup{},
	}
	var wrap struct {
		DefaultPolicy string      `json:"default_policy"`
		UpdatedAt     int64       `json:"updated_at"`
		Groups        []*ACLGroup `json:"groups"`
	}
	// 文件不存在等于「全新安装 / 从无 ACL 的旧版本升级」：保持空表 + open 策略，
	// 不写盘，等第一次真正改动再落盘（避免只读挂载时启动失败）。
	if err := readJSON(a.path(), &wrap); err != nil {
		if !os.IsNotExist(err) {
			return nil, fmt.Errorf("读取权限组失败：%w", err)
		}
		return a, nil
	}
	a.policy = normalizePolicy(wrap.DefaultPolicy)
	a.updatedAt = wrap.UpdatedAt
	for _, g := range wrap.Groups {
		if g == nil || g.ID == "" {
			continue
		}
		if g.Name == "" {
			g.Name = g.ID
		}
		if g.Nodes == nil {
			g.Nodes = []string{}
		}
		a.groups[g.ID] = g
	}
	return a, nil
}

func (a *ACLStore) path() string { return filepath.Join(a.dir, "acl.json") }

func normalizePolicy(p string) string {
	switch strings.ToLower(strings.TrimSpace(p)) {
	case PolicyClosed, "strict", "none", "deny":
		return PolicyClosed
	default:
		return PolicyOpen
	}
}

func (a *ACLStore) saveLocked() error {
	if a.updatedAt == 0 {
		a.updatedAt = time.Now().UnixMilli()
	}
	list := make([]*ACLGroup, 0, len(a.groups))
	for _, g := range a.groups {
		list = append(list, g)
	}
	sortGroupsSlice(list)
	return writeJSONAtomic(a.path(), map[string]any{
		"default_policy": a.policy,
		"updated_at":     a.updatedAt,
		"groups":         list,
	})
}

func sortGroupsSlice(list []*ACLGroup) {
	sort.SliceStable(list, func(i, j int) bool {
		if list[i].CreatedAt != list[j].CreatedAt {
			return list[i].CreatedAt < list[j].CreatedAt
		}
		if list[i].Name != list[j].Name {
			return list[i].Name < list[j].Name
		}
		return list[i].ID < list[j].ID
	})
}

// ---- 读 ----

func (a *ACLStore) Policy() string {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.policy
}

func (a *ACLStore) List() []*ACLGroup {
	a.mu.RLock()
	defer a.mu.RUnlock()
	out := make([]*ACLGroup, 0, len(a.groups))
	for _, g := range a.groups {
		out = append(out, g.clone())
	}
	sortGroupsSlice(out)
	return out
}

func (a *ACLStore) Get(id string) (*ACLGroup, bool) {
	a.mu.RLock()
	defer a.mu.RUnlock()
	g, ok := a.groups[id]
	if !ok {
		return nil, false
	}
	return g.clone(), true
}

// FindByName 给命令行用：管理员记不住 12 位 id，按名称找。
func (a *ACLStore) FindByName(name string) (*ACLGroup, bool) {
	name = strings.TrimSpace(name)
	a.mu.RLock()
	defer a.mu.RUnlock()
	for _, g := range a.groups {
		if strings.EqualFold(g.Name, name) || g.ID == name {
			return g.clone(), true
		}
	}
	return nil, false
}

func (a *ACLStore) Count() int {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return len(a.groups)
}

// Scope 计算一个账号的可见节点集合。
//
// allow == nil 表示「不限制」（可见全部启用节点）；restricted 为 true 时
// 只有 allow 里的节点可见（allow 可能为空 map，即什么都看不到）。
func (a *ACLStore) Scope(userGroups []string) (allow map[string]bool, restricted bool) {
	ids := dedupeStrings(userGroups)
	a.mu.RLock()
	defer a.mu.RUnlock()

	if len(ids) == 0 {
		if a.policy == PolicyClosed {
			return map[string]bool{}, true
		}
		return nil, false
	}

	allow = make(map[string]bool, 16)
	hit := 0
	for _, gid := range ids {
		g, ok := a.groups[gid]
		if !ok {
			continue
		}
		hit++
		for _, n := range g.Nodes {
			allow[n] = true
		}
	}
	if hit == 0 {
		// 引用的组全被删了：按「未分组」处理。正常流程里删组会把账号上的引用
		// 一起摘掉，这里是防御性兜底，行为与未分组保持一致、便于排查。
		if a.policy == PolicyClosed {
			return map[string]bool{}, true
		}
		return nil, false
	}
	return allow, true
}

// ---- 写 ----

func (a *ACLStore) SetPolicy(p string) error {
	p = normalizePolicy(p)
	a.mu.Lock()
	defer a.mu.Unlock()
	old, oldAt := a.policy, a.updatedAt
	a.policy = p
	a.updatedAt = time.Now().UnixMilli()
	if err := a.saveLocked(); err != nil {
		a.policy, a.updatedAt = old, oldAt
		return err
	}
	return nil
}

func (a *ACLStore) Create(name, description string) (*ACLGroup, error) {
	name = strings.TrimSpace(name)
	if err := validateGroupName(name); err != nil {
		return nil, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	for _, g := range a.groups {
		if strings.EqualFold(g.Name, name) {
			return nil, ErrGroupExists
		}
	}
	now := time.Now().UnixMilli()
	g := &ACLGroup{
		ID:          newNodeID(),
		Name:        name,
		Description: strings.TrimSpace(description),
		Nodes:       []string{},
		CreatedAt:   now,
		UpdatedAt:   now,
	}
	a.groups[g.ID] = g
	a.updatedAt = now
	if err := a.saveLocked(); err != nil {
		delete(a.groups, g.ID)
		return nil, err
	}
	return g.clone(), nil
}

// Update 改名称/备注；nil 表示这一项不动。
func (a *ACLStore) Update(id string, name, description *string) (*ACLGroup, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	g, ok := a.groups[id]
	if !ok {
		return nil, ErrNoGroup
	}
	oldName, oldDesc := g.Name, g.Description
	if name != nil {
		n := strings.TrimSpace(*name)
		if err := validateGroupName(n); err != nil {
			return nil, err
		}
		for oid, o := range a.groups {
			if oid != id && strings.EqualFold(o.Name, n) {
				return nil, ErrGroupExists
			}
		}
		g.Name = n
	}
	if description != nil {
		g.Description = strings.TrimSpace(*description)
	}
	g.UpdatedAt = time.Now().UnixMilli()
	a.updatedAt = g.UpdatedAt
	if err := a.saveLocked(); err != nil {
		g.Name, g.Description = oldName, oldDesc
		return nil, err
	}
	return g.clone(), nil
}

// SetNodes 整体替换权限组的节点集合（幂等，重复 id 自动去重）。
func (a *ACLStore) SetNodes(id string, ids []string) (*ACLGroup, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	g, ok := a.groups[id]
	if !ok {
		return nil, ErrNoGroup
	}
	old := g.Nodes
	g.Nodes = dedupeStrings(ids)
	g.UpdatedAt = time.Now().UnixMilli()
	a.updatedAt = g.UpdatedAt
	if err := a.saveLocked(); err != nil {
		g.Nodes = old
		return nil, err
	}
	return g.clone(), nil
}

func (a *ACLStore) Delete(id string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	g, ok := a.groups[id]
	if !ok {
		return ErrNoGroup
	}
	delete(a.groups, id)
	a.updatedAt = time.Now().UnixMilli()
	if err := a.saveLocked(); err != nil {
		a.groups[id] = g
		return err
	}
	return nil
}

// PruneNodes 在节点被删除后清理引用，返回受影响的权限组数。
// 不清理的话，被删节点的 id 会永远留在组里，重新录入同名节点也不会自动恢复。
func (a *ACLStore) PruneNodes(removed []string) (int, error) {
	if len(removed) == 0 {
		return 0, nil
	}
	gone := make(map[string]bool, len(removed))
	for _, id := range removed {
		gone[id] = true
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	changed := 0
	snapshot := make(map[string][]string, len(a.groups))
	for id, g := range a.groups {
		if len(g.Nodes) == 0 {
			continue
		}
		kept := make([]string, 0, len(g.Nodes))
		for _, n := range g.Nodes {
			if gone[n] {
				continue
			}
			kept = append(kept, n)
		}
		if len(kept) == len(g.Nodes) {
			continue
		}
		snapshot[id] = g.Nodes
		g.Nodes = kept
		g.UpdatedAt = time.Now().UnixMilli()
		changed++
	}
	if changed == 0 {
		return 0, nil
	}
	a.updatedAt = time.Now().UnixMilli()
	if err := a.saveLocked(); err != nil {
		for id, old := range snapshot {
			if g, ok := a.groups[id]; ok {
				g.Nodes = old
			}
		}
		return 0, err
	}
	return changed, nil
}

// ---- 校验 ----

func validateGroupName(name string) error {
	if name == "" {
		return errors.New("权限组名称不能为空")
	}
	if utf8.RuneCountInString(name) > 32 {
		return errors.New("权限组名称最长 32 个字符")
	}
	if strings.ContainsAny(name, "\r\n\t") {
		return errors.New("权限组名称不能包含换行或制表符")
	}
	return nil
}

func dedupeStrings(in []string) []string {
	out := make([]string, 0, len(in))
	seen := make(map[string]bool, len(in))
	for _, s := range in {
		s = strings.TrimSpace(s)
		if s == "" || seen[s] {
			continue
		}
		seen[s] = true
		out = append(out, s)
	}
	return out
}

// groupNames 把权限组 id 翻成 {id,name} 给客户端/前端展示。
// 认不出的 id 也原样返回（name 留空），方便排查历史脏数据。
func groupNames(a *ACLStore, ids []string) []map[string]string {
	out := make([]map[string]string, 0, len(ids))
	for _, id := range dedupeStrings(ids) {
		item := map[string]string{"id": id, "name": ""}
		if g, ok := a.Get(id); ok {
			item["name"] = g.Name
		}
		out = append(out, item)
	}
	return out
}
