#pragma once

#include <QString>
#include <QList>
#include <QMutex>

#include "TrafficData.hpp"

namespace NekoGui_traffic {
    class TrafficLooper {
    public:
        bool loop_enabled = false;
        bool looping = false;
        QMutex loop_mutex;

        QList<std::shared_ptr<TrafficData>> items;
        TrafficData *proxy = nullptr;

        void UpdateAll();

        // 停止代理时把实时速率清零，保留累计流量
        void ResetRates();

        void Loop();

    private:
        TrafficData *bypass = new TrafficData("bypass");

        [[nodiscard]] static TrafficData *update_stats(TrafficData *item);

        [[nodiscard]] static QJsonArray get_connection_list();
    };

    extern TrafficLooper *trafficLooper;
} // namespace NekoGui_traffic
