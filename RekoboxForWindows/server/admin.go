package main

// 管理端 HTTP 接口 + 内嵌的网页控制台。
//
// 鉴权：先用 /api/v1/auth/login 用 admin 账号换令牌，之后每个请求带
//   Authorization: Bearer <token>
// 普通账号（role=user）访问 /api/v1/admin/* 一律 403。
//
// 路由（前缀 /api/v1/admin/，末尾路径由 handleAdmin 分发）：
//   GET    overview                服务器与节点池概览
//   GET    nodes                   节点全量（含禁用）
//   POST   nodes                   {links:"...\n...", group, remark} 批量录入
//   PATCH  nodes/<id>              改名称/分组/备注/启用/链接
//   DELETE nodes/<id>              删一个
//   POST   nodes/batch             {ids:[], action:"enable|disable|delete"}
//   POST   nodes/order             {ids:[...]} 重排
//   GET    users                   账号列表
//   POST   users                   {name, password, role}
//   DELETE users/<name>            删号（连带吊销令牌）
//   POST   users/<name>/password   {password}
//   POST   users/<name>/state      {disabled:true|false}

import (
	_ "embed"
	"errors"
	"log"
	"net/http"
	"runtime"
	"strings"
	"time"
)

//go:embed web/admin.html
var adminHTML []byte

func (s *Server) serveAdminPage(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET")
		return
	}
	if r.URL.Path != "/admin" && r.URL.Path != "/admin/" &&
		r.URL.Path != "/admin/index.html" {
		writeErr(w, http.StatusNotFound, "not_found", "没有这个页面："+r.URL.Path)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Referrer-Policy", "same-origin")
	w.WriteHeader(http.StatusOK)
	if r.Method == http.MethodHead {
		return
	}
	_, _ = w.Write(adminHTML)
}

// requireAdmin 鉴权失败时已写好响应，返回 nil。
func (s *Server) requireAdmin(w http.ResponseWriter, r *http.Request) *User {
	u := s.requireUser(w, r)
	if u == nil {
		return nil
	}
	if !u.isAdmin() {
		writeErr(w, http.StatusForbidden, "forbidden", "当前账号没有管理权限")
		return nil
	}
	return u
}

func (s *Server) handleAdmin(w http.ResponseWriter, r *http.Request) {
	u := s.requireAdmin(w, r)
	if u == nil {
		return
	}
	rest := strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/v1/admin"), "/")
	if rest == "" {
		writeErr(w, http.StatusNotFound, "not_found", "缺少子路径")
		return
	}

	switch {
	case rest == "overview":
		if r.Method != http.MethodGet {
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET")
			return
		}
		s.adminOverview(w, u)
	case rest == "nodes":
		switch r.Method {
		case http.MethodGet:
			s.adminListNodes(w)
		case http.MethodPost:
			s.adminAddNodes(w, r)
		default:
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET/POST")
		}
	case rest == "nodes/batch":
		if r.Method != http.MethodPost {
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 POST")
			return
		}
		s.adminBatchNodes(w, r)
	case rest == "nodes/order":
		if r.Method != http.MethodPost {
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 POST")
			return
		}
		s.adminReorderNodes(w, r)
	case strings.HasPrefix(rest, "nodes/"):
		id := strings.TrimPrefix(rest, "nodes/")
		s.adminNodeItem(w, r, id)
	case rest == "users":
		switch r.Method {
		case http.MethodGet:
			s.adminListUsers(w, u)
		case http.MethodPost:
			s.adminCreateUser(w, r)
		default:
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET/POST")
		}
	case strings.HasPrefix(rest, "users/"):
		s.adminUserItem(w, r, strings.TrimPrefix(rest, "users/"), u.Name)
	default:
		writeErr(w, http.StatusNotFound, "not_found", "没有这个接口："+r.URL.Path)
	}
}

// ---- 概览 ----

func (s *Server) adminOverview(w http.ResponseWriter, me *User) {
	version, updatedAt, total := s.nodes.Version()
	enabled := len(s.nodes.List())
	users := s.store.ListUsers()
	writeJSON(w, http.StatusOK, map[string]any{
		"server": map[string]any{
			"name":       "rekosync",
			"version":    s.version,
			"uptime_sec": int64(time.Since(s.started).Seconds()),
			"time":       time.Now().UnixMilli(),
			"go":         runtime.Version(),
		},
		"me": map[string]any{"name": me.Name, "role": me.Role},
		"nodes": map[string]any{
			"version":    version,
			"updated_at": updatedAt,
			"total":      total,
			"enabled":    enabled,
		},
		"users": map[string]any{
			"total": len(users),
		},
	})
}

// ---- 节点 ----

func (s *Server) adminListNodes(w http.ResponseWriter) {
	version, updatedAt, _ := s.nodes.Version()
	nodes := s.nodes.ListAll()
	if nodes == nil {
		nodes = []Node{}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"version":    version,
		"updated_at": updatedAt,
		"nodes":      nodes,
	})
}

