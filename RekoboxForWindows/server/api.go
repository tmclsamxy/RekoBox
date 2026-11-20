package main

// HTTP 接口层。所有响应都是 JSON，错误统一为：
//   {"error":{"code":"...","message":"..."}}
//
// 客户端接口（任意已登录账号）：
//   GET  /api/v1/health          探活（不需要鉴权）
//   POST /api/v1/auth/login      登录，发永久令牌
//   POST /api/v1/auth/logout     注销，吊销当前令牌
//   GET  /api/v1/auth/me         校验令牌 + 回带账号角色
//   GET  /api/v1/nodes           拉取节点池（只读；禁用节点不下发）
//
// 管理端接口（role=admin）在 admin.go；网页控制台挂在 /admin。
//
// 只读约束：节点池由管理员统一维护，客户端没有任何写入入口。
// 旧的 /api/v1/vault（双向快照）保留 GET 兼容，PUT 直接 403。
//
// 可见性：GET /api/v1/nodes 不再是「全量节点池」，而是「该账号权限组允许的节点」，
// 解析逻辑集中在 nodeScope / visibleNodes 一处，避免各处各写一遍过滤条件。

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Server struct {
	store   *Store
	nodes   *NodeStore
	acl     *ACLStore
	limiter *loginLimiter
	started time.Time
	version string
}

func NewServer(store *Store, nodes *NodeStore, acl *ACLStore, version string) *Server {
	return &Server{
		store:   store,
		nodes:   nodes,
		acl:     acl,
		limiter: newLoginLimiter(10, 10*time.Minute),
		started: time.Now(),
		version: version,
	}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/health", s.withLog(s.handleHealth))
	mux.HandleFunc("/api/v1/auth/login", s.withLog(s.handleLogin))
	mux.HandleFunc("/api/v1/auth/logout", s.withLog(s.handleLogout))
	mux.HandleFunc("/api/v1/auth/me", s.withLog(s.handleMe))
	mux.HandleFunc("/api/v1/nodes", s.withLog(s.handleNodes))
	mux.HandleFunc("/api/v1/vault", s.withLog(s.handleVaultLegacy))
	mux.HandleFunc("/api/v1/admin/", s.withLog(s.handleAdmin))
	mux.HandleFunc("/admin", s.serveAdminPage)
	mux.HandleFunc("/admin/", s.serveAdminPage)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/" {
			// 根路径直接跳控制台，省得管理员记路径
			http.Redirect(w, r, "/admin", http.StatusFound)
			return
		}
		writeErr(w, http.StatusNotFound, "not_found", "没有这个接口："+r.URL.Path)
	})
	return mux
}

// ---- 中间件 ----

func (s *Server) withLog(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		if maxBodyBytes > 0 && r.Body != nil {
			// 超限时 MaxBytesReader 让后续 Read 返回错误，解码环节统一报 400
			r.Body = http.MaxBytesReader(w, r.Body, maxBodyBytes)
		}
		rec := &statusRecorder{ResponseWriter: w, status: 200}
		next(rec, r)
		log.Printf("%s %s %d %s %s", r.Method, r.URL.Path, rec.status,
			time.Since(start).Round(time.Millisecond), clientIP(r))
	}
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

// bearerToken 从 Authorization 头取令牌；没有则返回空串。
func bearerToken(r *http.Request) string {
	h := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if len(h) > len(prefix) && strings.EqualFold(h[:len(prefix)], prefix) {
		return strings.TrimSpace(h[len(prefix):])
	}
	// 兼容把令牌直接放 X-Auth-Token 的调用方式（curl 调试方便）
	return strings.TrimSpace(r.Header.Get("X-Auth-Token"))
}

// requireUser 鉴权失败时已写好响应，返回 nil。
func (s *Server) requireUser(w http.ResponseWriter, r *http.Request) *User {
	token := bearerToken(r)
	u, ok := s.store.TokenUser(token)
	if !ok {
		writeErr(w, http.StatusUnauthorized, "unauthorized", "令牌无效或已注销，请重新登录")
		return nil
	}
	return u
}

