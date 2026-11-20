#include "sys/macos/MacPlatform.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>

namespace {
    constexpr auto DNS_STATE_FILE = "tun-dns-state.json";
    // 必须和 res/vpn/sing-box-vpn.json 里的 inet4_address 一致（172.19.0.1/28）：
    // sing-box 的 tun 会接住发往这个地址的 53 端口查询，交给 dns-out 走 fakeip
    constexpr auto TUN_DNS_ADDRESS = "172.19.0.1";

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

    void ApplyTunDNS(bool enable) {
        const auto statePath = DnsStatePath();

        if (enable) {
            // 已经接管过就别再抓一次：否则会把 tun 地址当成"用户原本的 DNS"存下来，
            // 还原时写回 172.19.0.1，TUN 一停就整机解析不了（和系统代理那边同一个坑）
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
            qDebug() << "[mac] system DNS ->" << TUN_DNS_ADDRESS << "(snapshot:" << statePath << ")";
        } else {
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
        }
    }

    void RecoverTunDNSIfNeeded() {
        // 只有非正常退出才会留下文件：正常停 TUN / 退出程序都会还原并删掉
        if (!QFile::exists(DnsStatePath())) return;
        qDebug() << "[mac] leftover tun DNS detected, restoring before startup";
        ApplyTunDNS(false);
    }

} // namespace NekoGui_mac
