package main

// rekosync —— RekoBox 节点同步后端。
//
// 只依赖 Go 标准库，编译产物是单个可执行文件，配 systemd 直接跑。
// 账号不开放注册，全部由运维在服务器上执行子命令创建。
//
// 用法：
//   rekosync -adduser alice                 # 创建普通账号（口令从 stdin 读，或 -password 指定）
//   rekosync -addadmin root                 # 创建管理员账号（可登录 /admin 网页控制台）
//   rekosync -passwd  alice                 # 改口令（同时吊销该账号所有旧令牌）
//   rekosync -deluser alice                 # 删号（连带吊销令牌）
//   rekosync -disable alice / -enable alice  # 禁用 / 启用账号
//   rekosync -users                         # 列出账号
//   rekosync -addgroup 套餐A                 # 创建权限组
//   rekosync -groups                        # 列出权限组及其节点数
//   rekosync -groupnodes "套餐A=id1,id2"     # 设置权限组可访问的节点（覆盖）
//   rekosync -usergroups "alice=套餐A"       # 设置账号所属权限组（覆盖）
//   rekosync -acl closed                    # 未分组账号的策略（open|closed）
//   rekosync                                # 启动 HTTP 服务（网页控制台在 /admin）

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"strings"
	"syscall"
	"time"
)

const serverVersion = "2.1.0"

var maxBodyBytes int64 = 8 << 20

