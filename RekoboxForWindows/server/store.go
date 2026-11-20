package main

// 文件型存储：一台 VPS、一个进程、几百 KB 数据，用不着数据库。
//
// 目录结构（-data 指定，默认 ./data）：
//   users.json            用户表（口令哈希 + 所属权限组）
//   tokens.json           令牌表（键为令牌 SHA-256）
//   acl.json              权限组 + 未分组账号的兜底策略（见 acl.go）
//   vaults/<user>.json    旧版按账号快照（已废弃，不再读写）
//
// 所有写入都走「临时文件 + rename」，断电最多丢最后一次写，不会写出半截 JSON。

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

var (
	// ErrUserExists 账号已存在
	ErrUserExists = errors.New("user already exists")
	// ErrNoUser 账号不存在
	ErrNoUser = errors.New("user not found")
	// ErrDisabled 账号已禁用
	ErrDisabled = errors.New("user disabled")
	// ErrBadPassword 口令错误
	ErrBadPassword = errors.New("bad password")
	// ErrVersionConflict 客户端基线版本落后，需要重拉合并
	ErrVersionConflict = errors.New("version conflict")
)

type User struct {
	Name      string `json:"name"`
	Password  string `json:"password"`
	CreatedAt int64  `json:"created_at"`
	Disabled  bool   `json:"disabled,omitempty"`
	// Role 只有 admin / user 两种：
	//   admin 可登录网页控制台管理节点与账号
	//   user  只能用客户端拉取节点（只读，不能上传）
	Role string `json:"role,omitempty"`
	// Groups 是账号所属权限组的 id 列表（见 acl.go）。
	// 空表示「未分组」，行为由 acl.json 的 default_policy 决定：
	//   open   → 可见全部启用节点（旧版本账号升级后的默认状态）
	//   closed → 什么都看不到
	// 这是唯一新增的字段；旧 users.json 没有它，反序列化后就是 nil，语义等价于未分组。
	Groups []string `json:"groups,omitempty"`
}

// cloneUser 做浅拷贝 + Groups 深拷贝，避免调用方改到内部状态。
func cloneUser(u *User) *User {
	cp := *u
	cp.Password = ""
	cp.Groups = append([]string(nil), u.Groups...)
	return &cp
}

// RoleAdmin 是网页控制台的准入角色。
const RoleAdmin = "admin"

func (u *User) isAdmin() bool { return u.Role == RoleAdmin }

type Session struct {
	User      string `json:"user"`
	CreatedAt int64  `json:"created_at"`
	LastUsed  int64  `json:"last_used,omitempty"`
	Agent     string `json:"agent,omitempty"`
}

// Vault 是「一个账号的节点数据快照」。服务端不解析 payload 内部结构，
// 只维护单调递增的 version 做乐观锁。
type Vault struct {
	User      string          `json:"user"`
	Version   int64           `json:"version"`
	UpdatedAt int64           `json:"updated_at"`
	Device    string          `json:"device"`
	Payload   json.RawMessage `json:"payload"`
}

type Store struct {
	mu     sync.RWMutex
	dir    string
	users  map[string]*User
	tokens map[string]*Session
	vaults map[string]*Vault
}

func OpenStore(dir string) (*Store, error) {
	s := &Store{
		dir:    dir,
		users:  map[string]*User{},
		tokens: map[string]*Session{},
		vaults: map[string]*Vault{},
	}
	if err := os.MkdirAll(filepath.Join(dir, "vaults"), 0o700); err != nil {
		return nil, err
	}
	if err := s.loadUsers(); err != nil {
		return nil, err
	}
	if err := s.loadTokens(); err != nil {
		return nil, err
	}
	if err := s.loadVaults(); err != nil {
		return nil, err
	}
	return s, nil
}

func (s *Store) loadUsers() error {
	var wrap struct {
		Users map[string]*User `json:"users"`
	}
	if err := readJSON(filepath.Join(s.dir, "users.json"), &wrap); err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	if wrap.Users != nil {
		s.users = wrap.Users
	}
	return nil
}

func (s *Store) loadTokens() error {
	var wrap struct {
		Tokens map[string]*Session `json:"tokens"`
	}
	if err := readJSON(filepath.Join(s.dir, "tokens.json"), &wrap); err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	if wrap.Tokens != nil {
		s.tokens = wrap.Tokens
	}
	return nil
}

func (s *Store) loadVaults() error {
	matches, err := filepath.Glob(filepath.Join(s.dir, "vaults", "*.json"))
	if err != nil {
		return err
	}
	for _, m := range matches {
		var v Vault
		if err := readJSON(m, &v); err != nil {
			return fmt.Errorf("read %s: %w", m, err)
		}
		if v.User == "" {
			v.User = trimJSONName(m)
		}
		cp := v
		s.vaults[v.User] = &cp
	}
	return nil
}

