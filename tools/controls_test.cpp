// Host test for src/hardware/ActionCatalog.hpp: the Lens (Pinefeat) actions and analog targets (the rest of the
// vocabulary is checked where it is used). Build: g++ -std=c++20 -I src tools/controls_test.cpp

#include "hardware/ActionCatalog.hpp"

#include <cstdio>

namespace act = apertar::hardware::actions;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    for (const char *id : {"lens_focus_up", "lens_focus_down", "lens_iris_up", "lens_iris_down", "lens_calibrate"}) {
        const act::Spec *spec = act::findSpec(id);
        CHECK(spec != nullptr);
        if (!spec) continue;
        CHECK(spec->group == "Lens (Pinefeat)" && spec->arg == act::Arg::None);
        CHECK(act::canonicalAction(id) == id);
        CHECK(act::classify(id).known && act::classify(id).id == id);
        CHECK(act::isStepping(id) == (std::string_view(id) != "lens_calibrate"));
        CHECK(act::warnOnTurn(id) == (std::string_view(id) == "lens_calibrate"));
        CHECK(((spec->sources & act::Rotation) != 0) == (std::string_view(id) != "lens_calibrate"));
        CHECK((spec->sources & act::Button) != 0);
    }
    CHECK(act::canonicalAction("Lens-Focus-Up") == "lens_focus_up");
    CHECK(act::suggestion("lens_iris_upp") == "lens_iris_up");

    // The existing families still count as stepping, the new ones do not leak into the old.
    CHECK(act::isStepping("iso_up") && act::isStepping("preview_opacity_down") && !act::isStepping("iso"));
    CHECK(!act::isStepping("lens_focus") && !act::isStepping("lens_iris_value"));

    // Analog targets: both listed, the aliases land on them, and "focus" still means focus peaking.
    auto listed = [](std::string_view target) {
        for (const auto t : act::kAnalogTargets) if (t == target) return true;
        return false;
    };
    CHECK(listed("lens_focus") && listed("lens_iris"));
    for (const char *alias : {"lens_focus", "focus_position", "Focus-Position"}) CHECK(act::canonicalAnalogTarget(alias) == "lens_focus");
    for (const char *alias : {"lens_iris", "iris"}) CHECK(act::canonicalAnalogTarget(alias) == "lens_iris");
    CHECK(act::canonicalAnalogTarget("focus") == "focus_peaking_threshold");
    CHECK(act::canonicalAnalogTarget("headphones") == "headphone_volume" && act::canonicalAnalogTarget("off").empty());

    // The catalogue the WebUI builds its pickers from.
    const std::string json = act::catalogJson();
    CHECK(json.find("\"id\":\"lens_iris_up\",\"group\":\"Lens (Pinefeat)\"") != std::string::npos);
    CHECK(json.find("\"lens_focus\",\"lens_iris\"]") != std::string::npos);

    if (failures == 0) std::puts("controls_test: ok");
    return failures == 0 ? 0 : 1;
}
