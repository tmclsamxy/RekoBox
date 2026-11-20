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
#include <QStyleFactory>

namespace {
    constexpr auto DNS_STATE_FILE = "tun-dns-state.json";
    // 与安卓端一致：VpnService.addDnsServer(PRIVATE_VLAN4_ROUTER)，值就是 172.19.0.2。
    // 必须落在 tun 的 inet4_address 网段内（172.19.0.1/28），且不能是接口地址本身。
    constexpr auto TUN_DNS_ADDRESS = "172.19.0.2";
    // 探测不到任何可用上游时的兜底（阿里 DNS，国内可用性最好）
    constexpr auto FALLBACK_DNS = "223.5.5.5";

    QString DnsStatePath() {
        // 运行期 cwd 就是 config 目录（main.cpp 里 setCurrent 过）
        auto dir = QDir::current();
        if (!dir.absolutePath().isEmpty() && dir.exists()) {
            return dir.absoluteFilePath(DNS_STATE_FILE);
        }
        return QDir(NekoGui_mac::DataDir()).absoluteFilePath(DNS_STATE_FILE);
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
        // 还原时写回 172.19.0.2，TUN 一停就整机解析不了
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
            NetSet({"-setdnsservers", service, TUN_DNS_ADDRESS});
        }
        qDebug() << "[mac] system DNS ->" << TUN_DNS_ADDRESS;
    }

    void RecoverTunDNSIfNeeded() {
        // 只有非正常退出才会留下快照：正常停 TUN / 退出程序都会还原并删掉
        if (!QFile::exists(DnsStatePath())) return;
        qDebug() << "[mac] leftover tun DNS detected, restoring before startup";
        ApplyTunDNS(false);
    }

} // namespace NekoGui_mac
