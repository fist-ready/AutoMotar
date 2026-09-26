#pragma once
#include <cmath>
#include <optional>

namespace mortar {
struct Point { double x{}, y{}; };
inline double pixels(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
enum class Phase { Idle, CalibrateFirst, CalibrateSecond, MeasureFirst, MeasureSecond, Complete };

// All points belong to one unchanged screen view. No game-specific ballistics.
class Measurement {
public:
    Phase phase{Phase::Idle};
    std::optional<double> metersPerPixel;
    std::optional<Point> first, second;
    double referenceMeters{100.0};

    void clearPoints() {
        first.reset(); second.reset();
        phase = metersPerPixel ? Phase::MeasureFirst : Phase::Idle;
    }
    void invalidateScale() { metersPerPixel.reset(); clearPoints(); }
    bool calibrate(double meters) {
        invalidateScale();
        if (!std::isfinite(meters) || meters <= 0 || meters > 10000) return false;
        referenceMeters = meters;
        phase = Phase::CalibrateFirst;
        return true;
    }
    bool click(Point p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
        switch (phase) {
        case Phase::CalibrateFirst:
            first = p; phase = Phase::CalibrateSecond; return true;
        case Phase::CalibrateSecond: {
            const double length = pixels(*first, p);
            if (length < 20) return false;
            metersPerPixel = referenceMeters / length;
            clearPoints(); return true;
        }
        case Phase::Complete: clearPoints(); [[fallthrough]];
        case Phase::MeasureFirst:
            if (!metersPerPixel) return false;
            first = p; phase = Phase::MeasureSecond; return true;
        case Phase::MeasureSecond:
            second = p; phase = Phase::Complete; return true;
        default: return false;
        }
    }
    std::optional<double> distance() const {
        if (phase != Phase::Complete || !metersPerPixel || !first || !second) return {};
        return pixels(*first, *second) * *metersPerPixel;
    }
};
}
