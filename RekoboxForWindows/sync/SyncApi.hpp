#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include <functional>

// 极薄的 HTTP 包装：拼接 baseUrl、带 Bearer 头、统一错误文案、统一 20s 超时。
// 全部异步，回调在 GUI 线程执行。

namespace NekoGui_sync {

    struct ApiResult {
        int status = 0;      // HTTP 状态码；0 表示根本没拿到响应（DNS/连接/超时）
        QJsonObject body;
        QString error;       // 可直接展示给用户的错误
        QString code;        // 服务端错误码，如 bad_credentials / version_conflict

        [[nodiscard]] bool ok() const { return status >= 200 && status < 300; }
        [[nodiscard]] bool networkError() const { return status == 0; }
    };

    using ApiCallback = std::function<void(const ApiResult &)>;

    class SyncApi : public QObject {
    public:
        explicit SyncApi(QObject *parent = nullptr);

        void setBaseUrl(const QString &url);

        void setToken(const QString &token);

        void setProxy(bool enabled, int port, const QString &user, const QString &password);

        void get(const QString &path, const ApiCallback &cb);

        void post(const QString &path, const QJsonObject &body, const ApiCallback &cb);

        void put(const QString &path, const QJsonObject &body, const ApiCallback &cb);

    private:
        void request(const QByteArray &verb, const QString &path, const QJsonObject &body, const ApiCallback &cb);

        QNetworkAccessManager nam;
        QString baseUrl;
        QString token;
        bool proxyEnabled = false;
        int proxyPort = 0;
        QString proxyUser;
        QString proxyPassword;
    };

} // namespace NekoGui_sync
