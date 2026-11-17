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

const serverVersion = "2.0.0"

var maxBodyBytes int64 = 8 << 20

func main() {
	var (
		addr     = flag.String("addr", ":8080", "监听地址")
		dataDir  = flag.String("data", "./data", "数据目录")
		bodyMax  = flag.Int64("max-body", 8<<20, "单次请求体上限（字节）")
		addUser  = flag.String("adduser", "", "创建普通账号后退出")
		addAdmin = flag.String("addadmin", "", "创建管理员账号后退出")
		delUser  = flag.String("deluser", "", "删除账号后退出")
		passwd   = flag.String("passwd", "", "重置口令后退出")
		disable  = flag.String("disable", "", "禁用账号后退出")
		enable   = flag.String("enable", "", "启用账号后退出")
		list     = flag.Bool("users", false, "列出账号后退出")
		password = flag.String("password", "", "-adduser / -addadmin / -passwd 的口令；留空则从 stdin 读取")
		showVer  = flag.Bool("version", false, "打印版本后退出")
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
		fmt.Printf("%-20s %-8s %-8s %s\n", "账号", "角色", "状态", "创建时间")
		for _, u := range users {
			state := "正常"
			if u.Disabled {
				state = "禁用"
			}
			fmt.Printf("%-20s %-8s %-8s %s\n", u.Name, roleOf(u), state,
				time.UnixMilli(u.CreatedAt).Format("2006-01-02 15:04"))
		}
		return
	}

	srv := NewServer(store, nodes, serverVersion)
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
	log.Printf("节点池：%d 条启用中，版本 %d；网页控制台 http://%s/admin",
		len(nodes.List()), version, displayAddr(*addr))
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
