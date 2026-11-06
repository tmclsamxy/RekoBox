package neko

import (
	"context"
	"encoding/json"
	"net"
	"strconv"
	"sync"
	"time"

	"github.com/sagernet/sing-box/adapter"
	N "github.com/sagernet/sing/common/network"
)

// 连接记录：给 GUI 的「连接」页使用。
//
// sing-box 1.12 的 ClashServer 接口不再暴露连接列表（只有 ConnectionTracker），
// 所以这里自己挂一个 tracker 记录，避免依赖 clash api 的 HTTP 端口。
// 字段名与 GUI 侧的解析保持一致（Refresh_connection_list 读 Tag/ID/Start/End/Dest/RDest）。

type ConnRecord struct {
	ID       uint64 `json:"ID"`
	Tag      string `json:"Tag"`
	Start    int64  `json:"Start"`
	End      int64  `json:"End"`
	Dest     string `json:"Dest"`
	RDest    string `json:"RDest"`
	Network  string `json:"Network"`
	Inbound  string `json:"Inbound"`
	Uplink   int64  `json:"Uplink"`
	Downlink int64  `json:"Downlink"`
}

// 只保留最近的记录，避免长时间运行后无限增长
const maxConnRecords = 300

type connStore struct {
	mu    sync.Mutex
	seq   uint64
	order []uint64
	conns map[uint64]*ConnRecord
}

func newConnStore() *connStore {
	return &connStore{conns: make(map[uint64]*ConnRecord)}
}

func (s *connStore) reset() {
	s.mu.Lock()
	s.seq = 0
	s.order = nil
	s.conns = make(map[uint64]*ConnRecord)
	s.mu.Unlock()
}

func (s *connStore) add(metadata adapter.InboundContext, outboundTag string) uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.seq++
	id := s.seq

	dest := metadata.Domain
	if dest == "" {
		dest = metadata.Destination.AddrString()
	}
	record := &ConnRecord{
		ID:      id,
		Tag:     outboundTag,
		Start:   time.Now().Unix(),
		Dest:    dest,
		RDest:   metadata.Destination.String(),
		Network: metadata.Network,
		Inbound: metadata.Inbound,
	}
	s.conns[id] = record
	s.order = append(s.order, id)

	if len(s.order) > maxConnRecords {
		drop := len(s.order) - maxConnRecords
		for _, oldID := range s.order[:drop] {
			delete(s.conns, oldID)
		}
		s.order = append([]uint64(nil), s.order[drop:]...)
	}
	return id
}

func (s *connStore) close(id uint64) {
	s.mu.Lock()
	if record, ok := s.conns[id]; ok && record.End == 0 {
		record.End = time.Now().Unix()
	}
	s.mu.Unlock()
}

func (s *connStore) snapshot() []ConnRecord {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]ConnRecord, 0, len(s.order))
	// 新的在前
	for i := len(s.order) - 1; i >= 0; i-- {
		if record, ok := s.conns[s.order[i]]; ok {
			out = append(out, *record)
		}
	}
	return out
}

var defaultConnStore = newConnStore()

// ConnectionsJSON 返回给 GUI 的连接列表（JSON 数组）
func ConnectionsJSON() string {
	data, err := json.Marshal(defaultConnStore.snapshot())
	if err != nil {
		return "[]"
	}
	return string(data)
}

type connTracker struct {
	store *connStore
}

func (t *connTracker) RoutedConnection(ctx context.Context, conn net.Conn, metadata adapter.InboundContext, matchedRule adapter.Rule, matchOutbound adapter.Outbound) net.Conn {
	tag := ""
	if matchOutbound != nil {
		tag = matchOutbound.Tag()
	}
	id := t.store.add(metadata, tag)
	return &trackedConn{Conn: conn, store: t.store, id: id}
}

func (t *connTracker) RoutedPacketConnection(ctx context.Context, conn N.PacketConn, metadata adapter.InboundContext, matchedRule adapter.Rule, matchOutbound adapter.Outbound) N.PacketConn {
	tag := ""
	if matchOutbound != nil {
		tag = matchOutbound.Tag()
	}
	id := t.store.add(metadata, tag)
	return &trackedPacketConn{PacketConn: conn, store: t.store, id: id}
}

type trackedConn struct {
	net.Conn
	store *connStore
	id    uint64
	once  sync.Once
}

func (c *trackedConn) Close() error {
	c.once.Do(func() { c.store.close(c.id) })
	return c.Conn.Close()
}

type trackedPacketConn struct {
	N.PacketConn
	store *connStore
	id    uint64
	once  sync.Once
}

func (c *trackedPacketConn) Close() error {
	c.once.Do(func() { c.store.close(c.id) })
	return c.PacketConn.Close()
}

// 便于以后按 id 断开连接时使用
func connIDString(id uint64) string {
	return strconv.FormatUint(id, 10)
}
