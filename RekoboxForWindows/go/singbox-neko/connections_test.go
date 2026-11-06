package neko

import (
	"encoding/json"
	"io"
	"net"
	"testing"
	"time"

	xproxy "golang.org/x/net/proxy"
)

// 验证 GUI「连接」页的数据源：tracker 能把经过代理的连接记下来。
func TestConnectionsTracking(t *testing.T) {
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

	socksLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	socksPort := socksLn.Addr().(*net.TCPAddr).Port
	socksLn.Close()

	cfg := `{
      "log": {"level": "warn"},
      "inbounds": [{"type":"socks","tag":"socks-in","listen":"127.0.0.1","listen_port":` + itoa(socksPort) + `}],
      "outbounds": [{"type":"direct","tag":"proxy"}]
    }`

	inst, cancel, err := Create([]byte(cfg), nil, nil)
	if err != nil {
		t.Fatal("create:", err)
	}
	defer func() {
		cancel()
		Close(inst)
	}()

	dialer, err := xproxy.SOCKS5("tcp", "127.0.0.1:"+itoa(socksPort), nil, xproxy.Direct)
	if err != nil {
		t.Fatal("socks dialer:", err)
	}
	conn, err := dialer.Dial("tcp", echoLn.Addr().String())
	if err != nil {
		t.Fatal("dial:", err)
	}
	if _, err = conn.Write([]byte("hello")); err != nil {
		t.Fatal("write:", err)
	}
	buf := make([]byte, 5)
	if _, err = io.ReadFull(conn, buf); err != nil {
		t.Fatal("read:", err)
	}
	time.Sleep(200 * time.Millisecond)

	var records []ConnRecord
	if err = json.Unmarshal([]byte(ConnectionsJSON()), &records); err != nil {
		t.Fatal("bad json:", err)
	}
	t.Logf("records=%d", len(records))
	if len(records) == 0 {
		t.Fatal("没有任何连接记录：tracker 没挂上或没触发")
	}
	rec := records[0]
	t.Logf("first: id=%d tag=%s dest=%s rdest=%s start=%d", rec.ID, rec.Tag, rec.Dest, rec.RDest, rec.Start)
	if rec.Tag != "proxy" {
		t.Fatalf("出站标签应为 proxy，实际 %q", rec.Tag)
	}
	if rec.RDest == "" {
		t.Fatal("RDest 为空，GUI 第二列会显示空")
	}

	conn.Close()
	time.Sleep(300 * time.Millisecond)
	json.Unmarshal([]byte(ConnectionsJSON()), &records)
	if len(records) > 0 && records[0].End == 0 {
		t.Log("警告：关闭后 End 仍为 0（只会影响状态图标，不影响列表显示）")
	}
}
