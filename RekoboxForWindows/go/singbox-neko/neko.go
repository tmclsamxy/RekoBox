// Package neko 是 RekoBox 对 sing-box 的封装层。
//
// 上游 1.11+ 删除了 nekoray 依赖的 cmd/sing-box 可导入入口（package main），
// 这里用 sing-box 的公开 API 重新提供三件事：
//  1. Create()：从 JSON 字节串创建并启动实例（供 gRPC 服务使用）
//  2. QueryStats()：读取 outbound 流量计数（替代已移除的 Router().SetV2RayServer）
//  3. CLI()：最小命令行（GUI 的 TUN 模式会以 `--disable-color run -c <config>` 拉起内核）
package neko

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"sync"
	"syscall"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/experimental/v2rayapi"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
	"github.com/sagernet/sing/service"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/json"
)

var (
	disableColor bool

	ctxMu   sync.RWMutex
	instCtx = make(map[*box.Box]context.Context)
)

// SetDisableColor 关闭日志里的 ANSI 颜色（Windows 控制台不支持时使用）
func SetDisableColor(d bool) {
	disableColor = d
}

// Writer 把 sing-box 的日志转发到任意 io.Writer（GUI 用它显示内核日志）
type Writer struct {
	Out     io.Writer
	NoColor bool
}

func (w Writer) DisableColors() bool { return w.NoColor }

func (w Writer) WriteMessage(level log.Level, message string) {
	if w.Out == nil {
		return
	}
	_, _ = io.WriteString(w.Out, fmt.Sprintf("%s %s\n", log.FormatLevel(level), message))
}

// Create 解析配置并启动实例。statsOutbounds 非空时开启 v2ray 统计（对应旧版的
// Router().SetV2RayServer），供 QueryStats 查询每个出站的流量。
func Create(content []byte, statsOutbounds []string, platformWriter log.PlatformWriter) (*box.Box, context.CancelFunc, error) {
	ctx, cancel := context.WithCancel(context.Background())
	ctx = include.Context(ctx)
	ctx = service.ContextWithDefaultRegistry(ctx)

	options, err := json.UnmarshalExtendedContext[option.Options](ctx, content)
	if err != nil {
		cancel()
		return nil, nil, E.Cause(err, "decode config")
	}

	if disableColor {
		if options.Log == nil {
			options.Log = &option.LogOptions{}
		}
		options.Log.DisableColor = true
	}

	if len(statsOutbounds) > 0 {
		if options.Experimental == nil {
			options.Experimental = &option.ExperimentalOptions{}
		}
		// 注意：box.New 只有在 V2RayAPI.Listen 非空时才会创建 v2ray server 并注册
		// adapter.V2RayServer（见 box.go 的 needV2RayAPI）。Listen 为空则统计服务不存在，
		// QueryStats 会一直返回 0。这里给一个本地回环随机端口，不对外开放。
		listen := "127.0.0.1:0"
		if options.Experimental.V2RayAPI != nil && options.Experimental.V2RayAPI.Listen != "" {
			listen = options.Experimental.V2RayAPI.Listen
		}
		options.Experimental.V2RayAPI = &option.V2RayAPIOptions{
			Listen: listen,
			Stats: &option.V2RayStatsServiceOptions{
				Enabled:   true,
				Outbounds: statsOutbounds,
			},
		}
	}

	instance, err := box.New(box.Options{
		Context:           ctx,
		Options:           options,
		PlatformLogWriter: platformWriter,
	})
	if err != nil {
		cancel()
		return nil, nil, E.Cause(err, "create service")
	}

	osSignals := make(chan os.Signal, 1)
	signal.Notify(osSignals, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	go func() {
		<-osSignals
		cancel()
	}()

	if err = instance.Start(); err != nil {
		signal.Stop(osSignals)
		cancel()
		return nil, nil, E.Cause(err, "start service")
	}

	ctxMu.Lock()
	instCtx[instance] = ctx
	ctxMu.Unlock()

	return instance, cancel, nil
}

// Close 回收实例与其上下文
func Close(instance *box.Box) {
	if instance == nil {
		return
	}
	ctxMu.Lock()
	delete(instCtx, instance)
	ctxMu.Unlock()
	_ = instance.Close()
}

// QueryStats 读取某个出站自上次查询以来的流量增量（字节）。
//
// 必须带 Reset_ = true：sing-box 的计数器是累计值，不 reset 的话调用方拿到的
// 是「启动至今总量」，GUI 把它当增量累加会让总量飙升，算出来的速率也永远是个
// 不归零的大数（表现为停止代理后速度还在显示）。
func QueryStats(instance *box.Box, name string) int64 {
	ctxMu.RLock()
	ctx := instCtx[instance]
	ctxMu.RUnlock()
	if ctx == nil {
		return 0
	}
	server := service.FromContext[adapter.V2RayServer](ctx)
	if server == nil {
		return 0
	}
	stats, ok := server.StatsService().(*v2rayapi.StatsService)
	if !ok || stats == nil {
		return 0
	}
	resp, err := stats.GetStats(ctx, &v2rayapi.GetStatsRequest{Name: name, Reset_: true})
	if err != nil || resp == nil || resp.Stat == nil {
		return 0
	}
	return resp.Stat.Value
}

// CLI 实现内核被当作命令行工具调用时的行为。
// 目前 GUI 只用 `--disable-color run -c <config>`（TUN 模式）与 `version`。
func CLI(args []string) int {
	fs := flag.NewFlagSet("rekobox_core", flag.ContinueOnError)
	fs.SetOutput(os.Stdout)
	fs.BoolVar(&disableColor, "disable-color", false, "disable color output")
	// sing-box 的其它常用参数在这里接受但忽略，避免旧脚本传参直接报错
	fs.String("D", "", "working directory")
	fs.String("C", "", "config directory")
	fs.String("c", "", "config file")

	if err := fs.Parse(args); err != nil {
		return 2
	}

	rest := fs.Args()
	if len(rest) == 0 {
		fmt.Printf("sing-box %s (RekoBox)\n", constant.Version)
		fs.Usage()
		return 0
	}

	switch rest[0] {
	case "version":
		fmt.Printf("sing-box version %s\n", constant.Version)
		return 0
	case "check":
		return cmdCheck(rest[1:])
	case "run":
		return cmdRun(rest[1:])
	default:
		fmt.Fprintf(os.Stderr, "unsupported command: %s\n", rest[0])
		return 2
	}
}

// configFromArgs 从子命令参数里取 -c
func configFromArgs(args []string) (string, error) {
	fs := flag.NewFlagSet("sub", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	path := fs.String("c", "", "config file")
	if err := fs.Parse(args); err != nil {
		return "", err
	}
	if *path == "" {
		return "", E.New("missing config path (-c)")
	}
	return *path, nil
}

func cmdCheck(args []string) int {
	path, err := configFromArgs(args)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	content, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	ctx := include.Context(context.Background())
	ctx = service.ContextWithDefaultRegistry(ctx)
	if _, err = json.UnmarshalExtendedContext[option.Options](ctx, content); err != nil {
		fmt.Fprintf(os.Stderr, "decode config: %v\n", err)
		return 1
	}
	return 0
}

func cmdRun(args []string) int {
	path, err := configFromArgs(args)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	content, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	// CLI 模式下 sing-box 自带 stderr 日志，再挂 PlatformWriter 会重复输出
	instance, cancel, err := Create(content, nil, nil)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer cancel()
	defer Close(instance)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig
	return 0
}
