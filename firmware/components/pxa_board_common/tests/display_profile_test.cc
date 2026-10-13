#undef NDEBUG
#include <cassert>
#include <pxa_board_common/display_completion.h>
#include <pxa_board_common/display_profile.h>

int main() {
    using namespace pxa_board_common;
    constexpr uint64_t wrap = UINT64_C(1) << 32;
    static_assert(DisplayCompletionTimestamp(wrap + 12, UINT32_MAX - 8) == wrap - 9);
    static_assert(DisplayCompletionTimestamp(wrap * 3 + 50000, 10000) == wrap * 3 + 10000);
    static_assert(DisplayCompletionTimestamp(100, 100) == 100);

    pxsys_display_profile_t output{};
    ApplyDisplayProfile({.width = 800, .height = 480}, &output);
    assert(output.density_dpi == 160 && output.width == 800 && output.height == 480);
    assert(pxsys_display_profile_validate(&output) == PXSYS_STATUS_OK);

    const DisplayProfile watcher{
        .width = 412, .height = 412, .density_dpi = 284,
        .shape = PXSYS_DISPLAY_SHAPE_CIRCLE,
        .corner_radii = {206, 206, 206, 206},
        .safe_insets = {60, 60, 60, 60}};
    ApplyDisplayProfile(watcher, &output);
    assert(output.density_dpi == 284 && output.shape == PXSYS_DISPLAY_SHAPE_CIRCLE);
    assert(output.safe_insets.left == 60 && output.corner_radii.top_left == 206);
    assert(pxsys_display_profile_validate(&output) == PXSYS_STATUS_OK);
    assert(pxsys_display_contains_point(&output, 206, 0));
    assert(!pxsys_display_contains_point(&output, 0, 0));
    ApplyDisplayProfile(watcher, nullptr);
}
