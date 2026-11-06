package grpc_server

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"grpc_server/auth"
	"grpc_server/gen"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/matsuridayo/libneko/neko_common"

	grpc_auth "github.com/grpc-ecosystem/go-grpc-middleware/auth"
	"google.golang.org/grpc"
)

type BaseServer struct {
	gen.LibcoreServiceServer
}

func (s *BaseServer) Exit(ctx context.Context, in *gen.EmptyReq) (out *gen.EmptyResp, _ error) {
	out = &gen.EmptyResp{}

	// Connection closed
	os.Exit(0)
	return
}

func RunCore(setupCore func(), server gen.LibcoreServiceServer) {
	_token := flag.String("token", "", "")
	_port := flag.Int("port", 19810, "")
	_debug := flag.Bool("debug", false, "")
	flag.CommandLine.Parse(os.Args[2:])

	neko_common.Debug = *_debug

	// 父进程（GUI）消失后自动退出，避免 GUI 崩溃或被强杀后内核留在后台占着
	// rekobox_core.exe 与本地端口。Windows 上不能用 Process.Wait（不是本进程
	// 创建的子进程，会一直阻塞），改为轮询存活状态。
	go func() {
		parentPid := os.Getppid()
		for {
			time.Sleep(2 * time.Second)
			if !processAlive(parentPid) {
				log.Println("parent exited, shutting down")
				os.Exit(0)
			}
		}
	}()

	// Libcore
	setupCore()

	// GRPC
	lis, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(*_port))
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}

	token := *_token
	if token == "" {
		os.Stderr.WriteString("Please set a token: ")
		s := bufio.NewScanner(os.Stdin)
		if s.Scan() {
			token = strings.TrimSpace(s.Text())
		}
	}
	if token == "" {
		fmt.Println("You must set a token")
		os.Exit(0)
	}
	os.Stderr.WriteString("token is set\n")

	auther := auth.Authenticator{
		Token: token,
	}

	s := grpc.NewServer(
		grpc.StreamInterceptor(grpc_auth.StreamServerInterceptor(auther.Authenticate)),
		grpc.UnaryInterceptor(grpc_auth.UnaryServerInterceptor(auther.Authenticate)),
	)
	gen.RegisterLibcoreServiceServer(s, server)

	name := "nekobox_core"

	log.Printf("%s grpc server listening at %v\n", name, lis.Addr())
	if err := s.Serve(lis); err != nil {
		log.Fatalf("failed to serve: %v", err)
	}
}