// ---- 可见性（权限组） ----

// nodeScope 解析账号能看见哪些节点。
// allow == nil 表示不限制（管理员，或未分组账号 + open 策略）；
// restricted == true 表示只能看 allow 里的节点（allow 可能是空集合）。
//
// 这是全服务唯一的权限判定入口：拉取接口、登录响应、/me、管理端统计都调它，
// 保证「界面显示的数量」和「实际下发的数量」不会打架。
func (s *Server) nodeScope(u *User) (allow map[string]bool, restricted bool) {
	if u == nil {
		// 没登录不该走到这里（调用方已 401），按最小权限处理
		return map[string]bool{}, true
	}
	if u.isAdmin() {
		// 管理员不受权限组限制：否则管理员把自己分进某个组之后，
		// 控制台里看不到别的节点，等于把自己锁在门外。
		return nil, false
	}
	return s.acl.Scope(u.Groups)
}

// visibleNodes 返回该账号可见的节点摘要，以及结果是否被权限组裁剪过。
// 永远不会返回 nil（JSON 里必须是 [] 而不是 null）。
func (s *Server) visibleNodes(u *User) ([]NodeSummary, bool) {
	allow, restricted := s.nodeScope(u)
	list := s.nodes.ListByIDs(allow)
	if list == nil {
		list = []NodeSummary{}
	}
	return list, restricted
}

// ---- handlers ----

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":         true,
		"name":       "rekosync",
		"version":    s.version,
		"time":       time.Now().UnixMilli(),
		"uptime_sec": int64(time.Since(s.started).Seconds()),
	})
}

type loginReq struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Device   string `json:"device"`
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 POST")
		return
	}
	ip := clientIP(r)
	if !s.limiter.Allow(ip) {
		writeErr(w, http.StatusTooManyRequests, "rate_limited", "登录失败次数过多，请 10 分钟后再试")
		return
	}

	var req loginReq
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	agent := r.Header.Get("User-Agent")
	if req.Device != "" {
		agent = req.Device
	}

	token, user, err := s.store.Authenticate(strings.ToLower(strings.TrimSpace(req.Username)), req.Password, agent)
	if err != nil {
		s.limiter.Fail(ip)
		switch {
		case errors.Is(err, ErrBadPassword):
			writeErr(w, http.StatusUnauthorized, "bad_credentials", "用户名或口令错误")
		case errors.Is(err, ErrDisabled):
			writeErr(w, http.StatusForbidden, "disabled", "账号已被禁用")
		default:
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
		}
		return
	}
	s.limiter.Reset(ip)

	visible, restricted := s.visibleNodes(user)
	writeJSON(w, http.StatusOK, map[string]any{
		"token":      token,
		"expires_at": 0, // 0 = 永久有效，只有注销或服务端删号才失效
		"user": map[string]any{
			"name":       user.Name,
			"created_at": user.CreatedAt,
			"role":       roleOf(user),
			"groups":     groupNames(s.acl, user.Groups),
		},
		"server_time": time.Now().UnixMilli(),
		"nodes": map[string]any{
			"count":      len(visible),
			"read_only":  true,
			"restricted": restricted,
		},
	})
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 POST")
		return
	}
	if s.requireUser(w, r) == nil {
		return
	}
	s.store.RevokeToken(bearerToken(r))
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET")
		return
	}
	u := s.requireUser(w, r)
	if u == nil {
		return
	}
	version, updatedAt, _ := s.nodes.Version()
	visible, restricted := s.visibleNodes(u)
	writeJSON(w, http.StatusOK, map[string]any{
		"user": map[string]any{
			"name":       u.Name,
			"created_at": u.CreatedAt,
			"role":       roleOf(u),
			"groups":     groupNames(s.acl, u.Groups),
		},
		"server_time": time.Now().UnixMilli(),
		"nodes": map[string]any{
			"version":    version,
			"updated_at": updatedAt,
			"count":      len(visible),
			"read_only":  true,
			"restricted": restricted,
		},
	})
}