func trimJSONName(path string) string {
	base := filepath.Base(path)
	return base[:len(base)-len(".json")]
}

func (s *Store) saveUsersLocked() error {
	return writeJSONAtomic(filepath.Join(s.dir, "users.json"), map[string]any{"users": s.users})
}

func (s *Store) saveTokensLocked() error {
	return writeJSONAtomic(filepath.Join(s.dir, "tokens.json"), map[string]any{"tokens": s.tokens})
}

func (s *Store) saveVaultLocked(v *Vault) error {
	return writeJSONAtomic(filepath.Join(s.dir, "vaults", v.User+".json"), v)
}

// ---- 用户管理（只在服务端 CLI 调用，不暴露 HTTP 注册接口）----

// CreateUser 建号。groups 为变参：不传等于未分组，老调用点无需改动。
func (s *Store) CreateUser(name, password, role string, groups ...string) (*User, error) {
	if !validUserName(name) {
		return nil, fmt.Errorf("非法用户名 %q：只允许小写字母、数字与 _-.，2-32 位且不以符号开头", name)
	}
	if len(password) < 6 {
		return nil, fmt.Errorf("口令至少 6 位")
	}
	if role == "" {
		role = "user"
	}
	if role != "user" && role != RoleAdmin {
		return nil, fmt.Errorf("角色只能是 user 或 admin")
	}
	hash, err := hashPassword(password)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.users[name]; ok {
		return nil, ErrUserExists
	}
	u := &User{
		Name:      name,
		Password:  hash,
		CreatedAt: time.Now().UnixMilli(),
		Role:      role,
		Groups:    dedupeStrings(groups),
	}
	s.users[name] = u
	if err := s.saveUsersLocked(); err != nil {
		delete(s.users, name)
		return nil, err
	}
	return cloneUser(u), nil
}

// SetUserGroups 整体替换账号的权限组归属。
// 调用方负责先校验这些组是否存在（见 admin.go 的 adminSetUserGroups）。
func (s *Store) SetUserGroups(name string, groups []string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	u, ok := s.users[name]
	if !ok {
		return ErrNoUser
	}
	old := u.Groups
	u.Groups = dedupeStrings(groups)
	if err := s.saveUsersLocked(); err != nil {
		u.Groups = old
		return err
	}
	return nil
}

// RemoveGroupFromUsers 在权限组被删除时摘掉账号上的引用，返回受影响的账号数。
// 不摘的话，账号会永久保留一个指向空组的 id，排查起来非常费劲。
func (s *Store) RemoveGroupFromUsers(groupID string) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	snapshot := make(map[string][]string)
	for name, u := range s.users {
		if len(u.Groups) == 0 {
			continue
		}
		kept := make([]string, 0, len(u.Groups))
		for _, g := range u.Groups {
			if g == groupID {
				continue
			}
			kept = append(kept, g)
		}
		if len(kept) == len(u.Groups) {
			continue
		}
		snapshot[name] = u.Groups
		u.Groups = kept
	}
	if len(snapshot) == 0 {
		return 0, nil
	}
	if err := s.saveUsersLocked(); err != nil {
		for name, old := range snapshot {
			if u, ok := s.users[name]; ok {
				u.Groups = old
			}
		}
		return 0, err
	}
	return len(snapshot), nil
}

