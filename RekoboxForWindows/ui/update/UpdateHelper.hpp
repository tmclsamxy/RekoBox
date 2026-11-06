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

    /**
     * 启动时检查上一次更新是否失败（替换脚本会写 %TEMP%\rekobox-update.log）。
     * 失败时弹窗告知并给出日志内容，避免用户以为已经升级、实际还是旧版。
     */
    void CheckLastUpdateResult(QWidget *parent);

} // namespace NekoGui_update