func main() {
	var (
		addr       = flag.String("addr", ":8080", "监听地址")
		dataDir    = flag.String("data", "./data", "数据目录")
		bodyMax    = flag.Int64("max-body", 8<<20, "单次请求体上限（字节）")
		addUser    = flag.String("adduser", "", "创建普通账号后退出")
		addAdmin   = flag.String("addadmin", "", "创建管理员账号后退出")
		delUser    = flag.String("deluser", "", "删除账号后退出")
		passwd     = flag.String("passwd", "", "重置口令后退出")
		disable    = flag.String("disable", "", "禁用账号后退出")
		enable     = flag.String("enable", "", "启用账号后退出")
		list       = flag.Bool("users", false, "列出账号后退出")
		password   = flag.String("password", "", "-adduser / -addadmin / -passwd 的口令；留空则从 stdin 读取")
		addGroup   = flag.String("addgroup", "", "创建权限组后退出")
		listGroups = flag.Bool("groups", false, "列出权限组后退出")
		groupNodes = flag.String("groupnodes", "", `设置权限组可访问节点后退出，格式 "组名=节点id,节点id"`)
		userGroups = flag.String("usergroups", "", `设置账号所属权限组后退出，格式 "账号=组名,组名"`)
		aclPolicy  = flag.String("acl", "", "设置未分组账号的策略（open|closed）后退出，不传则不改动")
		showVer    = flag.Bool("version", false, "打印版本后退出")
	)
	flag.Parse()

	maxBodyBytes = *bodyMax
	if *showVer {
		fmt.Printf("rekosync %s\n", serverVersion)
		return
	}

	store, err := OpenStore(*dataDir)
	if err != nil {
		log.Fatalf("打开数据目录失败：%v", err)
	}
	nodes, err := OpenNodeStore(*dataDir)
	if err != nil {
		log.Fatalf("打开节点池失败：%v", err)
	}
	acl, err := OpenACLStore(*dataDir)
	if err != nil {
		log.Fatalf("打开权限组失败：%v", err)
	}

	switch {
	case *addUser != "" || *addAdmin != "":
		name, role := *addUser, "user"
		if *addAdmin != "" {
			name, role = *addAdmin, RoleAdmin
		}
		pwd := *password
		if pwd == "" {
			pwd = readPassword(fmt.Sprintf("为%s %s 设置口令: ",
				map[bool]string{true: "管理员账号", false: "新账号"}[role == RoleAdmin], name))
		}
		u, err := store.CreateUser(name, pwd, role)
		if err != nil {
			log.Fatalf("创建失败：%v", err)
		}
		fmt.Printf("已创建%s %s（数据目录 %s）\n",
			map[bool]string{true: "管理员账号", false: "账号"}[role == RoleAdmin], u.Name, *dataDir)
		return
	case *delUser != "":
		if err := store.DeleteUser(*delUser); err != nil {
			log.Fatalf("删除失败：%v", err)
		}
		fmt.Printf("已删除账号 %s 及其全部令牌\n", *delUser)
		return
	case *disable != "" || *enable != "":
		name, off := *disable, true
		if *enable != "" {
			name, off = *enable, false
		}
		if err := store.SetDisabled(name, off); err != nil {
			log.Fatalf("操作失败：%v", err)
		}
		fmt.Printf("账号 %s 已%s（旧令牌已吊销）\n", name,
			map[bool]string{true: "禁用", false: "启用"}[off])
		return
	case *passwd != "":
		pwd := *password
		if pwd == "" {
			pwd = readPassword("输入新口令: ")
		}
		if err := store.SetPassword(*passwd, pwd); err != nil {
			log.Fatalf("改口令失败：%v", err)
		}
		fmt.Printf("已更新账号 %s 的口令，旧令牌已全部吊销\n", *passwd)
		return
	case *list:
		users := store.ListUsers()
		if len(users) == 0 {
			fmt.Println("还没有账号，用 -addadmin 创建一个管理员")
			return
		}
		fmt.Printf("%-20s %-8s %-8s %-22s %s\n", "账号", "角色", "状态", "权限组", "创建时间")
		for _, u := range users {
			state := "正常"
			if u.Disabled {
				state = "禁用"
			}
			fmt.Printf("%-20s %-8s %-8s %-22s %s\n", u.Name, roleOf(u), state,
				groupLabel(acl, u.Groups),
				time.UnixMilli(u.CreatedAt).Format("2006-01-02 15:04"))
		}
		fmt.Printf("\n未分组账号策略：%s（%s）\n", acl.Policy(), policyHint(acl.Policy()))
		return
	case *addGroup != "":
		g, err := acl.Create(*addGroup, "")
		if err != nil {
			log.Fatalf("创建失败：%v", err)
		}
		fmt.Printf("已创建权限组 %s（id %s），接着用 -groupnodes %q=节点id,节点id 配节点\n",
			g.Name, g.ID, g.Name)
		return
	case *listGroups:
		groups := acl.List()
		if len(groups) == 0 {
			fmt.Println("还没有权限组，用 -addgroup 创建一个")
			return
		}
		usage := map[string]int{}
		for _, u := range store.ListUsers() {
			for _, gid := range u.Groups {
				usage[gid]++
			}
		}
		fmt.Printf("%-20s %-14s %-8s %s\n", "权限组", "ID", "节点数", "账号数")
		for _, g := range groups {
			fmt.Printf("%-20s %-14s %-8d %d\n", g.Name, g.ID, len(g.Nodes), usage[g.ID])
		}
		return
	case *groupNodes != "":
		name, ids, ok := splitAssignment(*groupNodes)
		if !ok {
			log.Fatal(`格式应为 "组名=节点id,节点id"（用 -groups 或控制台查 id）`)
		}
		g, found := acl.FindByName(name)
		if !found {
			log.Fatalf("权限组 %q 不存在", name)
		}
		ids = dedupeStrings(ids)
		if missing := nodes.MissingIDs(ids); len(missing) > 0 {
			log.Fatalf("节点不存在：%s", previewList(missing))
		}
		g, err = acl.SetNodes(g.ID, ids)
		if err != nil {
			log.Fatalf("配置失败：%v", err)
		}
		if _, err := nodes.BumpVersion(); err != nil {
			log.Printf("警告：更新节点池版本失败：%v", err)
		}
		fmt.Printf("权限组 %s（%s）现在包含 %d 个节点，属组账号下次拉取即生效\n",
			g.Name, g.ID, len(g.Nodes))
		return
	case *userGroups != "":
		name, names, ok := splitAssignment(*userGroups)
		if !ok {
			log.Fatal(`格式应为 "账号=组名,组名"（留空表示清空归属）`)
		}
		if store.GetUser(name) == nil {
			log.Fatalf("账号 %q 不存在", name)
		}
		ids := make([]string, 0, len(names))
		for _, n := range names {
			g, found := acl.FindByName(n)
			if !found {
				log.Fatalf("权限组 %q 不存在", n)
			}
			ids = append(ids, g.ID)
		}
		if err := store.SetUserGroups(name, ids); err != nil {
			log.Fatalf("配置失败：%v", err)
		}
		if _, err := nodes.BumpVersion(); err != nil {
			log.Printf("警告：更新节点池版本失败：%v", err)
		}
		fmt.Printf("账号 %s 的权限组已设为 %s\n", name, groupLabel(acl, ids))
		return
	case *aclPolicy != "":
		p := strings.ToLower(strings.TrimSpace(*aclPolicy))
		if p != PolicyOpen && p != PolicyClosed {
			log.Fatalf("-acl 只能是 open 或 closed（当前 %s）", acl.Policy())
		}
		if err := acl.SetPolicy(p); err != nil {
			log.Fatalf("设置失败：%v", err)
		}
		if _, err := nodes.BumpVersion(); err != nil {
			log.Printf("警告：更新节点池版本失败：%v", err)
		}
		fmt.Printf("未分组账号策略已设为 %s（%s）\n", p, policyHint(p))
		return
	}

	srv := NewServer(store, nodes, acl, serverVersion)
	httpServer := &http.Server{
		Addr:              *addr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       60 * time.Second,
		WriteTimeout:      60 * time.Second,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}

	// 优雅退出：停机时把正在处理的请求跑完（同步请求很短）
	done := make(chan struct{})
	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
		<-sig
		log.Println("收到退出信号，正在关闭...")
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = httpServer.Shutdown(ctx)
		close(done)
	}()

	log.Printf("rekosync %s 启动，监听 %s，数据目录 %s，%s/%s",
		serverVersion, *addr, *dataDir, runtime.GOOS, runtime.GOARCH)
	version, _, _ := nodes.Version()
	log.Printf("节点池：%d 条启用中，版本 %d；权限组：%d 个，未分组账号策略 %s；网页控制台 http://%s/admin",
		len(nodes.List()), version, acl.Count(), acl.Policy(), displayAddr(*addr))
	if ungrouped := store.UngroupedUsers(); len(ungrouped) > 0 && acl.Policy() == PolicyOpen {
		log.Printf("提示：%d 个账号未分配权限组，当前策略下可见全部节点（改成 -acl closed 即收紧）",
			len(ungrouped))
	}
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("监听失败：%v", err)
	}
	<-done
}

