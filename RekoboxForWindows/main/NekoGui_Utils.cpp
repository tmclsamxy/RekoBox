#include "NekoGui_Utils.hpp"

#include "3rdparty/base64.h"
#include "3rdparty/QThreadCreateThread.hpp"

#include <random>

#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QUrlQuery>
#include <QTcpServer>
#include <QTimer>
#include <QMessageBox>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QDateTime>
#include <QLocale>

#ifdef Q_OS_WIN
#include "sys/windows/guihelper.h"
#endif
#ifdef Q_OS_MACOS
#include "sys/macos/Gui.hpp"
#endif

QStringList SplitLines(const QString &_string) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return _string.split(QRegularExpression("[\r\n]"), Qt::SplitBehaviorFlags::SkipEmptyParts);
#else
    return _string.split(QRegularExpression("[\r\n]"), QString::SkipEmptyParts);
#endif
}

QStringList SplitLinesSkipSharp(const QString &_string, int maxLine) {
    auto lines = SplitLines(_string);
    QStringList newLines;
    int i = 0;
    for (const auto &line: lines) {
        if (line.trimmed().startsWith("#")) continue;
        newLines << line;
        if (maxLine > 0 && ++i >= maxLine) break;
    }
    return newLines;
}

QByteArray DecodeB64IfValid(const QString &input, QByteArray::Base64Options options) {
    Qt515Base64::Base64Options newOptions = Qt515Base64::Base64Option::AbortOnBase64DecodingErrors;
    if (options.testFlag(QByteArray::Base64UrlEncoding)) newOptions |= Qt515Base64::Base64Option::Base64UrlEncoding;
    if (options.testFlag(QByteArray::OmitTrailingEquals)) newOptions |= Qt515Base64::Base64Option::OmitTrailingEquals;
    auto result = Qt515Base64::QByteArray_fromBase64Encoding(input.toUtf8(), newOptions);
    if (result) {
        return result.decoded;
    }
    return {};
}

QString QStringList2Command(const QStringList &list) {
    QStringList new_list;
    for (auto str: list) {
        auto q = "\"" + str.replace("\"", "\\\"") + "\"";
        new_list << q;
    }
    return new_list.join(" ");
}

QString GetQueryValue(const QUrlQuery &q, const QString &key, const QString &def) {
    auto a = q.queryItemValue(key);
    if (a.isEmpty()) {
        return def;
    }
    return a;
}

QString GetRandomString(int randomStringLength) {
    std::random_device rd;
    std::mt19937 mt(rd());

    const QString possibleCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

    std::uniform_int_distribution<int> dist(0, possibleCharacters.length() - 1);

    QString randomString;
    for (int i = 0; i < randomStringLength; ++i) {
        QChar nextChar = possibleCharacters.at(dist(mt));
        randomString.append(nextChar);
    }
    return randomString;
}

quint64 GetRandomUint64() {
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<quint64> dist;
    return dist(mt);
}

// QString >> QJson
QJsonObject QString2QJsonObject(const QString &jsonString) {
    QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonString.toUtf8());
    QJsonObject jsonObject = jsonDocument.object();
    return jsonObject;
}

// QJson >> QString
QString QJsonObject2QString(const QJsonObject &jsonObject, bool compact) {
    return QJsonDocument(jsonObject).toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented);
}

template<typename T>
QJsonArray QList2QJsonArray(const QList<T> &list) {
    QVariantList list2;
    for (auto &item: list)
        list2.append(item);
    return QJsonArray::fromVariantList(list2);
}

template QJsonArray QList2QJsonArray<int>(const QList<int> &list);
template QJsonArray QList2QJsonArray<QString>(const QList<QString> &list);

QList<int> QJsonArray2QListInt(const QJsonArray &arr) {
    QList<int> list2;
    for (auto item: arr)
        list2.append(item.toInt());
    return list2;
}

QList<QString> QJsonArray2QListString(const QJsonArray &arr) {
    QList<QString> list2;
    for (auto item: arr)
        list2.append(item.toString());
    return list2;
}

QByteArray ReadFile(const QString &path) {
    QFile file(path);
    file.open(QFile::ReadOnly);
    return file.readAll();
}

QString ReadFileText(const QString &path) {
    QFile file(path);
    file.open(QFile::ReadOnly | QFile::Text);
    QTextStream stream(&file);
    return stream.readAll();
}

int MkPort() {
    QTcpServer s;
    s.listen();
    auto port = s.serverPort();
    s.close();
    return port;
}

QString ReadableSize(const qint64 &size) {
    double sizeAsDouble = size;
    static QStringList measures;
    if (measures.isEmpty())
        measures << "B"
                 << "KiB"
                 << "MiB"
                 << "GiB"
                 << "TiB"
                 << "PiB"
                 << "EiB"
                 << "ZiB"
                 << "YiB";
    QStringListIterator it(measures);
    QString measure(it.next());
    while (sizeAsDouble >= 1024.0 && it.hasNext()) {
        measure = it.next();
        sizeAsDouble /= 1024.0;
    }
    return QString::fromLatin1("%1 %2").arg(sizeAsDouble, 0, 'f', 2).arg(measure);
}

