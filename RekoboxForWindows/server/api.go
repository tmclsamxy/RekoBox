package main

// HTTP 接口层。所有响应都是 JSON，错误统一为：
//   {"error":{"code":"...","message":"..."}}
//
// 路由：
//   GET  /api/v1/health          探活（不需要鉴权）
//   POST /api/v1/auth/login      登录，发永久令牌
//   POST /api/v1/auth/logout     注销，吊销当前令牌
//   GET  /api/v1/auth/me         校验令牌 + 回带账号与快照版本
//   GET  /api/v1/vault           下载节点快照（204 表示服务端还没有数据）
//   PUT  /api/v1/vault           上传节点快照（乐观锁，冲突返回 409 + 服务端当前快照）

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

type Server struct {
	store   *Store
	limiter *loginLimiter
	started time.Time
	version string
}

func NewServer(store *Store, version string) *Server {
	return &Server{
		store:   store,
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
	mux.HandleFunc("/api/v1/vault", s.withLog(s.handleVault))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
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

	writeJSON(w, http.StatusOK, map[string]any{
		"token":        token,
		"expires_at":   0, // 0 = 永久有效，只有注销或服务端删号才失效
		"user":         map[string]any{"name": user.Name, "created_at": user.CreatedAt},
		"server_time":  time.Now().UnixMilli(),
		"vault_exists": s.vaultExists(user.Name),
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
	resp := map[string]any{
		"user":        map[string]any{"name": u.Name, "created_at": u.CreatedAt},
		"server_time": time.Now().UnixMilli(),
		"vault":       nil,
	}
	if v, ok := s.store.GetVault(u.Name); ok {
		resp["vault"] = map[string]any{
			"version":    v.Version,
			"updated_at": v.UpdatedAt,
			"device":     v.Device,
		}
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handleVault(w http.ResponseWriter, r *http.Request) {
	u := s.requireUser(w, r)
	if u == nil {
		return
	}
	switch r.Method {
	case http.MethodGet:
		v, ok := s.store.GetVault(u.Name)
		if !ok {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{
			"version":    v.Version,
			"updated_at": v.UpdatedAt,
			"device":     v.Device,
			"payload":    json.RawMessage(v.Payload),
		})
	case http.MethodPut:
		var req struct {
			BaseVersion int64           `json:"base_version"`
			Device      string          `json:"device"`
			Payload     json.RawMessage `json:"payload"`
		}
		if err := decodeJSON(w, r, &req); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		if len(req.Payload) == 0 {
			writeErr(w, http.StatusBadRequest, "bad_request", "payload 不能为空")
			return
		}
		// payload 必须是 JSON 对象：服务端不解析内容，但要求结构可控
		var probe map[string]json.RawMessage
		if err := json.Unmarshal(req.Payload, &probe); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", "payload 必须是 JSON 对象")
			return
		}
		device := req.Device
		if device == "" {
			device = r.Header.Get("User-Agent")
		}
		v, err := s.store.PutVault(u.Name, req.BaseVersion, device, req.Payload)
		if errors.Is(err, ErrVersionConflict) {
			body := map[string]any{
				"error": map[string]any{
					"code":    "version_conflict",
					"message": "服务端版本已更新，请合并后重试",
				},
			}
			if v != nil {
				body["current"] = map[string]any{
					"version":    v.Version,
					"updated_at": v.UpdatedAt,
					"device":     v.Device,
					"payload":    json.RawMessage(v.Payload),
				}
			}
			writeJSON(w, http.StatusConflict, body)
			return
		}
		if err != nil {
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{
			"version":    v.Version,
			"updated_at": v.UpdatedAt,
			"device":     v.Device,
		})
	default:
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET/PUT")
	}
}

func (s *Server) vaultExists(user string) bool {
	_, ok := s.store.GetVault(user)
	return ok
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
