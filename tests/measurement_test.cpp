#include "Measurement.h"
#include "MapSession.h"
#include "OverlayVisibility.h"
#include <cstdlib>
#include <iostream>
#include <limits>

void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int main() {
    using namespace mortar;
    Measurement m;
    check(!m.click({1, 2}), "uncalibrated measurement rejected");
    check(!m.calibrate(0), "zero reference rejected");
    check(!m.calibrate(std::numeric_limits<double>::infinity()), "nonfinite reference rejected");
    check(m.calibrate(500), "multi-grid reference");
    check(m.click({-1000, 20}), "negative monitor coordinate");
    check(!m.click({-995, 20}), "short calibration rejected without losing first point");
    check(m.click({-750, 20}), "250 pixels = 500 meters");
    check(m.click({-900, 100}) && m.click({-750, 300}), "two measurement points");
    check(std::abs(*m.distance() - 500.0) < 1e-9, "3-4-5 triangle scaled to meters");
    check(m.click({10, 10}) && !m.distance(), "next pair clears old result");
    m.clearPoints();
    check(m.metersPerPixel && !m.first && !m.distance(), "pan clears points but retains scale");
    m.click({1, 1});
    m.invalidateScale();
    check(!m.metersPerPixel && !m.first && !m.distance(), "zoom invalidates all view-dependent data");
    m.calibrate(100); m.click({0, 0}); m.clearPoints();
    check(m.phase == Phase::Idle && !m.click({100, 0}), "pan cancels incomplete calibration");
    m.calibrate(100); m.click({0, 0}); m.click({100, 0});
    m.click({50, 50}); m.click({50, 50});
    check(m.distance() && *m.distance() == 0, "coincident points have zero distance");
    MapSession s;
    s.newMatch(L"Erangel.1920x1080.96"); s.setActive(true);
    s.measurement.calibrate(100); s.click({0, 0}); s.click({80, 0});
    s.click({0, 0}); s.click({240, 320});
    check(s.lastDistance && *s.lastDistance == 500, "completed distance saved independently");
    s.setActive(false);
    check(!s.measurement.first && *s.lastDistance == 500, "closing map drops screen points but retains result");
    check(!s.click({10, 20}), "closed map ignores points");
    check(!s.wheel(120) && s.zoom == 0, "closed map ignores wheel");
    s.setActive(true);
    check(s.measurement.metersPerPixel == 1.25, "reopening restores scale without calibration");
    check(s.wheel(120) && s.zoom == 1 && !s.measurement.metersPerPixel, "uncalibrated next zoom cannot measure");
    check(*s.lastDistance == 500 && !s.click({0, 0}), "unknown scale retains history but rejects measurement");
    s.measurement.calibrate(100); s.click({0, 0}); s.click({160, 0});
    check(s.savedScale() == 0.625, "second zoom calibrated independently");
    s.wheel(-120);
    check(s.measurement.metersPerPixel == 1.25, "previous zoom calibration reused");
    s.wheel(120); check(s.measurement.metersPerPixel == 0.625, "next zoom calibration reused");
    s.wheel(1200); check(s.zoom == 4, "multi-notch event clamps maximum");
    for (int i = 0; i < 10; ++i) s.wheel(120);
    s.wheel(-120); check(s.zoom == 3, "no overscroll debt at maximum");
    s.wheel(-1200); check(s.zoom == 0, "clamps minimum");
    s.wheel(-60); s.wheel(60); check(s.zoom == 0, "partial wheel events accumulate at limit");
    s.wheel(60); check(s.zoom == 1, "high-resolution wheel makes one complete notch");
    s.resetZoom(); check(s.zoom == 0 && s.savedScale() == 1.25, "manual reset keeps stored calibrations");
    s.measurement.calibrate(100); s.click({0, 0}); s.wheel(120);
    check(!s.measurement.first && s.measurement.metersPerPixel == 0.625, "zoom cancels unfinished calibration");
    s.newMatch(L"Miramar.1920x1080.96");
    check(!s.active && s.zoom == 0 && !s.lastDistance && !s.savedScale(), "new map resets match and has isolated scales");
    s.newMatch(L"Erangel.1920x1080.96");
    check(s.savedScale() == 1.25, "next match reuses original map scale");
    s.useProfile(L"Erangel.2560x1440.144");
    check(!s.savedScale(), "resolution and DPI profiles are isolated");
    s.useProfile(L"Erangel.1920x1080.96"); s.setActive(true);
    s.click({0, 0}); s.click({0, 0}); s.clearResult();
    check(!s.lastDistance && s.savedScale() == 1.25, "explicit clear retains calibration");
    OverlayVisibility visibility;
    check(!visibility.visible(false, 0), "startup overlay hidden until map opens");
    visibility.mapStateChanged(false, true, 100);
    check(visibility.visible(true, 100000), "open map remains visible without timeout");
    visibility.mapStateChanged(true, false, 1000);
    check(visibility.visible(false, 3999), "closed map visible before three seconds");
    check(!visibility.visible(false, 4000), "closed map hidden at three seconds");
    visibility.mapStateChanged(false, false, 2000);
    check(!visibility.visible(false, 4000), "repeated close does not restart countdown");
    visibility.mapStateChanged(false, true, 2500);
    check(visibility.visible(true, 5000), "reopening cancels old countdown");
    visibility.mapStateChanged(true, false, 6000);
    check(visibility.visible(false, 8999) && !visibility.visible(false, 9000), "each close starts a new countdown");
    visibility.reset();
    check(!visibility.visible(false, 6100), "new match clears old countdown");
    std::cout << "All measurement and visibility checks passed.\n";
}