// UngroupedUsers 返回未分配权限组的账号名（不含管理员），供控制台提示用。
func (s *Store) UngroupedUsers() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]string, 0)
	for name, u := range s.users {
		if u.isAdmin() || len(u.Groups) != 0 {
			continue
		}
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// SetDisabled 禁用/启用账号。禁用会顺手吊销该账号的全部令牌，
// 否则已经登录的设备仍能继续拉节点。
func (s *Store) SetDisabled(name string, disabled bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	u, ok := s.users[name]
	if !ok {
		return ErrNoUser
	}
	u.Disabled = disabled
	if err := s.saveUsersLocked(); err != nil {
		return err
	}
	if disabled {
		for id, sess := range s.tokens {
			if sess.User == name {
				delete(s.tokens, id)
			}
		}
		return s.saveTokensLocked()
	}
	return nil
}

func (s *Store) SetPassword(name, password string) error {
	if len(password) < 6 {
		return fmt.Errorf("口令至少 6 位")
	}
	hash, err := hashPassword(password)
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	u, ok := s.users[name]
	if !ok {
		return ErrNoUser
	}
	old := u.Password
	u.Password = hash
	if err := s.saveUsersLocked(); err != nil {
		u.Password = old
		return err
	}
	// 改口令后踢掉旧令牌，避免旧设备继续同步
	for id, sess := range s.tokens {
		if sess.User == name {
			delete(s.tokens, id)
		}
	}
	return s.saveTokensLocked()
}

func (s *Store) DeleteUser(name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.users[name]; !ok {
		return ErrNoUser
	}
	delete(s.users, name)
	for id, sess := range s.tokens {
		if sess.User == name {
			delete(s.tokens, id)
		}
	}
	delete(s.vaults, name)
	if err := s.saveUsersLocked(); err != nil {
		return err
	}
	if err := s.saveTokensLocked(); err != nil {
		return err
	}
	_ = os.Remove(filepath.Join(s.dir, "vaults", name+".json"))
	return nil
}

func (s *Store) ListUsers() []*User {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*User, 0, len(s.users))
	for _, u := range s.users {
		out = append(out, cloneUser(u))
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// GetUser 返回账号副本（不含口令哈希）；不存在返回 nil。
func (s *Store) GetUser(name string) *User {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if u, ok := s.users[name]; ok {
		return cloneUser(u)
	}
	return nil
}

// ---- 登录 / 令牌 ----

func (s *Store) Authenticate(name, password, agent string) (string, *User, error) {
	s.mu.RLock()
	u, ok := s.users[name]
	var hash string
	disabled := false
	if ok {
		hash = u.Password
		disabled = u.Disabled
	}
	s.mu.RUnlock()

	// 账号不存在也跑一次哈希，避免用响应时间探测账号是否存在
	if !ok {
		verifyPassword("pbkdf2_sha256$210000$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", password)
		return "", nil, ErrBadPassword
	}
	if !verifyPassword(hash, password) {
		return "", nil, ErrBadPassword
	}
	if disabled {
		return "", nil, ErrDisabled
	}

	token, err := newToken()
	if err != nil {
		return "", nil, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now().UnixMilli()
	s.tokens[tokenID(token)] = &Session{User: name, CreatedAt: now, LastUsed: now, Agent: agent}
	if err := s.saveTokensLocked(); err != nil {
		delete(s.tokens, tokenID(token))
		return "", nil, err
	}
	return token, cloneUser(u), nil
}

func (s *Store) TokenUser(token string) (*User, bool) {
	if token == "" {
		return nil, false
	}
	id := tokenID(token)
	s.mu.RLock()
	sess, ok := s.tokens[id]
	var u *User
	if ok {
		if uu, exist := s.users[sess.User]; exist {
			u = cloneUser(uu)
		}
	}
	s.mu.RUnlock()
	if !ok || u == nil || u.Disabled {
		return nil, false
	}
	// last_used 只更新内存，不每次写盘
	s.mu.Lock()
	if sess2, ok2 := s.tokens[id]; ok2 {
		sess2.LastUsed = time.Now().UnixMilli()
	}
	s.mu.Unlock()
	return u, true
}

func (s *Store) RevokeToken(token string) {
	if token == "" {
		return
	}
	id := tokenID(token)
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.tokens[id]; !ok {
		return
	}
	delete(s.tokens, id)
	_ = s.saveTokensLocked()
}

// SessionStat 给管理界面用：在线设备数（未撤销的令牌个数）与最后活动时间。
func (s *Store) SessionStat(user string) (count int, lastUsed int64) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, sess := range s.tokens {
		if sess.User != user {
			continue
		}
		count++
		if sess.LastUsed > lastUsed {
			lastUsed = sess.LastUsed
		}
	}
	return count, lastUsed
}

// ---- 节点快照 ----

func (s *Store) GetVault(user string) (*Vault, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	v, ok := s.vaults[user]
	if !ok {
		return nil, false
	}
	cp := *v
	cp.Payload = append(json.RawMessage(nil), v.Payload...)
	return &cp, true
}

// PutVault 做乐观锁：baseVersion 必须等于当前版本，否则返回 ErrVersionConflict
// 和当前快照，由客户端合并后重试。
func (s *Store) PutVault(user string, baseVersion int64, device string, payload json.RawMessage) (*Vault, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	cur, exists := s.vaults[user]
	var curVersion int64
	if exists {
		curVersion = cur.Version
	}
	if baseVersion != curVersion {
		return cur, ErrVersionConflict
	}

	next := &Vault{
		User:      user,
		Version:   curVersion + 1,
		UpdatedAt: time.Now().UnixMilli(),
		Device:    device,
		Payload:   append(json.RawMessage(nil), payload...),
	}
	s.vaults[user] = next
	if err := s.saveVaultLocked(next); err != nil {
		if exists {
			s.vaults[user] = cur
		} else {
			delete(s.vaults, user)
		}
		return nil, err
	}
	cp := *next
	return &cp, nil
}

// ---- 文件读写 ----

func readJSON(path string, out any) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(b) == 0 {
		return nil
	}
	return json.Unmarshal(b, out)
}

func writeJSONAtomic(path string, v any) error {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".tmp-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if _, err := tmp.Write(b); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(name, 0o600); err != nil {
		return err
	}
	return os.Rename(name, path)
}
