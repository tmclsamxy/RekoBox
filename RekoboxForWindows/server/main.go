package main

// rekosync —— RekoBox 节点同步后端。
//
// 只依赖 Go 标准库，编译产物是单个可执行文件，配 systemd 直接跑。
// 账号不开放注册，全部由运维在服务器上执行子命令创建。
//
// 用法：
//   rekosync -adduser alice                 # 创建账号（口令从 stdin 读，或 -password 指定）
//   rekosync -passwd  alice                 # 改口令（同时吊销该账号所有旧令牌）
//   rekosync -deluser alice                 # 删号（连带删快照、吊销令牌）
//   rekosync -users                         # 列出账号
//   rekosync                                # 启动 HTTP 服务

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

const serverVersion = "1.0.0"

var maxBodyBytes int64 = 8 << 20

func main() {
	var (
		addr     = flag.String("addr", ":8080", "监听地址")
		dataDir  = flag.String("data", "./data", "数据目录")
		bodyMax  = flag.Int64("max-body", 8<<20, "单次请求体上限（字节）")
		addUser  = flag.String("adduser", "", "创建账号后退出")
		delUser  = flag.String("deluser", "", "删除账号后退出")
		passwd   = flag.String("passwd", "", "重置口令后退出")
		list     = flag.Bool("users", false, "列出账号后退出")
		password = flag.String("password", "", "-adduser / -passwd 的口令；留空则从 stdin 读取")
	)
	flag.Parse()

	maxBodyBytes = *bodyMax

	store, err := OpenStore(*dataDir)
	if err != nil {
		log.Fatalf("打开数据目录失败：%v", err)
	}

	switch {
	case *addUser != "":
		pwd := *password
		if pwd == "" {
			pwd = readPassword("为新账号设置口令: ")
		}
		u, err := store.CreateUser(*addUser, pwd)
		if err != nil {
			log.Fatalf("创建失败：%v", err)
		}
		fmt.Printf("已创建账号 %s（数据目录 %s）\n", u.Name, *dataDir)
		return
	case *delUser != "":
		if err := store.DeleteUser(*delUser); err != nil {
			log.Fatalf("删除失败：%v", err)
		}
		fmt.Printf("已删除账号 %s 及其节点快照\n", *delUser)
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
			fmt.Println("还没有账号，用 -adduser 创建")
			return
		}
		fmt.Printf("%-20s %-8s %s\n", "账号", "状态", "创建时间")
		for _, u := range users {
			state := "正常"
			if u.Disabled {
				state = "禁用"
			}
			fmt.Printf("%-20s %-8s %s\n", u.Name, state,
				time.UnixMilli(u.CreatedAt).Format("2006-01-02 15:04"))
		}
		return
	}

	srv := NewServer(store, serverVersion)
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
