package main

// 节点池：管理员在网页里录入分享链接，客户端登录后只读拉取。
//
// 存储：data/nodes.json
//   {
//     "version": 12,              // 全局单调递增，客户端用它判断是否需要重拉
//     "updated_at": 1794473678040,
//     "nodes": [ {...}, ... ]
//   }
//
// 设计取舍：服务端只做「校验 + 摘要」的最小解析（类型 / 地址 / 端口 / 备注名），
// 真正的协议参数由客户端用现成的链接解析器还原，这样加新协议不用改服务端。

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	// ErrNodeExists 同一链接重复录入
	ErrNodeExists = errors.New("节点链接已存在")
	// ErrNoNode 节点不存在
	ErrNoNode = errors.New("节点不存在")
)

// Node 是节点池里的一条记录。Link 是唯一事实来源，其余字段都是它的摘要，
// 便于网页展示、排序和客户端识别。
type Node struct {
	ID        string `json:"id"`
	Link      string `json:"link"`
	Name      string `json:"name"`
	Type      string `json:"type"`
	Addr      string `json:"addr"`
	Port      int    `json:"port"`
	Group     string `json:"group"`
	Remark    string `json:"remark"`
	Enabled   bool   `json:"enabled"`
	Order     int    `json:"order"`
	CreatedAt int64  `json:"created_at"`
	UpdatedAt int64  `json:"updated_at"`
}

// NodeSummary 是给客户端看的字段集合：不含 enabled/order/备注以外的管理信息。
type NodeSummary struct {
	ID     string `json:"id"`
	Link   string `json:"link"`
	Name   string `json:"name"`
	Type   string `json:"type"`
	Addr   string `json:"addr"`
	Port   int    `json:"port"`
	Group  string `json:"group"`
	Remark string `json:"remark"`
}

type NodeStore struct {
	mu        sync.RWMutex
	dir       string
	version   int64
	updatedAt int64
	nodes     []*Node
}

func OpenNodeStore(dir string) (*NodeStore, error) {
	s := &NodeStore{dir: dir}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	// 注意：不能直接把 NodeStore 交给 json.Unmarshal——它的字段是小写（不导出），
	// 解码器会静默跳过，导致每次重启节点池都变空。
	var wrap struct {
		Version   int64   `json:"version"`
		UpdatedAt int64   `json:"updated_at"`
		Nodes     []*Node `json:"nodes"`
	}
	if err := readJSON(s.path(), &wrap); err != nil && !os.IsNotExist(err) {
		return nil, fmt.Errorf("读取节点池失败：%w", err)
	}
	s.version = wrap.Version
	s.updatedAt = wrap.UpdatedAt
	s.nodes = wrap.Nodes

	// 老数据缺字段时补默认值，避免前端显示空白
	for i, n := range s.nodes {
		if n == nil {
			continue
		}
		if n.ID == "" {
			n.ID = newNodeID()
		}
		if n.Order == 0 {
			n.Order = i + 1
		}
	}
	sortNodes(s.nodes)
	return s, nil
}

func (s *NodeStore) path() string { return filepath.Join(s.dir, "nodes.json") }

func (s *NodeStore) saveLocked() error {
	return writeJSONAtomic(s.path(), map[string]any{
		"version":    s.version,
		"updated_at": s.updatedAt,
		"nodes":      s.nodes,
	})
}

func (s *NodeStore) touchLocked() {
	s.version++
	s.updatedAt = time.Now().UnixMilli()
}

// ---- 读 ----

func (s *NodeStore) Version() (int64, int64, int) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.version, s.updatedAt, len(s.nodes)
}

// List 返回启用中的节点摘要，按 Order 排序。
func (s *NodeStore) List() []NodeSummary {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]NodeSummary, 0, len(s.nodes))
	for _, n := range s.nodes {
		if !n.Enabled {
			continue
		}
		out = append(out, NodeSummary{
			ID: n.ID, Link: n.Link, Name: n.Name, Type: n.Type,
			Addr: n.Addr, Port: n.Port, Group: n.Group, Remark: n.Remark,
		})
	}
	return out
}

