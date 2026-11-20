// Package neko_headroom 修补 sing v0.7.18 的 UDP 回程崩溃（仅 macOS 生效）。
//
// 现象（macOS TUN 两进程模型下必现）：
//
//	panic: buffer overflow: capacity 16384,start 0, need 10
//	github.com/sagernet/sing/protocol/socks.(*AssociatePacketConn).WritePacket
//
// 原因：sing 的 bufio.copyPacketWaitWithPool（PacketReadWaiter 快路径）直接把
// ReadWaiter 交回的 buffer 传给 WritePacket，**没有**按 destination 的需求预留
// 头部空间（对比 CopyPacketWithPool 走 ReadWaitOptions.NewPacketBuffer 就会预留）。
// SOCKS5 UDP 回程要在包前面塞 3 + addrport（IPv4 共 10 字节）的头，buffer 的
// start 是 0，AssociatePacketConn 里的 ExtendHeader 直接 panic。
//
// 为什么是 mac 必现：tun 内核用 socks 出站连主内核，主内核把 VLESS 的下行 UDP
// 写回给这个 socks 客户端——进程间只能走 socks，于是回程必然经过
// AssociatePacketConn.WritePacket。Windows 用内核自建 tun（单进程），没有这一跳。
//
// 为什么不能靠 CalculateFrontHeadroom 判断：这条链上 destination 的实际类型是
// *canceler.TimeoutPacketConn / *bufio.ExtendedUDPConn，中间的包装层没有透传
// Upstream()，算出来是 0（实测探针），所以 mac 上直接给一个下限。
//
// 计数不影响：内层 CounterPacketConn.WritePacket 自己计数，本包不实现
// PacketWriteCounter，拷贝循环也就不会重复计数。
package neko_headroom

import (
	"github.com/sagernet/sing/common/buf"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
)

// SOCKS5 UDP 头最长 3 + 1 + 16 + 2 = 22 字节，留一点余量
const minHeadroom = 32

type writer struct {
	N.PacketWriter
	headroom int
}

// Ensure 包一层，保证写之前 buffer 头部至少有 headroom 字节空位。
func Ensure(destination N.PacketWriter) N.PacketWriter {
	if destination == nil {
		return nil
	}
	headroom := N.CalculateFrontHeadroom(destination)
	if headroom < minHeadroom {
		headroom = minHeadroom
	}
	return &writer{PacketWriter: destination, headroom: headroom}
}

func (w *writer) WritePacket(buffer *buf.Buffer, destination M.Socksaddr) error {
	if buffer.Start() < w.headroom {
		// 复制到前面留了空位的 buffer：start 推到 headroom，数据长度不变
		newBuffer := buf.NewSize(buffer.Len() + w.headroom)
		newBuffer.Resize(w.headroom, 0)
		newBuffer.Write(buffer.Bytes())
		buffer.Release()
		buffer = newBuffer
	}
	return w.PacketWriter.WritePacket(buffer, destination)
}

// Upstream 让外层（计数、超时包装）继续能找到内层
func (w *writer) Upstream() any {
	return w.PacketWriter
}
