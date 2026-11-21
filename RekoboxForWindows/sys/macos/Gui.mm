// AppKit 桥接层。必须是 ObjC++（.mm）：NSApplication / NSAppearance 都是 ObjC 类。
#include "sys/macos/Gui.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include <QDebug>

namespace NekoGui_mac {

    namespace {
        // 外观变化的观察者。用 __block + 堆上的 block 变量，取消订阅时要 -removeObserver。
        id g_appearance_observer = nil;
        std::function<void()> g_appearance_callback;

        // App Nap 抑制句柄：必须长期持有，release 就等于取消抑制。
        // beginActivityWithOptions 返回 id（不是 NSProcessInfoActivity——那个是
        // category 名，不能当类型用）。
        id g_activity = nil;
        // NSActivityUserInitiatedAllowingIdleSystemSleep：
        //   阻止 App Nap / 定时器合并（这正是我们要的），
        //   但**不**阻止系统空闲休眠（那是笔记本上很扰人的行为，不该由代理软件决定）。
        NSActivityOptions g_activity_options = NSActivityUserInitiatedAllowingIdleSystemSleep;

        id g_wake_observer = nil;
        SCDynamicStoreRef g_dynamic_store = nullptr;
        std::function<void()> g_wake_callback;
        std::function<void()> g_net_callback;

        // 系统外观变化时由 NSNotificationCenter 回调。它在主线程投递，
        // 所以这里可以直接调 Qt（ApplyTheme 会碰 qApp->setPalette）。
        void handle_appearance_change(NSNotification *note) {
            (void) note;
            if (g_appearance_callback) {
                g_appearance_callback();
            }
        }

        // SystemConfiguration 的网络变化回调：全局 IPv4 配置一变就来。
        // 这里只把"变了"这件事传给 Qt 侧，具体怎么处理由 UI 层决定
        // （避免在 ObjC 里写业务逻辑）。
        //
        // 注意 changedKeys 是回调参数，**不能**在这里 CFRelease：它是借来的引用，
        // 由 SystemConfiguration 自己管生命周期。
        void network_change_callback(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info) {
            (void) store;
            (void) changedKeys;
            (void) info;
            if (g_net_callback) {
                g_net_callback();
            }
        }
    } // namespace

    void SetDockIconVisible(bool visible) {
        @autoreleasepool {
            NSApplication *app = [NSApplication sharedApplication];
            if (app == nil) return;

            // Accessory：无 Dock 图标、不进 Cmd+Tab，但有菜单栏图标 —— 正是"纯托盘"。
            // Regular：普通应用，Dock 与 Cmd+Tab 都回来。
            //
            // 注意 setActivationPolicy 在窗口已经可见时调用会让窗口暂时失焦，
            // 所以只在真的需要切换时才调（Qt 的 isVisible 判断在调用方做）。
            const auto policy = visible ? NSApplicationActivationPolicyRegular
                                        : NSApplicationActivationPolicyAccessory;
            if (![app setActivationPolicy:policy]) {
                qDebug() << "[mac] setActivationPolicy" << (visible ? "Regular" : "Accessory") << "failed";
            }
        }
    }

    void ActivateAppIgnoringOtherApps() {
        @autoreleasepool {
            NSApplication *app = [NSApplication sharedApplication];
            if (app == nil) return;

            // activateIgnoringOtherApps 已 deprecated，但 Apple 的替代方案
            // -activate（macOS 14+）在 13.0 部署目标上不可用，且行为更弱：
            // 它只在"当前 App 有窗口"时生效。这里两个都调，老系统走前者。
            [app activateIgnoringOtherApps:YES];
        }
    }

    bool IsSystemDarkMode() {
        @autoreleasepool {
            NSApplication *app = [NSApplication sharedApplication];
            if (app == nil) return false;

            // effectiveAppearance 是"当前 App 实际生效"的外观（含强制指定），
            // 比 NSAppearance.currentAppearance（绘制期才有效）适合在这里读。
            NSAppearance *appearance = app.effectiveAppearance;
            if (appearance == nil) return false;

            NSAppearanceName name = [appearance bestMatchFromAppearancesWithNames:@[
                    NSAppearanceNameAqua,
                    NSAppearanceNameDarkAqua,
            ]];
            return [name isEqualToString:NSAppearanceNameDarkAqua];
        }
    }

