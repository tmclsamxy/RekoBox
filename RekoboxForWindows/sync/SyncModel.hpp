#pragma once

#include <QJsonObject>
#include <QString>

// 订阅用的小工具：跨设备稳定标识、时间戳、内容指纹。
// 纯函数，不碰磁盘、网络与 UI，方便单独编译验证。

namespace NekoGui_sync {

    // 新 UUID（v4 形态的随机串）。用作实体的 sync_id：
    // 本地 int id 会在不同设备间撞车，也会被 -flag_reorder 重排，不能当稳定标识。
    [[nodiscard]] QString NewSyncId();

    [[nodiscard]] qint64 NowMs();

    // 稳定的内容指纹（键排序后序列化再哈希），用于判断"这条到底变没变"。
    [[nodiscard]] QString ItemHash(const QJsonObject &data);

} // namespace NekoGui_sync