// ListAll 是管理端视图：包含禁用项与时间戳。
func (s *NodeStore) ListAll() []Node {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]Node, 0, len(s.nodes))
	for _, n := range s.nodes {
		out = append(out, *n)
	}
	return out
}

// ---- 写 ----

type linkReject struct {
	Link   string `json:"link"`
	Reason string `json:"reason"`
}

// AddLinks 批量录入分享链接。单条失败不影响其他条，失败原因逐条返回。
func (s *NodeStore) AddLinks(rawLinks []string, group, remark string) ([]Node, []linkReject, error) {
	type pending struct {
		info linkInfo
		link string
		name string
	}
	var batch []pending
	var rejects []linkReject
	group = strings.TrimSpace(group)
	remark = strings.TrimSpace(remark)

	s.mu.RLock()
	existing := make(map[string]bool, len(s.nodes))
	nameCount := make(map[string]int, len(s.nodes))
	for _, n := range s.nodes {
		existing[normLink(n.Link)] = true
		nameCount[n.Name]++
	}
	s.mu.RUnlock()

	seen := map[string]bool{}
	for _, raw := range rawLinks {
		link := strings.TrimSpace(raw)
		if link == "" {
			continue
		}
		key := normLink(link)
		if existing[key] || seen[key] {
			rejects = append(rejects, linkReject{Link: link, Reason: "该链接已在节点池中"})
			continue
		}
		info, err := parseNodeLink(link)
		if err != nil {
			rejects = append(rejects, linkReject{Link: link, Reason: err.Error()})
			continue
		}
		name := info.Name
		if name == "" {
			name = fmt.Sprintf("%s-%s:%d", info.Type, info.Addr, info.Port)
		}
		// 同名太常见（一堆「香港01」），加序号保证列表可区分
		if nameCount[name] > 0 {
			name = fmt.Sprintf("%s (%d)", name, nameCount[name]+1)
		}
		nameCount[name]++
		seen[key] = true
		batch = append(batch, pending{info: info, link: link, name: name})
	}
	if len(batch) == 0 {
		return nil, rejects, nil
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now().UnixMilli()
	maxOrder := 0
	for _, n := range s.nodes {
		if n.Order > maxOrder {
			maxOrder = n.Order
		}
	}
	added := make([]Node, 0, len(batch))
	for _, p := range batch {
		maxOrder++
		n := &Node{
			ID: newNodeID(), Link: p.link, Name: p.name,
			Type: p.info.Type, Addr: p.info.Addr, Port: p.info.Port,
			Group: group, Remark: remark, Enabled: true, Order: maxOrder,
			CreatedAt: now, UpdatedAt: now,
		}
		s.nodes = append(s.nodes, n)
		added = append(added, *n)
	}
	s.touchLocked()
	if err := s.saveLocked(); err != nil {
		return nil, rejects, err
	}
	return added, rejects, nil
}

// NodePatch 的指针字段表示「这次要改」，nil 表示保持原值。
type NodePatch struct {
	Name    *string `json:"name"`
	Group   *string `json:"group"`
	Remark  *string `json:"remark"`
	Enabled *bool   `json:"enabled"`
	Link    *string `json:"link"`
}

func (s *NodeStore) Update(id string, p NodePatch) (*Node, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := s.findLocked(id)
	if n == nil {
		return nil, ErrNoNode
	}
	if p.Link != nil {
		link := strings.TrimSpace(*p.Link)
		if link == "" {
			return nil, errors.New("链接不能为空")
		}
		info, err := parseNodeLink(link)
		if err != nil {
			return nil, err
		}
		for _, other := range s.nodes {
			if other.ID != id && normLink(other.Link) == normLink(link) {
				return nil, ErrNodeExists
			}
		}
		n.Link = link
		n.Type = info.Type
		n.Addr = info.Addr
		n.Port = info.Port
		if info.Name != "" && p.Name == nil {
			n.Name = info.Name
		}
	}
	if p.Name != nil && strings.TrimSpace(*p.Name) != "" {
		n.Name = strings.TrimSpace(*p.Name)
	}
	if p.Group != nil {
		n.Group = strings.TrimSpace(*p.Group)
	}
	if p.Remark != nil {
		n.Remark = strings.TrimSpace(*p.Remark)
	}
	if p.Enabled != nil {
		n.Enabled = *p.Enabled
	}
	n.UpdatedAt = time.Now().UnixMilli()
	s.touchLocked()
	if err := s.saveLocked(); err != nil {
		return nil, err
	}
	cp := *n
	return &cp, nil
}

func (s *NodeStore) SetEnabled(ids []string, enabled bool) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	hit := 0
	for _, id := range ids {
		if n := s.findLocked(id); n != nil {
			if n.Enabled != enabled {
				n.Enabled = enabled
				n.UpdatedAt = time.Now().UnixMilli()
				hit++
			}
		}
	}
	if hit == 0 {
		return 0, nil
	}
	s.touchLocked()
	return hit, s.saveLocked()
}