func readPassword(prompt string) string {
	fmt.Print(prompt)
	r := bufio.NewReader(os.Stdin)
	line, err := r.ReadString('\n')
	if err != nil && line == "" {
		log.Fatalf("读取口令失败：%v", err)
	}
	pwd := strings.TrimRight(line, "\r\n")
	if pwd == "" {
		log.Fatal("口令不能为空")
	}
	return pwd
}

// displayAddr 把 ":8080" 变成 "127.0.0.1:8080"，方便直接点开控制台。
func displayAddr(addr string) string {
	if strings.HasPrefix(addr, ":") {
		return "127.0.0.1" + addr
	}
	if strings.HasPrefix(addr, "0.0.0.0:") {
		return "127.0.0.1" + strings.TrimPrefix(addr, "0.0.0.0")
	}
	return addr
}

// splitAssignment 解析命令行里的 "键=值1,值2" 形式。
// 值部分允许为空（用来清空归属），但等号必须有，避免手误当成建号命令。
func splitAssignment(s string) (key string, values []string, ok bool) {
	parts := strings.SplitN(s, "=", 2)
	if len(parts) != 2 {
		return "", nil, false
	}
	key = strings.TrimSpace(parts[0])
	if key == "" {
		return "", nil, false
	}
	for _, v := range strings.Split(parts[1], ",") {
		if v = strings.TrimSpace(v); v != "" {
			values = append(values, v)
		}
	}
	return key, values, true
}

// groupLabel 把组 id 列表渲染成人类可读的名称串。
func groupLabel(acl *ACLStore, ids []string) string {
	if len(ids) == 0 {
		return "(未分组)"
	}
	names := make([]string, 0, len(ids))
	for _, id := range ids {
		if g, ok := acl.Get(id); ok {
			names = append(names, g.Name)
			continue
		}
		names = append(names, id+"(已删除)")
	}
	return strings.Join(names, ",")
}

func policyHint(p string) string {
	if p == PolicyClosed {
		return "未分组账号看不到任何节点"
	}
	return "未分组账号可见全部启用节点（兼容旧部署）"
}
