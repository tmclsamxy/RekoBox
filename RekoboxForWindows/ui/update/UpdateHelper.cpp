#include "ui/update/UpdateHelper.hpp"

#include "main/NekoGui.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace NekoGui_update {

    namespace {

        // 仓库同时有 Android（v1.0.1）与 Windows（win-v1.0.4）两条发布线，
        // tag 前缀是唯一可靠的区分方式；/releases/latest 会返回另一平台的包。
        constexpr auto kTagPrefix = "win-v";
        constexpr auto kApiReleases = "https://api.github.com/repos/tmclsamxy/RekoBox/releases?per_page=30";
        constexpr auto kAssetKeyword = "windows-x64-qt.zip";

        // 允许用环境变量覆盖接口地址：便于本地起一个假的 release 列表做端到端测试，
        // 默认仍走 GitHub。
        QString apiUrl() {
            const auto override = qEnvironmentVariable("REKOBOX_UPDATE_API");
            return override.isEmpty() ? QString(kApiReleases) : override;
        }

        struct ReleaseInfo {
            QString version;
            QString tag;
            QString notes;
            QString pageUrl;
            QString assetName;
            QString assetUrl;
            qint64 assetSize = 0;
        };

        // 只比数字段，缺位补 0：1.0.10 > 1.0.9
        int compareVersion(const QString &a, const QString &b) {
            const auto pa = a.split(".");
            const auto pb = b.split(".");
            const int n = std::max(pa.size(), pb.size());
            for (int i = 0; i < n; i++) {
                const int x = i < pa.size() ? pa[i].toInt() : 0;
                const int y = i < pb.size() ? pb[i].toInt() : 0;
                if (x != y) return x - y;
            }
            return 0;
        }

        QString versionFromTag(const QString &tag) {
            if (!tag.startsWith(kTagPrefix)) return {};
            const auto v = tag.mid(QString(kTagPrefix).size());
            if (v.isEmpty() || !v[0].isDigit()) return {};
            return v;
        }

        QString readableSize(qint64 bytes) {
            if (bytes >= 1024 * 1024) return QString::number(bytes / 1048576.0, 'f', 1) + " MB";
            if (bytes >= 1024) return QString::number(bytes / 1024.0, 'f', 0) + " KB";
            return QString::number(bytes) + " B";
        }

        class Updater : public QObject {
        public:
            Updater(QWidget *parent, bool manual)
                : QObject(parent), parent(parent), manual(manual) {}

            ~Updater() override {
                if (downloadFile != nullptr) {
                    downloadFile->close();
                    delete downloadFile;
                    downloadFile = nullptr;
                }
            }

            void start() {
                tryFetch(true);
            }

        private:
            QWidget *parent;
            bool manual;
            bool triedDirect = false;
            QNetworkAccessManager nam;
            QNetworkReply *reply = nullptr;
            ReleaseInfo update;
            QString tempDir;
            QProgressDialog *progress = nullptr;
            QFile *downloadFile = nullptr;

            void applyProxy(bool viaProxy) {
                if (!viaProxy) {
                    nam.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
                    return;
                }
                const int port = NekoGui::dataStore->inbound_socks_port;
                if (port <= 0) {
                    nam.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
                    return;
                }
                QNetworkProxy proxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", static_cast<quint16>(port));
                if (NekoGui::dataStore->inbound_auth->NeedAuth()) {
                    proxy.setUser(NekoGui::dataStore->inbound_auth->username);
                    proxy.setPassword(NekoGui::dataStore->inbound_auth->password);
                }
                nam.setProxy(proxy);
            }

            void tryFetch(bool viaProxy) {
                applyProxy(viaProxy);
                QNetworkRequest request{QUrl(apiUrl())};
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QString("RekoBox/%1").arg(QString(NKR_VERSION)));
                request.setRawHeader("Accept", "application/vnd.github+json");

                reply = nam.get(request);
                connect(reply, &QNetworkReply::finished, this, [this] {
                    reply->deleteLater();
                    if (reply->error() != QNetworkReply::NoError) {
                        const auto err = reply->errorString();
                        reply = nullptr;
                        // 未连接时代理端口不监听，第一次必然失败：回退直连再试一次
                        if (!triedDirect) {
                            triedDirect = true;
                            tryFetch(false);
                            return;
                        }
                        if (manual) {
                            QMessageBox::warning(parent, QObject::tr("检查更新"),
                                                 QObject::tr("无法连接更新服务器：%1").arg(err));
                        }
                        deleteLater();
                        return;
                    }
                    const auto body = reply->readAll();
                    reply = nullptr;
                    handleReleases(body);
                });
            }

            void handleReleases(const QByteArray &body) {
                const auto doc = QJsonDocument::fromJson(body);
                if (!doc.isArray()) {
                    fail(QObject::tr("更新服务器返回了无法识别的内容。"));
                    return;
                }

                ReleaseInfo best;
                bool found = false;
                for (const auto &item: doc.array()) {
                    const auto obj = item.toObject();
                    if (obj["draft"].toBool()) continue;
                    const auto version = versionFromTag(obj["tag_name"].toString());
                    if (version.isEmpty()) continue;
                    if (!found || compareVersion(version, best.version) > 0) {
                        ReleaseInfo candidate;
                        candidate.version = version;
                        candidate.tag = obj["tag_name"].toString();
                        candidate.notes = obj["body"].toString().trimmed();
                        candidate.pageUrl = obj["html_url"].toString();
                        for (const auto &a: obj["assets"].toArray()) {
                            const auto asset = a.toObject();
                            const auto name = asset["name"].toString();
                            if (name.contains(kAssetKeyword, Qt::CaseInsensitive)) {
                                candidate.assetName = name;
                                candidate.assetUrl = asset["browser_download_url"].toString();
                                candidate.assetSize = static_cast<qint64>(asset["size"].toDouble());
                                break;
                            }
                        }
                        if (candidate.assetUrl.isEmpty()) continue;
                        best = candidate;
                        found = true;
                    }
                }

                if (!found) {
                    if (manual) {
                        QMessageBox::information(parent, QObject::tr("检查更新"),
                                                 QObject::tr("没有可用的更新。"));
                    }
                    deleteLater();
                    return;
                }

                if (compareVersion(best.version, QString(NKR_VERSION)) <= 0) {
                    if (manual) {
                        QMessageBox::information(parent, QObject::tr("检查更新"),
                                                 QObject::tr("当前已是最新版本（%1）。")
                                                     .arg(QString(NKR_VERSION)));
                    }
                    deleteLater();
                    return;
                }

                update = best;
                prompt();
            }

            void prompt() {
                QString text = QObject::tr("当前版本：%1\n新版本：%2（%3）\n\n是否立即下载并安装？")
                                   .arg(QString(NKR_VERSION), update.version, readableSize(update.assetSize));
                if (!update.notes.isEmpty()) {
                    auto notes = update.notes;
                    if (notes.size() > 1200) notes = notes.left(1200) + "…";
                    text += "\n\n" + notes;
                }

                QMessageBox box(parent);
                box.setWindowTitle(QObject::tr("发现新版本"));
                box.setText(text);
                box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                box.setDefaultButton(QMessageBox::Yes);
                if (box.exec() != QMessageBox::Yes) {
                    deleteLater();
                    return;
                }
                download();
            }

            void download() {
                tempDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                              .filePath("rekobox-update");
                QDir(tempDir).removeRecursively();
                QDir().mkpath(tempDir);

                progress = new QProgressDialog(QObject::tr("正在下载更新…"), QString(), 0, 100, parent);
                progress->setWindowTitle(QObject::tr("更新"));
                progress->setWindowModality(Qt::WindowModal);
                progress->setCancelButton(nullptr);
                progress->setMinimumDuration(0);
                progress->setAutoClose(false);
                progress->setValue(0);
                progress->show();

                QNetworkRequest request{QUrl(update.assetUrl)};
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QString("RekoBox/%1").arg(QString(NKR_VERSION)));

                downloadFile = new QFile(QDir(tempDir).filePath("update.zip"));
                if (!downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    delete downloadFile;
                    downloadFile = nullptr;
                    if (progress != nullptr) {
                        progress->close();
                        progress->deleteLater();
                        progress = nullptr;
                    }
                    QMessageBox::warning(parent, QObject::tr("更新"),
                                         QObject::tr("无法创建更新文件，请检查临时目录是否可写。"));
                    deleteLater();
                    return;
                }

                reply = nam.get(request);
                // 流式落盘：32 MB 一次性 readAll 会白占内存，也不利于进度反馈
                connect(reply, &QNetworkReply::readyRead, this, [this] {
                    if (downloadFile != nullptr) downloadFile->write(reply->readAll());
                });
                connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
                    if (progress == nullptr || total <= 0) return;
                    progress->setMaximum(100);
                    progress->setValue(int(received * 100 / total));
                    progress->setLabelText(
                        QString("%1 / %2  %3%")
                            .arg(readableSize(received), readableSize(total))
                            .arg(received * 100 / total));
                });
                connect(reply, &QNetworkReply::finished, this, [this] {
                    const auto error = reply->error();
                    const auto errorString = reply->errorString();

                    qint64 written = 0;
                    if (downloadFile != nullptr) {
                        downloadFile->flush();
                        written = downloadFile->size();
                        downloadFile->close();
                        delete downloadFile;
                        downloadFile = nullptr;
                    }
                    const auto expected = update.assetSize;

                    reply->deleteLater();
                    reply = nullptr;
                    if (progress != nullptr) {
                        progress->close();
                        progress->deleteLater();
                        progress = nullptr;
                    }
                    if (error != QNetworkReply::NoError) {
                        QMessageBox::warning(parent, QObject::tr("更新"),
                                             QObject::tr("下载失败：%1").arg(errorString));
                        deleteLater();
                        return;
                    }
                    if (expected > 0 && written != expected) {
                        QMessageBox::warning(
                            parent, QObject::tr("更新"),
                            QObject::tr("下载不完整：%1 / %2 字节。")
                                .arg(QString::number(written), QString::number(expected)));
                        deleteLater();
                        return;
                    }
                    applyUpdate();
                });
            }

            // 解压 zip 到 destDir，成功返回 true
            bool extractZip(const QString &zipPath, const QString &destDir) {
                const auto scriptPath = QDir(tempDir).filePath("extract.ps1");
                QFile script(scriptPath);
                if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
                script.write("\xEF\xBB\xBF");
                script.write(R"PS1(param([string]$Zip, [string]$Dest)
$ErrorActionPreference = 'Stop'
Expand-Archive -LiteralPath $Zip -DestinationPath $Dest -Force
)PS1");
                script.close();

                QProcess proc;
                proc.start("powershell",
                           {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", scriptPath,
                            zipPath, destDir});
                if (!proc.waitForFinished(180000)) {
                    proc.kill();
                    return false;
                }
                return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
            }

            void applyUpdate() {
                const auto zipPath = QDir(tempDir).filePath("update.zip");
                const auto payloadDir = QDir(tempDir).filePath("payload");
                QDir().mkpath(payloadDir);

                // 解压走 Expand-Archive：路径通过 -File 参数传递，含中文/空格都不会被
                // 代码页破坏，也不依赖系统 tar 的实现（Git for Windows 的 GNU tar 不认 zip）。
                if (!extractZip(zipPath, payloadDir)) {
                    QMessageBox::warning(parent, QObject::tr("更新"),
                                         QObject::tr("更新包解压失败。"));
                    deleteLater();
                    return;
                }

                // 包内顶层是 windows64/，兼容未来改结构：按 rekobox.exe 定位
                QString srcDir = payloadDir;
                if (!QFile::exists(QDir(srcDir).filePath("rekobox.exe"))) {
                    const auto candidates = QDir(payloadDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const auto &d: candidates) {
                        if (QFile::exists(QDir(payloadDir).filePath(d + "/rekobox.exe"))) {
                            srcDir = QDir(payloadDir).filePath(d);
                            break;
                        }
                    }
                }
                if (!QFile::exists(QDir(srcDir).filePath("rekobox.exe"))) {
                    QMessageBox::warning(parent, QObject::tr("更新"),
                                         QObject::tr("更新包中缺少 rekobox.exe。"));
                    deleteLater();
                    return;
                }

                const auto dstDir = QApplication::applicationDirPath();
                if (!QFileInfo(dstDir).isWritable()) {
                    QMessageBox::warning(
                        parent, QObject::tr("更新"),
                        QObject::tr("安装目录不可写：\n%1\n\n"
                                    "请把 RekoBox 放到可写的目录，或以管理员身份运行后重试。")
                            .arg(dstDir));
                    deleteLater();
                    return;
                }

                const auto scriptPath = QDir(tempDir).filePath("apply-update.ps1");
                QFile script(scriptPath);
                if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QMessageBox::warning(parent, QObject::tr("更新"),
                                         QObject::tr("无法写入更新脚本。"));
                    deleteLater();
                    return;
                }
                // UTF-8 BOM：PowerShell 5.1 靠它识别编码，否则含中文路径会乱码
                script.write("\xEF\xBB\xBF");
                script.write(R"PS1(param([string]$Src, [string]$Dst, [string]$Exe, [int]$AppPid)
$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'rekobox-update.log'
Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
function W($m) { "$(Get-Date -Format o) $m" | Out-File -FilePath $log -Encoding utf8 -Append }
try {
    W "start appPid=$AppPid dst=$Dst"
    # 按 PID 精确等待主进程
    if ($AppPid -gt 0) {
        try { Wait-Process -Id $AppPid -Timeout 60 -ErrorAction Stop }
        catch { W "wait-pid: $($_.Exception.Message)" }
    }
    # 内核 rekobox_core.exe 是 GUI 的子进程，GUI 退出不一定带走它。
    # 它还占着 rekobox_core.exe 与本地端口，会导致替换失败 + 新内核起不来。
    # 只处理安装目录下的进程：其它目录/用户手动开的实例不该被误杀。
    $deadline = (Get-Date).AddSeconds(20)
    while ($true) {
        $procs = Get-Process -Name 'rekobox','rekobox_core' -ErrorAction SilentlyContinue |
                 Where-Object { $_.Path -and $_.Path.StartsWith($Dst, 'OrdinalIgnoreCase') }
        if (-not $procs) { break }
        if ((Get-Date) -ge $deadline) {
            W ("force kill: " + (($procs | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ','))
            $procs | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
            break
        }
        Start-Sleep -Milliseconds 500
    }
    # 逐个探测要覆盖的可执行文件是否可写
    foreach ($name in @('rekobox.exe','rekobox_core.exe')) {
        $path = Join-Path $Dst $name
        $ready = $false
        for ($i = 0; $i -lt 30; $i++) {
            try {
                $fs = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
                $fs.Close(); $ready = $true; break
            } catch { Start-Sleep -Milliseconds 1000 }
        }
        if (-not $ready) { throw "$name still locked after 30s" }
    }

    # robocopy 对个别仍被占用的文件有内建重试，比 Copy-Item 一失败就整体放弃更稳
    $out = robocopy $Src $Dst /E /IS /IT /R:3 /W:2 /NFL /NDL /NJH /NJS 2>&1
    $rc = $LASTEXITCODE
    if ($rc -ge 8) {
        throw ("robocopy exit=$rc :: " + (($out | Select-Object -First 8) -join ' | '))
    }
    W "updated ok (robocopy exit=$rc)"
} catch {
    W "failed: $($_.Exception.Message)"
} finally {
    Start-Sleep -Milliseconds 300
    try { Start-Process -FilePath $Exe } catch { W "restart failed: $($_.Exception.Message)" }
    Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
}
)PS1");
                script.close();

                const auto exePath = QDir(dstDir).filePath("rekobox.exe");
                const bool ok = QProcess::startDetached(
                    "powershell",
                    {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", scriptPath,
                     "-Src", QDir::toNativeSeparators(srcDir),
                     "-Dst", QDir::toNativeSeparators(dstDir),
                     "-Exe", QDir::toNativeSeparators(exePath),
                     "-AppPid", QString::number(QCoreApplication::applicationPid())});
                if (!ok) {
                    QMessageBox::warning(parent, QObject::tr("更新"),
                                         QObject::tr("启动更新程序失败。"));
                    deleteLater();
                    return;
                }

                QMessageBox::information(
                    parent, QObject::tr("更新"),
                    QObject::tr("RekoBox 即将关闭，自动替换文件并重新启动。\n\n"
                                "这一步需要几秒钟，期间请不要手动打开程序。\n"
                                "如果失败，日志保存在 %%TEMP%%\\rekobox-update.log。"));
                // 走完整退出流程（停内核 / 关系统代理 / 保存配置）。
                // 直接 qApp->quit() 会把 rekobox_core.exe 留在后台，它占着
                // rekobox_core.exe 文件和本地端口，导致这次替换失败、新内核也起不来。
                if (!QMetaObject::invokeMethod(parent, "on_menu_exit_triggered", Qt::QueuedConnection)) {
                    qApp->quit();
                }
            }

            void fail(const QString &message) {
                if (manual) {
                    QMessageBox::warning(parent, QObject::tr("检查更新"), message);
                }
                deleteLater();
            }
        };

    } // namespace

    void CheckLastUpdateResult(QWidget *parent) {
        const auto logPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath("rekobox-update.log");
        QFile log(logPath);
        if (!log.open(QIODevice::ReadOnly)) return;
        const auto content = QString::fromUtf8(log.readAll()).trimmed();
        log.close();
        // 成功的日志是 "updated ok"，失败的是 "... failed: 原因"
        if (!content.contains("failed:", Qt::CaseInsensitive)) return;

        // 只提示一次，避免每次启动都弹
        QFile::remove(logPath);
        QMessageBox::warning(
            parent, QObject::tr("更新"),
            QObject::tr("上次更新没有完成，当前仍是 %1 版本。\n\n"
                        "原因：\n%2\n\n"
                        "最常见的原因是内核 rekobox_core.exe 仍在后台运行，占着文件与端口。\n"
                        "关掉后台残留的 rekobox_core 进程后，再点一次「检查更新」即可重试。")
                .arg(QString(NKR_VERSION), content));
    }

    void CheckForUpdates(QWidget *parent, bool manual) {
        // 启动检查与手动点击可能撞在一起，避免并发拉取
        static bool checking = false;
        if (checking) {
            if (manual) {
                QMessageBox::information(parent, QObject::tr("检查更新"),
                                         QObject::tr("正在检查更新，请稍候。"));
            }
            return;
        }
        checking = true;
        auto updater = new Updater(parent, manual);
        QObject::connect(updater, &QObject::destroyed, [] { checking = false; });
        updater->start();
    }

} // namespace NekoGui_update
