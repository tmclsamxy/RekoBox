#include "sys/macos/MacPlatform.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QProcess>
#include <QUdpSocket>
#include <QStandardPaths>
#include <QStyle>
#include <QElapsedTimer>
#include <QStyleFactory>
#include <QThread>

// CGPreflightScreenCaptureAccess / CGRequestScreenCaptureAccess（macOS 10.15+）。
// 只在 CoreGraphics 里有声明，没有 ObjC 依赖，所以这个 .cpp 不用改成 .mm。
#include <CoreGraphics/CoreGraphics.h>

namespace {
    constexpr auto DNS_STATE_FILE = "tun-dns-state.json";
    // 与 res/vpn/vpn-run-root-macos.sh 里的路径必须一致（同一个 config 目录）
    constexpr auto TUN_PID_FILE = "tun-root.pid";
    constexpr auto TUN_STOP_REQUEST_FILE = "tun-stop-request";
    // 与安卓端一致：VpnService.addDnsServer(PRIVATE_VLAN4_ROUTER)，值就是 172.19.0.2。
    // 历史方案：把系统 DNS 指向它，指望"落在 tun 网段所以会进隧道"。
    //
    // 实测不成立（P1-8 的根因）：utun 上并没有服务监听 172.19.0.2:53，内核把发往
    // 同网段地址的包直接投递给接口（当作直连邻居），根本走不到
    // "port 53 → dns-out" 那条规则 → 探活恒失败 → 系统 DNS 从未被真正接管。
    //
    // 现在的做法见 db/ConfigBuilder.cpp 里的 dns-in 入站：tun 内核（root）自己监听
    // 127.0.0.1:53，系统 DNS 指向 127.0.0.1。所以 TUN_DNS_ADDRESS 只用于还原旧快照
    // 与兼容老版本留下的 tun-dns-state.json，正常路径不再使用它。
    constexpr auto TUN_DNS_ADDRESS = "172.19.0.2";
    // 探测不到任何可用上游时的兜底（阿里 DNS，国内可用性最好）
    constexpr auto FALLBACK_DNS = "223.5.5.5";

    QString ConfigDir() {
        // 运行期 cwd 就是 config 目录（main.cpp 里 setCurrent 过）
        auto dir = QDir::current();
        if (!dir.absolutePath().isEmpty() && dir.exists()) return dir.absolutePath();
        return QDir(NekoGui_mac::DataDir()).absoluteFilePath("config");
    }

    QString DnsStatePath() {
        return QDir(ConfigDir()).absoluteFilePath(DNS_STATE_FILE);
    }

    QString NetSet(const QStringList &args) {
        QProcess p;
        p.start("/usr/sbin/networksetup", args);
        if (!p.waitForFinished(15000)) {
            p.kill();
            p.waitForFinished(1000);
            return {};
        }
        return QString::fromUtf8(p.readAllStandardOutput());
    }

    // 除 "*" 前缀（停用）以外的网络服务；第一行是说明文字，跳过
    QStringList NetworkServices() {
        QStringList result;
        const auto lines = NetSet({"-listallnetworkservices"}).split('\n', Qt::SkipEmptyParts);
        for (int i = 1; i < lines.size(); i++) {
            const auto name = lines[i].trimmed();
            if (name.isEmpty() || name.startsWith("*")) continue;
            result << name;
        }
        return result;
    }
} // namespace

namespace NekoGui_mac {

