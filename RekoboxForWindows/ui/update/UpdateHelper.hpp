#pragma once

#include <QWidget>

namespace NekoGui_update {

    /**
     * 检查更新。
     *
     * @param manual true：用户主动点「检查更新」，无更新/失败都会提示；
     *               false：启动时静默检查，只有发现新版本才弹窗。
     *
     * 网络优先走内核的 mixed 入站（用户当前选中的节点），失败回退直连。
     */
    void CheckForUpdates(QWidget *parent, bool manual);

} // namespace NekoGui_update
