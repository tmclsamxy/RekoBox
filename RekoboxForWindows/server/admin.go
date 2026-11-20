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
//   GET    users                   账号列表（含权限组与可见节点数）
//   POST   users                   {name, password, role, groups?}
//   DELETE users/<name>            删号（连带吊销令牌）
//   POST   users/<name>/password   {password}
//   POST   users/<name>/state      {disabled:true|false}
//   POST   users/<name>/groups     {groups:[组id...]}  整体替换归属
//   GET    groups                  权限组列表 + 兜底策略
//   POST   groups                  {name, description, nodes?} 建组
//   PATCH  groups/<id>             {name?, description?}
//   DELETE groups/<id>             删组（同时摘掉账号上的引用）
//   POST   groups/<id>/nodes       {ids:[节点id...]} 整体替换可访问节点
//   POST   acl/policy              {default_policy:"open"|"closed"}

import (
	_ "embed"
	"errors"
	"fmt"
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
	case rest == "groups":
		switch r.Method {
		case http.MethodGet:
			s.adminListGroups(w)
		case http.MethodPost:
			s.adminCreateGroup(w, r)
		default:
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 GET/POST")
		}
	case rest == "acl/policy":
		if r.Method != http.MethodPost {
			writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 POST")
			return
		}
		s.adminSetPolicy(w, r)
	case strings.HasPrefix(rest, "groups/"):
		s.adminGroupItem(w, r, strings.TrimPrefix(rest, "groups/"))
	default:
		writeErr(w, http.StatusNotFound, "not_found", "没有这个接口："+r.URL.Path)
	}
}

// ---- 概览 ----