func (s *NodeStore) Delete(ids []string) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	want := make(map[string]bool, len(ids))
	for _, id := range ids {
		want[id] = true
	}
	kept := s.nodes[:0]
	removed := 0
	for _, n := range s.nodes {
		if want[n.ID] {
			removed++
			continue
		}
		kept = append(kept, n)
	}
	if removed == 0 {
		return 0, nil
	}
	s.nodes = kept
	s.touchLocked()
	return removed, s.saveLocked()
}

// Reorder 按传入的 id 顺序重排，未列出的节点顺延到末尾。
func (s *NodeStore) Reorder(ids []string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	pos := make(map[string]int, len(ids))
	for i, id := range ids {
		pos[id] = i + 1
	}
	next := len(ids)
	for _, n := range s.nodes {
		if p, ok := pos[n.ID]; ok {
			n.Order = p
			continue
		}
		next++
		n.Order = next
	}
	sortNodes(s.nodes)
	s.touchLocked()
	return s.saveLocked()
}

func (s *NodeStore) findLocked(id string) *Node {
	for _, n := range s.nodes {
		if n.ID == id {
			return n
		}
	}
	return nil
}

func sortNodes(nodes []*Node) {
	sort.SliceStable(nodes, func(i, j int) bool {
		if nodes[i].Order != nodes[j].Order {
			return nodes[i].Order < nodes[j].Order
		}
		return nodes[i].CreatedAt < nodes[j].CreatedAt
	})
}

func newNodeID() string {
	b := make([]byte, 6)
	if _, err := rand.Read(b); err != nil {
		return strconv.FormatInt(time.Now().UnixNano(), 36)
	}
	return hex.EncodeToString(b) // 12 位小写十六进制
}

// normLink 用于去重：忽略首尾空白与 fragment 之外的大小写差异并不做过度归一化，
// 只折叠空白，避免把「真不同的节点」误判成重复。
func normLink(link string) string {
	return strings.TrimSpace(link)
}

// ---- 分享链接解析 ----

type linkInfo struct {
	Type string
	Name string
	Addr string
	Port int
}

// 只放行客户端真能导入的协议；宁可在录入时拒绝，也不要下发一条客户端解析不了的节点。
var linkSchemes = map[string]string{
	"vmess":       "vmess",
	"vless":       "vless",
	"trojan":      "trojan",
	"ss":          "shadowsocks",
	"socks":       "socks",
	"socks5":      "socks",
	"socks4":      "socks",
	"socks4a":     "socks",
	"http":        "http",
	"https":       "http",
	"tuic":        "tuic",
	"hysteria2":   "hysteria2",
	"hy2":         "hysteria2",
	"naive+https": "naive",
	"naive+quic":  "naive",
}

