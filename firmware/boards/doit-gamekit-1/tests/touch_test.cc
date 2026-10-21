#include "doit_gamekit_1_touch_report.h"
#include <cassert>

using namespace doit_gamekit_1;

static void Point(uint8_t* bytes, unsigned index, unsigned id, int x, int y, unsigned event = 0) {
    auto* record = bytes + 1 + index * 6;
    record[0] = uint8_t((event << 6) | (x >> 8));
    record[1] = uint8_t(x);
    record[2] = uint8_t((id << 4) | (y >> 8));
    record[3] = uint8_t(y);
}

int main() {
    uint8_t bytes[13]{};
    TouchReport report;
    assert(!DecodeTouchReport(nullptr, sizeof(bytes), LandscapeRotation::Deg270, report));
    assert(!DecodeTouchReport(bytes, 12, LandscapeRotation::Deg270, report));
    bytes[0] = 3;
    assert(!DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    bytes[0] = 1;
    // Reference touch native dimensions stay 240x320 while the LCD RAM is
    // addressed in 320x240 landscape. Check every sensor pixel and all edges.
    for (auto rotation : {LandscapeRotation::Deg90, LandscapeRotation::Deg270})
        for (int y = 0; y < kTouchNativeHeight; ++y)
            for (int x = 0; x < kTouchNativeWidth; ++x) {
                Point(bytes, 0, 7, x, y, 2);
                assert(DecodeTouchReport(bytes, sizeof(bytes), rotation, report));
                assert(report.count == 1 && report.points[0].id == 7);
                const auto& point = report.points[0];
                assert(point.x < kWidth && point.y < kHeight);
                const int index = rotation == LandscapeRotation::Deg270
                    ? point.x * kHeight + kHeight - 1 - point.y
                    : (kWidth - 1 - point.x) * kHeight + point.y;
                assert(index == y * kTouchNativeWidth + x);
            }
    Point(bytes, 0, 1, 240, 0);
    assert(!DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    Point(bytes, 0, 1, 0, 320);
    assert(!DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    Point(bytes, 0, 1, 0, 0, 3);
    assert(!DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    Point(bytes, 0, 1, 0, 0, 1);
    assert(DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    assert(report.count == 0); // UP is never delivered as a new press at (0,0)
    bytes[0] = 2;
    Point(bytes, 0, 4, 10, 20);
    Point(bytes, 1, 4, 30, 40);
    assert(!DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    Point(bytes, 1, 9, 30, 40);
    assert(DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    assert(report.count == 2 && report.points[0].x == 20 && report.points[0].y == 229);
    TouchSlots slots;
    auto points = slots.Update(report);
    assert(points[0].id == 4 && points[1].id == 9);
    std::swap(report.points[0], report.points[1]);
    points = slots.Update(report);
    assert(points[0].id == 4 && points[1].id == 9); // wire order does not swap fingers
    report.count = 1; // ID 9 remains; ID 4 lifted
    points = slots.Update(report);
    assert(!points[0].pressed && points[1].pressed && points[1].id == 9);
    report.points[1] = TouchPoint{.id = 12, .x = 100, .y = 100, .pressed = true};
    report.count = 2;
    points = slots.Update(report);
    assert(points[0].id == 12 && points[1].id == 9);
    points = slots.Update({});
    assert(!points[0].pressed && !points[1].pressed);
    bytes[0] = 0;
    assert(DecodeTouchReport(bytes, sizeof(bytes), LandscapeRotation::Deg270, report));
    assert(report.count == 0); // stale coordinates after release are ignored
}
