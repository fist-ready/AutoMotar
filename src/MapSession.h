#pragma once
#include "Measurement.h"
#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace mortar {
using Scales = std::array<std::optional<double>, 5>;
class MapSession {
public:
    Measurement measurement;
    bool active{};
    int zoom{};
    std::optional<double> lastDistance;
    std::map<std::wstring, Scales> profiles;
    std::wstring profile;

    void useProfile(const std::wstring& key) { profile = key; restoreScale(); }
    void newMatch(const std::wstring& key) {
        zoom = 0; wheelRemainder = 0; active = false; lastDistance.reset(); useProfile(key);
    }
    std::optional<double> savedScale() const {
        const auto found = profiles.find(profile);
        return found == profiles.end() ? std::nullopt : found->second[zoom];
    }
    void restoreScale() {
        measurement.metersPerPixel = savedScale(); measurement.clearPoints();
    }
    void setActive(bool value) {
        active = value; wheelRemainder = 0; restoreScale();
    }
    void resetZoom() { zoom = 0; wheelRemainder = 0; restoreScale(); }
    bool wheel(int delta) {
        if (!active) return false;
        // Accumulate high-resolution wheel events; discard surplus at either limit.
        const int total = wheelRemainder + delta;
        const int steps = total / 120;
        wheelRemainder = total % 120;
        const int next = std::clamp(zoom + steps, 0, 4);
        if ((next == 0 && wheelRemainder < 0) || (next == 4 && wheelRemainder > 0)) wheelRemainder = 0;
        if (next == zoom) return false;
        zoom = next; restoreScale(); return true;
    }
    bool click(Point point) {
        if (!active) return false;
        const auto before = measurement.phase;
        if (!measurement.click(point)) return false;
        if (before == Phase::CalibrateSecond && measurement.metersPerPixel && !profile.empty())
            profiles[profile][zoom] = measurement.metersPerPixel;
        if (const auto distance = measurement.distance()) lastDistance = distance;
        return true;
    }
    void clearResult() { lastDistance.reset(); restoreScale(); }
private:
    int wheelRemainder{};
};
}
