// SMAppService 的桥接层。必须是 ObjC++（.mm）：
// SMAppService 是 ObjC 类，纯 C++ 编译单元里拿不到它的接口。
#include "sys/macos/AutoRunMac.hpp"

#import <ServiceManagement/ServiceManagement.h>
#import <Foundation/Foundation.h>

#include <QDebug>

namespace NekoGui_mac {

    // SMAppService 只在 macOS 13+ 存在。部署目标是 13.0，但用户可能装到更老的系统上，
    // 所以仍要运行时判断一次，别直接崩在 dlsym 之前。
    static bool sm_available() {
        if (@available(macOS 13.0, *)) return true;
        return false;
    }

    int AutoRun_Status() {
        if (!sm_available()) return AutoRun_Unsupported;

        @autoreleasepool {
            SMAppService *svc = [SMAppService mainAppService];
            if (svc == nil) return AutoRun_NotFound;

            switch ([svc status]) {
                case SMAppServiceStatusEnabled:
                    return AutoRun_Enabled;
                case SMAppServiceStatusRequiresApproval:
                    return AutoRun_RequiresApproval;
                case SMAppServiceStatusNotRegistered:
                    return AutoRun_NotRegistered;
                case SMAppServiceStatusNotFound:
                default:
                    // SMAppServiceStatusNotFound：bundle 没在 /Applications 下（比如直接跑
                    // build/ 里的 .app），系统拒绝登记。这不是错误，是 mac 的既定规则。
                    return AutoRun_NotFound;
            }
        }
    }

    bool AutoRun_IsEnabled() {
        const auto st = AutoRun_Status();
        // RequiresApproval 也表示用户已注册过（只是还没在系统设置里批准），
        // 勾选框应该保持勾上，再由调用方提示"去系统设置打开"。
        return st == AutoRun_Enabled || st == AutoRun_RequiresApproval;
    }

    bool AutoRun_SetEnabled(bool enable) {
        if (!sm_available()) {
            qDebug() << "[mac] SMAppService unavailable (requires macOS 13+)";
            return false;
        }

        @autoreleasepool {
            SMAppService *svc = [SMAppService mainAppService];
            if (svc == nil) {
                qDebug() << "[mac] mainAppService is nil";
                return false;
            }

            NSError *error = nil;
            const BOOL ok = enable ? [svc registerAndReturnError:&error]
                                   : [svc unregisterAndReturnError:&error];
            if (!ok) {
                qDebug() << "[mac] SMAppService" << (enable ? "register" : "unregister")
                         << "failed:" << QString::fromNSString(error.localizedDescription);
                return false;
            }

            // 登记成功不代表生效：可能是 RequiresApproval（用户需在系统设置里批准），
            // 也可能是 NotFound（bundle 不在 /Applications）。这里把结果打出来，
            // 出问题时不必再靠猜。
            qDebug() << "[mac] SMAppService" << (enable ? "registered" : "unregistered")
                     << "status ->" << AutoRun_Status();
            return true;
        }
    }

} // namespace NekoGui_mac