func (s *Server) adminOverview(w http.ResponseWriter, me *User) {
	version, updatedAt, total := s.nodes.Version()
	enabled := len(s.nodes.List())
	users := s.store.ListUsers()
	ungrouped := s.store.UngroupedUsers()
	writeJSON(w, http.StatusOK, map[string]any{
		"server": map[string]any{
			"name":       "rekosync",
			"version":    s.version,
			"uptime_sec": int64(time.Since(s.started).Seconds()),
			"time":       time.Now().UnixMilli(),
			"go":         runtime.Version(),
		},
		"me": map[string]any{
			"name":   me.Name,
			"role":   me.Role,
			"groups": groupNames(s.acl, me.Groups),
		},
		"nodes": map[string]any{
			"version":    version,
			"updated_at": updatedAt,
			"total":      total,
			"enabled":    enabled,
		},
		"users": map[string]any{
			"total":     len(users),
			"ungrouped": len(ungrouped),
		},
		"acl": map[string]any{
			"groups":         s.acl.Count(),
			"default_policy": s.acl.Policy(),
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
		s.pruneACLForNodes([]string{id})
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
	if req.Action == "delete" && n > 0 {
		s.pruneACLForNodes(req.IDs)
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
		allow, restricted := s.nodeScope(u)
		out = append(out, map[string]any{
			"name":       u.Name,
			"role":       roleOf(u),
			"created_at": u.CreatedAt,
			"disabled":   u.Disabled,
			"sessions":   sessions,
			"last_used":  lastUsed,
			"groups":     groupNames(s.acl, u.Groups),
			// node_count = 这个账号实际能拉到的节点数，直接用真实鉴权逻辑算，
			// 免得界面上的数字和客户端看到的不一致。
			"node_count": len(s.nodes.ListByIDs(allow)),
			"restricted": restricted,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"users": out,
		"me":    map[string]any{"name": me.Name, "role": me.Role},
		"acl": map[string]any{
			"default_policy": s.acl.Policy(),
			"groups":         s.acl.Count(),
			"ungrouped":      s.store.UngroupedUsers(),
		},
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
		Name     string   `json:"name"`
		Password string   `json:"password"`
		Role     string   `json:"role"`
		Groups   []string `json:"groups"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	name := strings.ToLower(strings.TrimSpace(req.Name))
	if req.Role == "" {
		req.Role = "user"
	}
	groups, err := s.checkGroupsExist(req.Groups)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "unknown_group", err.Error())
		return
	}
	u, err := s.store.CreateUser(name, req.Password, req.Role, groups...)
	if errors.Is(err, ErrUserExists) {
		writeErr(w, http.StatusConflict, "duplicate", "账号已存在")
		return
	}
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	// 建号时带了权限组就已经影响可见范围，推版本让别的设备也跟着刷新
	if len(u.Groups) > 0 {
		s.aclChanged()
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"user": map[string]any{
			"name":       u.Name,
			"role":       roleOf(u),
			"created_at": u.CreatedAt,
			"groups":     groupNames(s.acl, u.Groups),
		},
	})
	log.Printf("admin: 创建账号 %s（%s，权限组 %v）", u.Name, roleOf(u), u.Groups)
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

	case action == "groups" && r.Method == http.MethodPost:
		var req struct {
			Groups []string `json:"groups"`
		}
		if err := decodeJSON(w, r, &req); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		groups, err := s.checkGroupsExist(req.Groups)
		if err != nil {
			writeErr(w, http.StatusBadRequest, "unknown_group", err.Error())
			return
		}
		if err := s.store.SetUserGroups(name, groups); err != nil {
			if errors.Is(err, ErrNoUser) {
				writeErr(w, http.StatusNotFound, "not_found", "账号不存在")
				return
			}
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		// 归属变了，这个账号（可能还有别的设备）下次必须重新拉一遍节点。
		s.aclChanged()
		count, restricted := 0, false
		if u := s.store.GetUser(name); u != nil {
			allow, r2 := s.nodeScope(u)
			count, restricted = len(s.nodes.ListByIDs(allow)), r2
		}
		writeJSON(w, http.StatusOK, map[string]any{
			"ok":         true,
			"groups":     groupNames(s.acl, groups),
			"node_count": count,
			"restricted": restricted,
		})
		log.Printf("admin: 账号 %s 的权限组设为 %v", name, groups)

	default:
		writeErr(w, http.StatusNotFound, "not_found", "没有这个接口："+r.URL.Path)
	}
}

// ---- 权限组 ----

func (s *Server) adminListGroups(w http.ResponseWriter) {
	groups := s.acl.List()
	// 每个组挂上节点数，前端不用再自己算
	type groupView struct {
		*ACLGroup
		NodeCount int `json:"node_count"`
	}
	out := make([]groupView, 0, len(groups))
	for _, g := range groups {
		out = append(out, groupView{ACLGroup: g, NodeCount: len(g.Nodes)})
	}
	ungrouped := s.store.UngroupedUsers()
	writeJSON(w, http.StatusOK, map[string]any{
		"groups":         out,
		"default_policy": s.acl.Policy(),
		"ungrouped":      ungrouped,
	})
}

func (s *Server) adminCreateGroup(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Name        string   `json:"name"`
		Description string   `json:"description"`
		Nodes       []string `json:"nodes"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	nodeIDs := dedupeStrings(req.Nodes)
	if missing := s.nodes.MissingIDs(nodeIDs); len(missing) > 0 {
		writeErr(w, http.StatusBadRequest, "unknown_node",
			"节点不存在："+previewList(missing))
		return
	}
	g, err := s.acl.Create(req.Name, req.Description)
	switch {
	case errors.Is(err, ErrGroupExists):
		writeErr(w, http.StatusConflict, "duplicate", err.Error())
		return
	case err != nil:
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	if len(nodeIDs) > 0 {
		if _, err := s.acl.SetNodes(g.ID, nodeIDs); err != nil {
			// 建组成功但配节点失败：把空组留着比回滚更可控，管理员重试即可
			log.Printf("admin: 权限组 %s 建好后配节点失败：%v", g.Name, err)
			writeErr(w, http.StatusInternalServerError, "internal", "权限组已创建，但节点配置失败："+err.Error())
			return
		}
		g, _ = s.acl.Get(g.ID)
	}
	s.aclChanged()
	writeJSON(w, http.StatusOK, map[string]any{
		"group":      g,
		"node_count": len(g.Nodes),
	})
	log.Printf("admin: 创建权限组 %s（%s），%d 个节点", g.Name, g.ID, len(g.Nodes))
}

func (s *Server) adminGroupItem(w http.ResponseWriter, r *http.Request, rest string) {
	parts := strings.Split(strings.Trim(rest, "/"), "/")
	id := strings.TrimSpace(parts[0])
	if id == "" {
		writeErr(w, http.StatusBadRequest, "bad_request", "缺少权限组 id")
		return
	}
	action := ""
	if len(parts) > 1 {
		action = parts[1]
	}

	switch {
	case action == "" && r.Method == http.MethodDelete:
		if err := s.acl.Delete(id); err != nil {
			if errors.Is(err, ErrNoGroup) {
				writeErr(w, http.StatusNotFound, "group_not_found", "权限组不存在")
				return
			}
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		// 组没了，账号上的引用必须一起摘掉，否则会留下指向空组的 id
		detached, err := s.store.RemoveGroupFromUsers(id)
		if err != nil {
			log.Printf("admin: 摘除账号上的权限组引用失败：%v", err)
		}
		s.aclChanged()
		writeJSON(w, http.StatusOK, map[string]any{"ok": true, "detached_users": detached})

	case action == "" && (r.Method == http.MethodPatch || r.Method == http.MethodPut || r.Method == http.MethodPost):
		var patch struct {
			Name        *string `json:"name"`
			Description *string `json:"description"`
		}
		if err := decodeJSON(w, r, &patch); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		g, err := s.acl.Update(id, patch.Name, patch.Description)
		switch {
		case errors.Is(err, ErrNoGroup):
			writeErr(w, http.StatusNotFound, "group_not_found", "权限组不存在")
		case errors.Is(err, ErrGroupExists):
			writeErr(w, http.StatusConflict, "duplicate", err.Error())
		case err != nil:
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		default:
			writeJSON(w, http.StatusOK, map[string]any{"group": g})
		}

	case action == "nodes" && r.Method == http.MethodPost:
		var req struct {
			IDs []string `json:"ids"`
		}
		if err := decodeJSON(w, r, &req); err != nil {
			writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
			return
		}
		ids := dedupeStrings(req.IDs)
		if missing := s.nodes.MissingIDs(ids); len(missing) > 0 {
			writeErr(w, http.StatusBadRequest, "unknown_node", "节点不存在："+previewList(missing))
			return
		}
		g, err := s.acl.SetNodes(id, ids)
		if err != nil {
			if errors.Is(err, ErrNoGroup) {
				writeErr(w, http.StatusNotFound, "group_not_found", "权限组不存在")
				return
			}
			writeErr(w, http.StatusInternalServerError, "internal", err.Error())
			return
		}
		s.aclChanged()
		writeJSON(w, http.StatusOK, map[string]any{
			"group":      g,
			"node_count": len(g.Nodes),
		})
		log.Printf("admin: 权限组 %s 配置 %d 个节点", g.Name, len(g.Nodes))

	case action == "":
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 PATCH/DELETE")
	case action != "nodes":
		writeErr(w, http.StatusNotFound, "not_found", "没有这个接口："+r.URL.Path)
	default:
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "只支持 POST")
	}
}

func (s *Server) adminSetPolicy(w http.ResponseWriter, r *http.Request) {
	var req struct {
		DefaultPolicy string `json:"default_policy"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	raw := strings.ToLower(strings.TrimSpace(req.DefaultPolicy))
	if raw != PolicyOpen && raw != PolicyClosed {
		writeErr(w, http.StatusBadRequest, "bad_request", "default_policy 只能是 open 或 closed")
		return
	}
	if err := s.acl.SetPolicy(raw); err != nil {
		writeErr(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	s.aclChanged()
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":             true,
		"default_policy": s.acl.Policy(),
		"ungrouped":      s.store.UngroupedUsers(),
	})
	log.Printf("admin: 未分组账号策略设为 %s", s.acl.Policy())
}

// ---- 权限组辅助 ----

// checkGroupsExist 校验一组权限组 id 是否都存在，顺带去重。
func (s *Server) checkGroupsExist(ids []string) ([]string, error) {
	ids = dedupeStrings(ids)
	missing := make([]string, 0)
	for _, id := range ids {
		if _, ok := s.acl.Get(id); !ok {
			missing = append(missing, id)
		}
	}
	if len(missing) > 0 {
		return nil, errors.New("权限组不存在：" + previewList(missing))
	}
	return ids, nil
}

// aclChanged 在权限组或归属变化后把节点池版本推高一格。
// 客户端只会比较 ?version=N，推高版本是「不改客户端也能立刻生效」的关键。
func (s *Server) aclChanged() {
	if _, err := s.nodes.BumpVersion(); err != nil {
		log.Printf("更新节点池版本失败：%v", err)
	}
}

// pruneACLForNodes 在节点删除后清理权限组里的引用。
func (s *Server) pruneACLForNodes(ids []string) {
	changed, err := s.acl.PruneNodes(ids)
	if err != nil {
		log.Printf("清理权限组内的节点引用失败：%v", err)
		return
	}
	if changed > 0 {
		s.aclChanged()
	}
}

// previewList 只列前几个，避免错误消息里塞进上百个 id。
func previewList(items []string) string {
	const max = 5
	if len(items) <= max {
		return strings.Join(items, ", ")
	}
	return strings.Join(items[:max], ", ") + fmt.Sprintf(" 等 %d 个", len(items))
}
