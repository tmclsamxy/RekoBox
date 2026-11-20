#pragma once
#include <QHostAddress>
#include <QObject>
#include <QString>
//
namespace Qv2ray::components::proxy {
    void ClearSystemProxy();
    void SetSystemProxy(int http_port, int socks_port);
    // macOS：启动时还原上次被强杀残留的系统代理（其他平台空实现）
    void RecoverSystemProxyIfNeeded();
} // namespace Qv2ray::components::proxy

using namespace Qv2ray::components;
using namespace Qv2ray::components::proxy;