func parseNodeLink(link string) (linkInfo, error) {
	var info linkInfo
	if strings.ContainsAny(link, "\r\n\t ") {
		return info, errors.New("链接里不能有空白字符")
	}
	idx := strings.Index(link, "://")
	if idx <= 0 {
		return info, errors.New("不是合法的分享链接（缺少 scheme://）")
	}
	scheme := strings.ToLower(link[:idx])
	kind, ok := linkSchemes[scheme]
	if !ok {
		return info, fmt.Errorf("不支持的协议 %q（可用：vmess / vless / trojan / ss / socks / http / tuic / hysteria2 / naive）", scheme)
	}
	info.Type = kind

	if scheme == "vmess" {
		return parseVMess(link)
	}

	u, err := url.Parse(link)
	if err == nil && u.Hostname() != "" {
		info.Addr = u.Hostname()
		if p, err := strconv.Atoi(u.Port()); err == nil {
			info.Port = p
		}
		info.Name = strings.TrimSpace(u.Fragment)
		if info.Port == 0 {
			info.Port = defaultPort(scheme)
		}
		return info, nil
	}

	// ss://<base64(method:pass@host:port)>#name 这种老格式 url.Parse 拿不到 host
	body := link[idx+3:]
	if h := strings.Index(body, "#"); h >= 0 {
		info.Name = strings.TrimSpace(percentDecode(body[h+1:]))
		body = body[:h]
	}
	if q := strings.Index(body, "?"); q >= 0 {
		body = body[:q]
	}
	if at := strings.LastIndex(body, "@"); at >= 0 {
		body = body[at+1:]
	} else if decoded, ok := b64Decode(body); ok {
		if at := strings.LastIndex(decoded, "@"); at >= 0 {
			body = decoded[at+1:]
		}
	}
	host, port, ok := splitHostPort(body)
	if !ok {
		return info, errors.New("解析不出服务器地址与端口")
	}
	info.Addr, info.Port = host, port
	return info, nil
}

func parseVMess(link string) (linkInfo, error) {
	var info linkInfo
	info.Type = "vmess"
	body := strings.TrimPrefix(link, "vmess://")
	if h := strings.Index(body, "#"); h >= 0 {
		body = body[:h]
	}
	decoded, ok := b64Decode(body)
	if !ok {
		return info, errors.New("vmess 链接的 base64 解不开")
	}
	var raw struct {
		PS   string `json:"ps"`
		Add  string `json:"add"`
		Port any    `json:"port"`
	}
	if err := json.Unmarshal([]byte(decoded), &raw); err != nil {
		return info, errors.New("vmess 链接不是合法的 JSON")
	}
	info.Name = strings.TrimSpace(raw.PS)
	info.Addr = strings.TrimSpace(raw.Add)
	info.Port = anyToInt(raw.Port)
	if info.Addr == "" || info.Port <= 0 {
		return info, errors.New("vmess 链接缺少 add/port")
	}
	return info, nil
}

func anyToInt(v any) int {
	switch x := v.(type) {
	case float64:
		return int(x)
	case string:
		n, _ := strconv.Atoi(strings.TrimSpace(x))
		return n
	}
	return 0
}

func defaultPort(scheme string) int {
	switch scheme {
	case "http", "https", "trojan", "vless", "vmess", "tuic", "naive+https", "naive+quic":
		return 443
	case "socks", "socks5", "socks4", "socks4a":
		return 1080
	}
	return 443
}

func splitHostPort(s string) (string, int, bool) {
	s = strings.TrimSpace(strings.TrimSuffix(s, "/"))
	if s == "" {
		return "", 0, false
	}
	// [v6]:port
	if strings.HasPrefix(s, "[") {
		if end := strings.Index(s, "]"); end > 0 {
			host := s[1:end]
			rest := strings.TrimPrefix(s[end+1:], ":")
			port, _ := strconv.Atoi(rest)
			return host, port, host != ""
		}
	}
	if i := strings.LastIndex(s, ":"); i > 0 {
		host := s[:i]
		port, err := strconv.Atoi(s[i+1:])
		if err != nil {
			return "", 0, false
		}
		return host, port, host != ""
	}
	return s, 0, false
}

func b64Decode(s string) (string, bool) {
	s = strings.TrimSpace(s)
	candidates := []*base64.Encoding{
		base64.RawURLEncoding, base64.URLEncoding,
		base64.RawStdEncoding, base64.StdEncoding,
	}
	for _, enc := range candidates {
		if b, err := enc.DecodeString(s); err == nil && len(b) > 0 {
			return string(b), true
		}
	}
	return "", false
}

func percentDecode(s string) string {
	if v, err := url.PathUnescape(s); err == nil {
		return v
	}
	return s
}
