package neko

import (
	"io"
	"net"
	"testing"
	"time"

	xproxy "golang.org/x/net/proxy"
)

// 验证 neko.Create 开启 stats 后，QueryStats 能否读到 outbound 流量。
// 只依赖标准库 + x/net/proxy，不引入测试框架。
func TestQueryStatsPipeline(t *testing.T) {
	// 1. 本地 echo 服务
	echoLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer echoLn.Close()
	go func() {
		for {
			c, err := echoLn.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				io.Copy(c, c)
			}()
		}
	}()

	// 2. sing-box：socks 入站 -> direct 出站（tag=proxy）
	socksLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	socksPort := socksLn.Addr().(*net.TCPAddr).Port
	socksLn.Close()

	cfg := `{
      "log": {"level": "warn"},
      "inbounds": [{"type":"socks","tag":"socks-in","listen":"127.0.0.1","listen_port":` +
		itoa(socksPort) + `}],
      "outbounds": [{"type":"direct","tag":"proxy"}]
    }`

	inst, cancel, err := Create([]byte(cfg), []string{"proxy"}, nil)
	if err != nil {
		t.Fatal("create:", err)
	}
	defer func() {
		cancel()
		Close(inst)
	}()

	// 3. 经 socks 访问 echo，产生流量
	dialer, err := xproxy.SOCKS5("tcp", "127.0.0.1:"+itoa(socksPort), nil, xproxy.Direct)
	if err != nil {
		t.Fatal("socks dialer:", err)
	}
	conn, err := dialer.Dial("tcp", echoLn.Addr().String())
	if err != nil {
		t.Fatal("dial:", err)
	}
	payload := make([]byte, 8192)
	if _, err = conn.Write(payload); err != nil {
		t.Fatal("write:", err)
	}
	buf := make([]byte, len(payload))
	if _, err = io.ReadFull(conn, buf); err != nil {
		t.Fatal("read:", err)
	}
	conn.Close()
	time.Sleep(200 * time.Millisecond)

	// 4. 查统计
	up := QueryStats(inst, "outbound>>>proxy>>>traffic>>>uplink")
	down := QueryStats(inst, "outbound>>>proxy>>>traffic>>>downlink")
	t.Logf("uplink=%d downlink=%d", up, down)
	if up == 0 && down == 0 {
		t.Fatal("stats 为 0：QueryStats 链路未生效")
	}
	if up != int64(len(payload)) || down != int64(len(payload)) {
		t.Fatalf("首次读取应为实际字节数 %d，得到 up=%d down=%d", len(payload), up, down)
	}

	// 再读一次：没有新流量，必须归零（Reset_ 生效）。
	// 不归零的话 GUI 会把累计值当增量，速率永远不回落到 0。
	time.Sleep(200 * time.Millisecond)
	up2 := QueryStats(inst, "outbound>>>proxy>>>traffic>>>uplink")
	down2 := QueryStats(inst, "outbound>>>proxy>>>traffic>>>downlink")
	t.Logf("second read: uplink=%d downlink=%d", up2, down2)
	if up2 != 0 || down2 != 0 {
		t.Fatalf("计数器未重置：第二次读到 up=%d down=%d（应为 0）", up2, down2)
	}
}

func itoa(v int) string {
	if v == 0 {
		return "0"
	}
	var b []byte
	for v > 0 {
		b = append([]byte{byte('0' + v%10)}, b...)
		v /= 10
	}
	return string(b)
}
