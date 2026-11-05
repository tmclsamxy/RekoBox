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
                QNetworkRequest request{QUrl(kApiReleases)};
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
                            QMessageBox::warning(parent, QObject::tr("Check update"),
                                                 QObject::tr("Failed to reach GitHub: %1").arg(err));
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
                    fail(QObject::tr("Unexpected response from GitHub."));
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
                        QMessageBox::information(parent, QObject::tr("Check update"),
                                                 QObject::tr("No update available."));
                    }
                    deleteLater();
                    return;
                }

                if (compareVersion(best.version, QString(NKR_VERSION)) <= 0) {
                    if (manual) {
                        QMessageBox::information(parent, QObject::tr("Check update"),
                                                 QObject::tr("You are already on the latest version (%1).")
                                                     .arg(QString(NKR_VERSION)));
                    }
                    deleteLater();
                    return;
                }

                update = best;
                prompt();
            }

            void prompt() {
                QString text = QObject::tr("Current version: %1\nNew version: %2 (%3)\n\nDownload and install now?")
                                   .arg(QString(NKR_VERSION), update.version, readableSize(update.assetSize));
                if (!update.notes.isEmpty()) {
                    auto notes = update.notes;
                    if (notes.size() > 1200) notes = notes.left(1200) + "…";
                    text += "\n\n" + notes;
                }

                QMessageBox box(parent);
                box.setWindowTitle(QObject::tr("Update available"));
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

                progress = new QProgressDialog(QObject::tr("Downloading update…"), QString(), 0, 100, parent);
                progress->setWindowTitle(QObject::tr("Update"));
                progress->setWindowModality(Qt::WindowModal);
                progress->setCancelButton(nullptr);
                progress->setMinimumDuration(0);
                progress->setAutoClose(false);
                progress->setValue(0);
                progress->show();

                QNetworkRequest request{QUrl(update.assetUrl)};
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QString("RekoBox/%1").arg(QString(NKR_VERSION)));
                reply = nam.get(request);
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
                    reply->deleteLater();
                    reply = nullptr;
                    if (progress != nullptr) {
                        progress->close();
                        progress->deleteLater();
                        progress = nullptr;
                    }
                    if (error != QNetworkReply::NoError) {
                        QMessageBox::warning(parent, QObject::tr("Update"),
                                             QObject::tr("Download failed: %1").arg(errorString));
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
                    QMessageBox::warning(parent, QObject::tr("Update"),
                                         QObject::tr("Failed to extract the update package."));
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
                    QMessageBox::warning(parent, QObject::tr("Update"),
                                         QObject::tr("The update package does not contain rekobox.exe."));
                    deleteLater();
                    return;
                }

                const auto dstDir = QApplication::applicationDirPath();
                if (!QFileInfo(dstDir).isWritable()) {
                    QMessageBox::warning(
                        parent, QObject::tr("Update"),
                        QObject::tr("The install directory is not writable:\n%1\n\n"
                                    "Move RekoBox to a writable folder, or run as administrator and retry.")
                            .arg(dstDir));
                    deleteLater();
                    return;
                }

                const auto scriptPath = QDir(tempDir).filePath("apply-update.ps1");
                QFile script(scriptPath);
                if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QMessageBox::warning(parent, QObject::tr("Update"),
                                         QObject::tr("Cannot write the update script."));
                    deleteLater();
                    return;
                }
                // UTF-8 BOM：PowerShell 5.1 靠它识别编码，否则含中文路径会乱码
                script.write("\xEF\xBB\xBF");
                script.write(R"PS1(param([string]$Src, [string]$Dst, [string]$Exe)
$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'rekobox-update.log'
try {
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Process -Name 'rekobox' -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)) {
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Milliseconds 800
    Copy-Item -Path (Join-Path $Src '*') -Destination $Dst -Recurse -Force
    "updated at $(Get-Date -Format o)" | Out-File -FilePath $log -Encoding utf8
} catch {
    "failed: $_" | Out-File -FilePath $log -Encoding utf8
} finally {
    Start-Sleep -Milliseconds 300
    Start-Process -FilePath $Exe
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
                     "-Exe", QDir::toNativeSeparators(exePath)});
                if (!ok) {
                    QMessageBox::warning(parent, QObject::tr("Update"),
                                         QObject::tr("Failed to start the updater."));
                    deleteLater();
                    return;
                }

                QMessageBox::information(parent, QObject::tr("Update"),
                                         QObject::tr("RekoBox will close and restart to finish the update."));
                qApp->quit();
            }

            void fail(const QString &message) {
                if (manual) {
                    QMessageBox::warning(parent, QObject::tr("Check update"), message);
                }
                deleteLater();
            }
        };

    } // namespace

    void CheckForUpdates(QWidget *parent, bool manual) {
        // 启动检查与手动点击可能撞在一起，避免并发拉取
        static bool checking = false;
        if (checking) {
            if (manual) {
                QMessageBox::information(parent, QObject::tr("Check update"),
                                         QObject::tr("Already checking, please wait."));
            }
            return;
        }
        checking = true;
        auto updater = new Updater(parent, manual);
        QObject::connect(updater, &QObject::destroyed, [] { checking = false; });
        updater->start();
    }

} // namespace NekoGui_update