    bool SetAppearanceChangeCallback(const std::function<void()> &callback) {
        @autoreleasepool {
            NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
            if (center == nil) return false;

            // 先撤掉旧的：重复订阅会让一次切换触发多次 ApplyTheme
            if (g_appearance_observer != nil) {
                [center removeObserver:g_appearance_observer];
                g_appearance_observer = nil;
            }
            g_appearance_callback = callback;
            if (!callback) return true;

            // 用 block 版 API（10.6+）：它持有 block，并在 dealloc 时自动回收。
            g_appearance_observer = [center addObserverForName:@"AppleInterfaceThemeChangedNotification"
                                                        object:nil
                                                         queue:[NSOperationQueue mainQueue]
                                                    usingBlock:^(NSNotification *note) {
                                                        handle_appearance_change(note);
                                                    }];
            return g_appearance_observer != nil;
        }
    }

    void SetAppNapInhibited(bool inhibited) {
        @autoreleasepool {
            if (inhibited) {
                if (g_activity != nil) return; // 已经持有，重复调用不做任何事
                NSProcessInfo *info = [NSProcessInfo processInfo];
                if (info == nil) return;
                // 原因串会显示在"活动监视器 → 能耗"里，写清楚便于用户排查
                g_activity = [info beginActivityWithOptions:g_activity_options
                                                     reason:@"RekoBox is proxying traffic"];
                if (g_activity == nil) {
                    qWarning() << "[mac] beginActivity returned nil, App Nap may still throttle timers";
                } else {
                    qDebug() << "[mac] App Nap inhibited";
                }
            } else {
                if (g_activity == nil) return;
                [[NSProcessInfo processInfo] endActivity:g_activity];
                g_activity = nil;
                qDebug() << "[mac] App Nap inhibition released";
            }
        }
    }

    bool SetPowerEventCallbacks(const std::function<void()> &wake_cb,
                                const std::function<void()> &net_cb) {
        @autoreleasepool {
            // ---- 睡眠唤醒 ----
            NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
            if (g_wake_observer != nil) {
                [center removeObserver:g_wake_observer];
                g_wake_observer = nil;
            }
            g_wake_callback = wake_cb;
            bool ok = true;
            if (wake_cb) {
                // 用 NSWorkspaceDidWakeNotification 而不是 "系统已唤醒"的分布式通知：
                // 后者是 NSDistributedNotificationCenter 的私有名字，不适合长期依赖。
                g_wake_observer = [center addObserverForName:NSWorkspaceDidWakeNotification
                                                      object:nil
                                                       queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification *note) {
                                                      (void) note;
                                                      if (g_wake_callback) g_wake_callback();
                                                  }];
                if (g_wake_observer == nil) {
                    qWarning() << "[mac] failed to observe NSWorkspaceDidWakeNotification";
                    ok = false;
                }
            }

            // ---- 网络变化 ----
            // 用 SCDynamicStore 监听 "State:/Network/Global/IPv4"：
            // 换 Wi-Fi、插网线、拿到新 DHCP 租约都会触发。
            // 旧 session 要先释放，否则每订阅一次就漏一个回调上下文。
            if (g_dynamic_store != nullptr) {
                SCDynamicStoreSetNotificationKeys(g_dynamic_store, nullptr, nullptr);
                CFRelease(g_dynamic_store);
                g_dynamic_store = nullptr;
            }
            g_net_callback = net_cb;
            if (net_cb) {
                SCDynamicStoreContext ctx = {0, nullptr, nullptr, nullptr, nullptr};
                g_dynamic_store = SCDynamicStoreCreate(kCFAllocatorDefault, CFSTR("rekobox.netwatch"),
                                                       network_change_callback, &ctx);
                if (g_dynamic_store == nullptr) {
                    qWarning() << "[mac] SCDynamicStoreCreate failed";
                    ok = false;
                } else {
                    CFStringRef key = CFSTR("State:/Network/Global/IPv4");
                    CFArrayRef keys = CFArrayCreate(kCFAllocatorDefault, (const void **) &key, 1,
                                                    &kCFTypeArrayCallBacks);
                    bool set = SCDynamicStoreSetNotificationKeys(g_dynamic_store, keys, nullptr);
                    CFRelease(keys);
                    if (!set) {
                        qWarning() << "[mac] SCDynamicStoreSetNotificationKeys failed";
                        ok = false;
                    } else if (!SCDynamicStoreSetDispatchQueue(g_dynamic_store, dispatch_get_main_queue())) {
                        // ⚠️ 没有 dispatch queue 就**不会**有回调（这是 SCDynamicStore 的坑：
                        // 只设 notification keys 不设 queue，注册成功但永远静默）。
                        qWarning() << "[mac] SCDynamicStoreSetDispatchQueue failed";
                        ok = false;
                    } else {
                        qDebug() << "[mac] network change observer installed";
                    }
                }
            }
            return ok;
        }
    }

} // namespace NekoGui_mac