func (s *Server) adminAddNodes(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Links  string   `json:"links"`
		Group  string   `json:"group"`
		Remark string   `json:"remark"`
		Link   string   `json:"link"`
		List   []string `json:"list"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	var links []string
	if strings.TrimSpace(req.Links) != "" {
		links = append(links, strings.Split(req.Links, "\n")...)
	}
	if strings.TrimSpace(req.Link) != "" {
		links = append(links, req.Link)
	}
	links = append(links, req.List...)
	if len(links) == 0 {
		writeErr(w, http.StatusBadRequest, "bad_request", "请至少提供一条节点链接")
		return
	}

	added, rejects, err := s.nodes.AddLinks(links, req.Group, req.Remark)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	if added == nil {
		added = []Node{}
	}
	if rejects == nil {
		rejects = []linkReject{}
	}
	version, updatedAt, total := s.nodes.Version()
	writeJSON(w, http.StatusOK, map[string]any{
		"added":      added,
		"rejected":   rejects,
		"version":    version,
		"updated_at": updatedAt,
		"total":      total,
	})
	log.Printf("admin: 录入节点 %d 条，拒绝 %d 条", len(added), len(rejects))
}

func (s *Server) adminNodeItem(w http.ResponseWriter, r *http.Request, id string) {
	switch r.Method {
	case http.MethodDelete:
		n, err := s.nodes.Delete([]string{id})
		if err != nil {
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		if n == 0 {
			writeErr(w, http.StatusNotFound, "not_found", "节点不存在")
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true, "removed": n})
	case http.MethodPatch, http.MethodPost, http.MethodPut:
		var patch NodePatch
		if err := decodeJSON(w, r, &patch); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		node, err := s.nodes.Update(id, patch)
		switch {
		case errors.Is(err, ErrNoNode):
			writeErr(w, http.StatusNotFound, "not_found", "节点不存在")
		case errors.Is(err, ErrNodeExists):
			writeErr(w, http.StatusConflict, "duplicate", err.Error())
		case err != nil:
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		default:
			writeJSON(w, http.StatusOK, map[string]any{"node": node})
		}
	default:
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 PATCH/DELETE")
	}
}

func (s *Server) adminBatchNodes(w http.ResponseWriter, r *http.Request) {
	var req struct {
		IDs    []string `json:"ids"`
		Action string   `json:"action"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	if len(req.IDs) == 0 {
		writeErr(w, http.StatusBadRequest, "bad_request", "ids 不能为空")
		return
	}
	var (
		n   int
		err error
	)
	switch req.Action {
	case "enable":
		n, err = s.nodes.SetEnabled(req.IDs, true)
	case "disable":
		n, err = s.nodes.SetEnabled(req.IDs, false)
	case "delete":
		n, err = s.nodes.Delete(req.IDs)
	default:
		writeErr(w, http.StatusBadRequest, "bad_request", "action 只能是 enable/disable/delete")
		return
	}
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "affected": n})
}

func (s *Server) adminReorderNodes(w http.ResponseWriter, r *http.Request) {
	var req struct {
		IDs []string `json:"ids"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	if err := s.nodes.Reorder(req.IDs); err != nil {
		writeErr(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// ---- 账号 ----

func (s *Server) adminListUsers(w http.ResponseWriter, me *User) {
	users := s.store.ListUsers()
	out := make([]map[string]any, 0, len(users))
	for _, u := range users {
		sessions, lastUsed := s.store.SessionStat(u.Name)
		out = append(out, map[string]any{
			"name":       u.Name,
			"role":       roleOf(u),
			"created_at": u.CreatedAt,
			"disabled":   u.Disabled,
			"sessions":   sessions,
			"last_used":  lastUsed,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"users": out,
		"me":    map[string]any{"name": me.Name, "role": me.Role},
	})
}

func roleOf(u *User) string {
	if u.Role == "" {
		return "user"
	}
	return u.Role
}

func (s *Server) adminCreateUser(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Name     string `json:"name"`
		Password string `json:"password"`
		Role     string `json:"role"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	name := strings.ToLower(strings.TrimSpace(req.Name))
	if req.Role == "" {
		req.Role = "user"
	}
	u, err := s.store.CreateUser(name, req.Password, req.Role)
	if errors.Is(err, ErrUserExists) {
		writeErr(w, http.StatusConflict, "duplicate", "账号已存在")
		return
	}
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"user": map[string]any{"name": u.Name, "role": roleOf(u), "created_at": u.CreatedAt},
	})
	log.Printf("admin: 创建账号 %s（%s）", u.Name, roleOf(u))
}

func (s *Server) adminUserItem(w http.ResponseWriter, r *http.Request, rest, me string) {
	parts := strings.Split(rest, "/")
	name := strings.ToLower(strings.TrimSpace(parts[0]))
	if name == "" {
		writeErr(w, http.StatusBadRequest, "bad_request", "缺少用户名")
		return
	}
	action := ""
	if len(parts) > 1 {
		action = parts[1]
	}

	switch {
	case action == "" && r.Method == http.MethodDelete:
		if name == me {
			writeErr(w, http.StatusBadRequest, "bad_request", "不能删除正在使用的账号")
			return
		}
		if err := s.store.DeleteUser(name); err != nil {
			if errors.Is(err, ErrNoUser) {
				writeErr(w, http.StatusNotFound, "not_found", "账号不存在")
				return
			}
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})

	case action == "password" && r.Method == http.MethodPost:
		var req struct {
			Password string `json:"password"`
		}
		if err := decodeJSON(w, r, &req); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		if err := s.store.SetPassword(name, req.Password); err != nil {
			if errors.Is(err, ErrNoUser) {
				writeErr(w, http.StatusNotFound, "not_found", "账号不存在")
				return
			}
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true, "note": "旧令牌已全部吊销"})

	case action == "state" && r.Method == http.MethodPost:
		var req struct {
			Disabled bool `json:"disabled"`
		}
		if err := decodeJSON(w, r, &req); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		if req.Disabled && name == me {
			writeErr(w, http.StatusBadRequest, "bad_request", "不能禁用正在使用的账号")
			return
		}
		if err := s.store.SetDisabled(name, req.Disabled); err != nil {
			if errors.Is(err, ErrNoUser) {
				writeErr(w, http.StatusNotFound, "not_found", "账号不存在")
				return
			}
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})

	default:
		writeErr(w, http.StatusNotFound, "not_found", "没有这个接口："+r.URL.Path)
	}
}