    QString DataDir() {
        // AppDataLocation = ~/Library/Application Support/<applicationName>
        // applicationName 在 main.cpp 里被设为 "rekobox"
        auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dir.isEmpty()) {
            dir = QDir::homePath() + "/Library/Application Support/rekobox";
        }
        return dir;
    }

    void ApplyLook() {
        // macOS 原生 style 会把按钮压到 22px、下拉换成原生外观，和 Windows 版差一截。
        // 固定 Fusion 之后，同一份 QSS 在两边的观感才一致。
        if (auto fusion = QStyleFactory::create("Fusion")) {
            QApplication::setStyle(fusion);
        }

        // 字体不写死族名：沿用系统 UI 字体（SF Pro / PingFang SC），只把字号定到 13pt。
        // QSS 里的 feiyangqingyun 主题会带自己的 font-family，这里作为兜底。
        auto font = QApplication::font();
        font.setPointSize(13);
        QApplication::setFont(font);
    }

    QString ShellQuote(const QString &s) {
        QString out = s;
        out.replace("'", "'\\''");
        return "'" + out + "'";
    }

    QString AppleScriptString(const QString &s) {
        QString out = s;
        out.replace("\\", "\\\\");
        out.replace("\"", "\\\"");
        return out;
    }

    QString TunDNSUpstream() {
        QStringList found;

        // 1) 用户在网络设置里显式配过 DNS 的，优先用（公司/局域网 DNS 往往只有内网能解析）
        for (const auto &service: NetworkServices()) {
            auto out = NetSet({"-getdnsservers", service});
            if (out.contains("aren't any")) continue;
            for (const auto &line: out.split('\n', Qt::SkipEmptyParts)) {
                const auto v = line.trimmed();
                if (!v.isEmpty()) found << v;
            }
        }

        // 2) 没显式配（走 DHCP）就取系统当前解析器
        if (found.isEmpty()) {
            QProcess p;
            p.start("/usr/sbin/scutil", {"--dns"});
            if (p.waitForFinished(5000)) {
                for (const auto &line: QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
                    const auto l = line.trimmed();
                    // nameserver[0] 常常是 fe80 链路本地，IPv4 往往在 [1]，所以全取
                    if (!l.startsWith("nameserver[")) continue;
                    const auto v = l.section(':', 1).trimmed();
                    if (!v.isEmpty()) found << v;
                }
            }
        }

        // 3) 挑一个能用的：跳过回环（那会自环）和链路本地，IPv4 优先
        QString ipv6;
        for (auto v: found) {
            v = v.trimmed();
            if (v.isEmpty() || v == "127.0.0.1" || v == "::1") continue;
            if (v.startsWith("fe80")) continue; // 链路本地需要 scope，sing-box 里不好写
            QHostAddress addr(v);
            if (addr.isNull()) continue;
            if (addr.protocol() == QAbstractSocket::IPv4Protocol) return v;
            if (ipv6.isEmpty()) ipv6 = v;
        }
        if (!ipv6.isEmpty()) return ipv6;
        return QString::fromLatin1(FALLBACK_DNS);
    }

    bool ProbeDNS(const QString &server, int port, int timeoutMs) {
        // 最小 DNS 查询：ID(2) + flags(2) + QDCOUNT(1) + 保留(6) + example.com A IN
        QByteArray query;
        query.append(char(0xAB)).append(char(0xCD)); // ID，用来认回包
        query.append(char(0x01)).append(char(0x00)); // 标准查询 + 递归
        query.append(char(0x00)).append(char(0x01)); // QDCOUNT = 1
        query.append(6, '\0');                      // AN / NS / AR = 0
        query.append(char(7)).append("example", 7);
        query.append(char(3)).append("com", 3);
        query.append(char(0));
        query.append(char(0x00)).append(char(0x01)); // QTYPE = A
        query.append(char(0x00)).append(char(0x01)); // QCLASS = IN

        QUdpSocket sock;
        sock.writeDatagram(query, QHostAddress(server), quint16(port));
        if (!sock.waitForReadyRead(timeoutMs)) return false;
        QByteArray reply;
        reply.resize(int(sock.pendingDatagramSize()));
        const auto n = sock.readDatagram(reply.data(), reply.size());
        return n >= 12 && quint8(reply[0]) == 0xAB && quint8(reply[1]) == 0xCD;
    }

    void ApplyTunDNS(bool enable) {
        ApplyTunDNS(enable, TUN_DNS_ADDRESS);
    }

    void ApplyTunDNS(bool enable, const QString &address) {
        const auto statePath = DnsStatePath();

        if (!enable) {
            QFile f(statePath);
            if (!f.exists()) return;
            if (!f.open(QIODevice::ReadOnly)) {
                qDebug() << "[mac] cannot read DNS state:" << f.errorString();
                return;
            }
            const auto doc = QJsonDocument::fromJson(f.readAll());
            f.close();

            for (const auto &v: doc.object()["services"].toArray()) {
                const auto obj = v.toObject();
                const auto service = obj["service"].toString();
                if (service.isEmpty()) continue;
                QStringList dns;
                for (const auto &d: obj["dns"].toArray()) dns << d.toString();
                // 原本没有显式 DNS（走 DHCP）时用 Empty 还原，别写死一个 IP
                NetSet(QStringList{"-setdnsservers", service} + (dns.isEmpty() ? QStringList{"Empty"} : dns));
            }
            QFile::remove(statePath);
            qDebug() << "[mac] system DNS restored";
            return;
        }

        // 已经接管过就别再抓一次：否则会把 tun 地址当成"用户原本的 DNS"存下来，
        // 还原时写回接管地址，TUN 一停就整机解析不了
        if (QFile::exists(statePath)) {
            qDebug() << "[mac] DNS already taken over, keep the original snapshot";
            return;
        }

        QJsonArray snapshot;
        for (const auto &service: NetworkServices()) {
            auto out = NetSet({"-getdnsservers", service});
            QStringList dns;
            // 没设置过时 networksetup 回 "There aren't any DNS Servers set on <service>."
            if (!out.contains("aren't any")) {
                for (const auto &line: out.split('\n', Qt::SkipEmptyParts)) {
                    const auto v = line.trimmed();
                    if (!v.isEmpty()) dns << v;
                }
            }
            snapshot.append(QJsonObject{{"service", service},
                                        {"dns", QJsonArray::fromStringList(dns)}});
        }

        QFile f(statePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qDebug() << "[mac] cannot write DNS state:" << f.errorString();
            return;
        }
        f.write(QJsonDocument(QJsonObject{{"services", snapshot}}).toJson(QJsonDocument::Indented));
        f.close();

        for (const auto &service: NetworkServices()) {
            NetSet({"-setdnsservers", service, address});
        }
        qDebug() << "[mac] system DNS ->" << address;
    }

    void RecoverTunDNSIfNeeded() {
        // 只有非正常退出才会留下快照：正常停 TUN / 退出程序都会还原并删掉
        if (!QFile::exists(DnsStatePath())) return;
        qDebug() << "[mac] leftover tun DNS detected, restoring before startup";
        ApplyTunDNS(false);
    }

    bool HasScreenCaptureAccess() {
        // 注意：CGPreflightScreenCaptureAccess 在 10.15 之前不存在，但部署目标是 13.0，
        // 所以这里不需要再做版本判断。
        return CGPreflightScreenCaptureAccess();
    }

    bool RequestScreenCaptureAccess() {
        // 弹系统授权框。用户点了"允许"也只是把 App 加进列表，**本进程仍然拿不到权限**，
        // 必须重启 App 才生效——调用方必须把这句话告诉用户。
        return CGRequestScreenCaptureAccess();
    }

    // ------------------------------------------------------------------
    // TUN 停止握手（P0-3）
    // ------------------------------------------------------------------

    qint64 ReadTunRootPid() {
        QFile f(QDir(ConfigDir()).absoluteFilePath(TUN_PID_FILE));
        if (!f.open(QIODevice::ReadOnly)) return 0;
        bool ok = false;
        const auto pid = f.readAll().trimmed().toLongLong(&ok);
        return (ok && pid > 0) ? pid : 0;
    }

    bool RequestTunStopBySentinel() {
        // 哨兵（vpn-run-root-macos.sh 里的 while 循环）以 root 跑，它看到这个文件
        // 就会自己 SIGTERM 内核。GUI 只负责建个文件——用户对自己的数据目录有写权限，
        // 所以这里**不需要任何提权**，也就不会再弹密码（这正是 P0-3 的修法）。
        //
        // 文件内容必须是**当前内核 pid**：脚本会校验，只认自己的。
        // 这样即使上一次崩溃留下旧标记，也不会把新内核刚起来就杀掉（实测踩过）。
        const auto pid = ReadTunRootPid();
        if (pid == 0) return false; // 哨兵没在跑（旧脚本/已被强杀）

        QFile f(QDir(ConfigDir()).absoluteFilePath(TUN_STOP_REQUEST_FILE));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qDebug() << "[mac] cannot create stop request:" << f.errorString();
            return false;
        }
        f.write(QByteArray::number(pid));
        f.close();
        return true;
    }

    bool WaitTunExit(int timeoutMs) {
        // 哨兵收到请求后：SIGTERM → 最多等 6 秒 → SIGKILL，然后删 pid 文件。
        // 所以"pid 文件消失"就是"内核确实退了"的信号。
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            if (ReadTunRootPid() == 0) return true;
            QThread::msleep(100);
        }
        return ReadTunRootPid() == 0;
    }

} // namespace NekoGui_mac
