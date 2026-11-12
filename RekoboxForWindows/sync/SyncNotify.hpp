#pragma once

// 只声明「本地数据变了」这一个入口，避免 db 层为了一个回调去包含整个同步引擎。
// 实现见 sync/SyncEngine.cpp。

namespace NekoGui_sync {
    void NotifyLocalChange();
} // namespace NekoGui_sync
