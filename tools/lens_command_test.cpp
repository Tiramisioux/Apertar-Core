// Host test: the set_lens / get_lens lines the WebUI, the socket and the hardware dispatch send reach the handler as the
// fields it reads (an empty "select" is present, a JSON true is the text "true", numbers stay numbers).
// Build: g++ -std=c++20 -I src -I include tools/lens_command_test.cpp src/ipc/ControlProtocol.cpp

#include "ipc/ControlProtocol.hpp"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    const auto set = apertar::parseControlCommand(
        "{\"cmd\":\"set_lens\",\"select\":\"\",\"calibrate\":true,\"iris\":5.6,\"focus_step\":-2,"
        "\"name\":\"Sigma 18-35\",\"save_over\":true,\"control\":\"Focus, Absolute\",\"value\":600}");
    CHECK(set.ok && set.command.name == "set_lens");
    CHECK(set.command.string("select") && set.command.string("select")->empty());
    CHECK(set.command.string("calibrate") == "true" && set.command.string("save_over") == "true");
    CHECK(set.command.number("iris") == 5.6 && set.command.number("focus_step") == -2.0 && set.command.number("value") == 600.0);
    CHECK(set.command.string("name") == "Sigma 18-35" && set.command.string("control") == "Focus, Absolute");
    CHECK(!set.command.string("delete") && !set.command.number("focus"));

    const auto hardware = apertar::parseControlCommand("{\"cmd\":\"set_lens\",\"iris_step\":\"3\"}");   // the dispatch lambda sends text
    CHECK(hardware.ok && hardware.command.number("iris_step") == 3.0);
    const auto get = apertar::parseControlCommand("{\"cmd\":\"get_lens\",\"id\":4}");
    CHECK(get.ok && get.command.name == "get_lens" && get.command.id == 4);

    if (failures == 0) std::puts("lens_command_test: ok");
    return failures == 0 ? 0 : 1;
}
