//go:build !darwin

package neko_headroom

import N "github.com/sagernet/sing/common/network"

// 非 macOS 平台不需要这个修补：TUN 是内核自建的单进程模式，回程不经过 SOCKS
// UDP 客户端。原样返回，保证 Windows / 其他平台行为完全不变。
func Ensure(destination N.PacketWriter) N.PacketWriter {
	return destination
}
