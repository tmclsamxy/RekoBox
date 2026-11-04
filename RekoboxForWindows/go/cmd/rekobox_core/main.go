package main

import (
	"fmt"
	"os"
	_ "unsafe"

	"grpc_server"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/neko"
)

func main() {
	fmt.Println("sing-box:", constant.Version, "RekoBox:", neko_common.Version_neko)
	fmt.Println()

	// gRPC 后端（GUI 以 `rekobox_core nekobox -port N` 拉起）
	if len(os.Args) > 1 && os.Args[1] == "nekobox" {
		neko_common.RunMode = neko_common.RunMode_NekoBox_Core
		grpc_server.RunCore(setupCore, &server{})
		return
	}

	// sing-box CLI（TUN 模式以 `--disable-color run -c <config>` 拉起内核）
	os.Exit(neko.CLI(os.Args[1:]))
}