bool IsIpAddress(const QString &str) {
    auto address = QHostAddress(str);
    if (address.protocol() == QAbstractSocket::IPv4Protocol || address.protocol() == QAbstractSocket::IPv6Protocol)
        return true;
    return false;
}

bool IsIpAddressV4(const QString &str) {
    auto address = QHostAddress(str);
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return true;
    return false;
}

bool IsIpAddressV6(const QString &str) {
    auto address = QHostAddress(str);
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return true;
    return false;
}

QString DisplayTime(long long time, int formatType) {
    QDateTime t;
    t.setMSecsSinceEpoch(time * 1000);
    return QLocale().toString(t, QLocale::FormatType(formatType));
}

QWidget *GetMessageBoxParent() {
    auto activeWindow = QApplication::activeWindow();
    if (activeWindow == nullptr && mainwindow != nullptr) {
        if (mainwindow->isVisible()) return mainwindow;
        return nullptr;
    }
    return activeWindow;
}

int MessageBoxWarning(const QString &title, const QString &text) {
    return QMessageBox::warning(GetMessageBoxParent(), title, text);
}

int MessageBoxInfo(const QString &title, const QString &text) {
    return QMessageBox::information(GetMessageBoxParent(), title, text);
}

void ActivateWindow(QWidget *w) {
    if (w == nullptr) return;
    w->setWindowState(w->windowState() & ~Qt::WindowMinimized);
    w->setVisible(true);
#ifdef Q_OS_WIN
    Windows_QWidget_SetForegroundWindow(w);
#endif
#ifdef Q_OS_MACOS
    // mac 上 raise() + activateWindow() 在 App 不是前台进程时通常只闪一下：
    // Qt 这两个调用走的是窗口层级和"请求激活"，系统会拒绝非前台 App 的抢占。
    // 必须先让 NSApp 自己抢到前台（activateIgnoringOtherApps），窗口才抬得起来。
    // 顺序不能反：先 activate 再 raise，否则 raise 发生在 App 未激活时无效。
    NekoGui_mac::ActivateAppIgnoringOtherApps();
#endif
    w->raise();
    w->activateWindow();
}

void runOnUiThread(const std::function<void()> &callback, QObject *parent) {
    // any thread
    //
    // ⚠️ 语义陷阱（P0-2 的一部分）：parent 传 QThread* 时，回调跑在**那个 QThread 管理的
    // 线程**上，不是 UI 线程。历史上 mainwindow_grpc.cpp / mainwindow.cpp 有 6 处传了
    // `DS_cores`，注释写着"main thread"但实际跑在 cores 线程，于是：
    //   - 那些回调里碰了 UI 对象（run-in-wrong-thread）
    //   - 更糟的是回调里捕获了调用方栈上的变量（`[&]` + 阻塞式信号量握手），
    //     一旦队列延迟到栈帧销毁之后执行，就是往已释放的栈上写 → 堆/栈被写坏。
    // 崩溃报告里 `QThread::exec → activateSocketNotifiers → notifyInternal2 → 读 QThreadData`
    // 的 SIGBUS、以及 `tiny_free_list_remove_ptr` 的堆损坏，都和这一类相符。
    //
    // 现在：QThread* 仍按"投递到该线程"处理（保持兼容），但会打一条 warn，
    // 逼调用方显式选择 runOnThread() 或留空走 UI 线程。
    QThread *target = nullptr;
    if (parent == nullptr) {
        if (mainwindow == nullptr || QCoreApplication::closingDown()) return;
        target = mainwindow->thread();
    } else if (auto as_thread = qobject_cast<QThread *>(parent)) {
        qWarning() << "[thread] runOnUiThread() was given a QThread*; the callback will run on"
                   << "that thread, NOT the UI thread. Use runOnThread() to be explicit.";
        target = as_thread;
    } else {
        target = parent->thread();
    }

    if (target == nullptr || !target->isRunning()) {
        // 线程没在跑就没有事件循环，投递过去永远不会执行（原来的实现会静默漏掉回调
        // 并泄漏 QTimer）。直接在调用方线程里跑，至少不会丢事件。
        callback();
        return;
    }

    auto *timer = new QTimer();
    timer->moveToThread(target);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, timer, [callback, timer]() {
        callback();
        timer->deleteLater();
    });
    QMetaObject::invokeMethod(timer, "start", Qt::QueuedConnection, Q_ARG(int, 0));
}

void runOnThread(QThread *thread, const std::function<void()> &callback) {
    if (thread == nullptr || thread == QThread::currentThread()) {
        callback();
        return;
    }
    if (!thread->isRunning()) {
        qWarning() << "[thread] runOnThread(): target thread is not running, running inline";
        callback();
        return;
    }
    auto *timer = new QTimer();
    timer->moveToThread(thread);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, timer, [callback, timer]() {
        callback();
        timer->deleteLater();
    });
    QMetaObject::invokeMethod(timer, "start", Qt::QueuedConnection, Q_ARG(int, 0));
}

void runOnNewThread(const std::function<void()> &callback) {
    createQThread(callback)->start();
}

void setTimeout(const std::function<void()> &callback, QObject *obj, int timeout) {
    auto t = new QTimer;
    QObject::connect(t, &QTimer::timeout, obj, [=] {
        callback();
        t->deleteLater();
    });
    t->setSingleShot(true);
    t->setInterval(timeout);
    t->start();
}
