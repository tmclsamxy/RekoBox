#include "SyncModel.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

namespace NekoGui_sync {

    QString NewSyncId() {
        // QUuid 生成 v4；toJson 里去掉花括号
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    qint64 NowMs() {
        return QDateTime::currentMSecsSinceEpoch();
    }

    QString ItemHash(const QJsonObject &data) {
        // QJsonObject 的键在序列化时按字典序输出，同一份内容哈希稳定
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
    }

} // namespace NekoGui_sync
