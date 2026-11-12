#include "SyncApi.hpp"

#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace NekoGui_sync {

    SyncApi::SyncApi(QObject *parent) : QObject(parent) {}

    void SyncApi::setBaseUrl(const QString &url) {
        baseUrl = url;
        while (baseUrl.endsWith('/')) baseUrl.chop(1);
    }

    void SyncApi::setToken(const QString &t) {
        token = t;
    }

    void SyncApi::setProxy(bool enabled, int port, const QString &user, const QString &password) {
        proxyEnabled = enabled && port > 0;
        proxyPort = port;
        proxyUser = user;
        proxyPassword = password;
    }

    void SyncApi::get(const QString &path, const ApiCallback &cb) {
        request("GET", path, {}, cb);
    }

    void SyncApi::post(const QString &path, const QJsonObject &body, const ApiCallback &cb) {
        request("POST", path, body, cb);
    }

    void SyncApi::put(const QString &path, const QJsonObject &body, const ApiCallback &cb) {
        request("PUT", path, body, cb);
    }

    void SyncApi::request(const QByteArray &verb, const QString &path, const QJsonObject &body, const ApiCallback &cb) {
        if (proxyEnabled) {
            // 本地内核是 mixed 入站，同时支持 HTTP 与 SOCKS5，这里用 HTTP CONNECT
            QNetworkProxy proxy(QNetworkProxy::HttpProxy, "127.0.0.1", static_cast<quint16>(proxyPort));
            if (!proxyUser.isEmpty()) {
                proxy.setUser(proxyUser);
                proxy.setPassword(proxyPassword);
            }
            nam.setProxy(proxy);
        } else {
            // 明确不走系统/应用代理由此保证「不连代理也能同步」
            nam.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }

        QNetworkRequest req{QUrl(baseUrl + path)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("User-Agent", "RekoBox-Windows-Sync/1");
        if (!token.isEmpty()) {
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        }
        req.setTransferTimeout(20000);

        QNetworkReply *reply = nullptr;
        if (verb == "GET") {
            reply = nam.get(req);
        } else {
            const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
            reply = verb == "POST" ? nam.post(req, payload) : nam.put(req, payload);
        }

        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            ApiResult r;
            r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto data = reply->readAll();
            if (!data.isEmpty()) r.body = QJsonDocument::fromJson(data).object();

            if (r.status == 0) {
                r.error = reply->errorString();
                switch (reply->error()) {
                    case QNetworkReply::ConnectionRefusedError:
                        r.error = QStringLiteral("连接被拒绝：服务器未监听该端口，或地址写错");
                        break;
                    case QNetworkReply::HostNotFoundError:
                        r.error = QStringLiteral("找不到主机，请检查服务器地址");
                        break;
                    case QNetworkReply::TimeoutError:
                    case QNetworkReply::OperationCanceledError:
                        r.error = QStringLiteral("连接超时，请检查网络或服务器状态");
                        break;
                    case QNetworkReply::RemoteHostClosedError:
                        r.error = QStringLiteral("服务器关闭了连接");
                        break;
                    default:
                        break;
                }
            } else if (!r.ok()) {
                const auto err = r.body.value("error").toObject();
                r.code = err.value("code").toString();
                r.error = err.value("message").toString();
                if (r.error.isEmpty()) r.error = QStringLiteral("服务端返回 HTTP %1").arg(r.status);
            }

            reply->deleteLater();
            if (cb) cb(r);
        });
    }

} // namespace NekoGui_sync