// handleNodes 是客户端的唯一数据入口：只读，且按权限组过滤。
// ?version=<上次拿到的版本> 命中时只回 unchanged，省一次流量。
//
// version 是「节点池全局版本」，权限组或账号归属变化时会被推高一格，
// 所以客户端不需要知道 ACL 的存在也能在权限变化后重新拉取。
func (s *Server) handleNodes(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET；节点由管理员维护，客户端无上传接口")
		return
	}
	u := s.requireUser(w, r)
	if u == nil {
		return
	}
	version, updatedAt, _ := s.nodes.Version()
	if since := strings.TrimSpace(r.URL.Query().Get("version")); since != "" {
		if v, err := strconv.ParseInt(since, 10, 64); err == nil && v == version {
			writeJSON(w, http.StatusOK, map[string]any{
				"version":    version,
				"updated_at": updatedAt,
				"unchanged":  true,
				"read_only":  true,
			})
			return
		}
	}
	list, restricted := s.visibleNodes(u)
	writeJSON(w, http.StatusOK, map[string]any{
		"version":    version,
		"updated_at": updatedAt,
		"count":      len(list),
		"read_only":  true,
		"restricted": restricted,
		"nodes":      list,
	})
}

// handleVaultLegacy 兼容旧客户端：不再按账号存快照。
// GET 一律 204（“服务端没有数据”），PUT 明确 403，避免旧客户端以为推送成功。
func (s *Server) handleVaultLegacy(w http.ResponseWriter, r *http.Request) {
	if s.requireUser(w, r) == nil {
		return
	}
	switch r.Method {
	case http.MethodPut, http.MethodPost:
		writeErr(w, http.StatusForbidden, "read_only",
			"节点由管理员统一维护，客户端不能上传；请升级到支持只读节点池的客户端版本")
	default:
		w.WriteHeader(http.StatusNoContent)
	}
}

// ---- 工具 ----

func decodeJSON(w http.ResponseWriter, r *http.Request, out any) error {
	ct := r.Header.Get("Content-Type")
	if ct != "" && !strings.HasPrefix(strings.ToLower(ct), "application/json") {
		return errors.New("Content-Type 必须是 application/json")
	}
	dec := json.NewDecoder(io.LimitReader(r.Body, maxBodyBytes+1))
	dec.DisallowUnknownFields()
	if err := dec.Decode(out); err != nil {
		if errors.Is(err, io.EOF) {
			return errors.New("请求体为空")
		}
		return errors.New("请求体不是合法 JSON：" + err.Error())
	}
	return nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeErr(w http.ResponseWriter, code int, id, msg string) {
	writeJSON(w, code, map[string]any{"error": map[string]any{"code": id, "message": msg}})
}

func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// ---- 登录限速 ----

type loginLimiter struct {
	mu     sync.Mutex
	limit  int
	window time.Duration
	hits   map[string]*limitEntry
}

type limitEntry struct {
	count int
	since time.Time
}

func newLoginLimiter(limit int, window time.Duration) *loginLimiter {
	return &loginLimiter{limit: limit, window: window, hits: map[string]*limitEntry{}}
}

func (l *loginLimiter) Allow(ip string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	e, ok := l.hits[ip]
	if !ok {
		return true
	}
	if time.Since(e.since) > l.window {
		delete(l.hits, ip)
		return true
	}
	return e.count < l.limit
}

func (l *loginLimiter) Fail(ip string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	e, ok := l.hits[ip]
	if !ok || time.Since(e.since) > l.window {
		l.hits[ip] = &limitEntry{count: 1, since: time.Now()}
		return
	}
	e.count++
}

func (l *loginLimiter) Reset(ip string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	delete(l.hits, ip)
}
