#include <string>
#include "doctest.h"
#include "stubhost.h"
#include "../source/vm.h"

struct PauseHost : StubHost {
    PauseHost() { stubInput(0, 0); }
    ~PauseHost() { stubInput(0, 0); }
};

static void loadPauseCart(Vm& vm, const std::string& loop) {
    const std::string cart = "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n" + loop;
    REQUIRE(vm.LoadCart(reinterpret_cast<const unsigned char*>(cart.data()), cart.size(), false));
    vm.vm_run();
}

TEST_CASE("Pause input survives every host frame alignment") {
    for (const auto& loop : {
        "function _update() end\nfunction _draw() end\n",
        "function _update60() end\nfunction _draw() end\n",
        "while true do flip() end\n"}) {
        for (int phase : {0, 1}) {
            for (int duration : {1, 30}) {
                CAPTURE(loop); CAPTURE(phase); CAPTURE(duration);
                PauseHost host;
                host.stubInput(0, 0);
                Vm vm(&host);
                loadPauseCart(vm, loop);
                for (int i = 0; i < 4 + phase; ++i) vm.Step();
                for (bool paused : {true, false, true}) {
                    for (int i = 0; i < duration; ++i) {
                        host.stubInput(i == 0 ? 64 : 0, 64);
                        vm.Step();
                        CHECK(vm.IsPaused() == paused);
                    }
                    host.stubInput(0, 0);
                    vm.Step();
                    vm.Step();
                    CHECK(vm.IsPaused() == paused);
                }
                CHECK(vm.GetBiosError().empty());
            }
        }
    }
}

TEST_CASE("Suppressed pause consumes the press until release") {
    PauseHost host;
    host.stubInput(0, 0);
    Vm vm(&host);
    loadPauseCart(vm, "function _update() end\n");
    vm.getPicoRam()->drawState.suppressPause = 1;
    for (int i = 0; i < 20; ++i) {
        host.stubInput(i == 0 ? 64 : 0, 64);
        vm.Step();
        CHECK_FALSE(vm.IsPaused());
    }
    host.stubInput(0, 0);
    vm.Step();
    host.stubInput(64, 64);
    vm.Step();
    CHECK(vm.IsPaused());
}

TEST_CASE("Action-button resume does not block a subsequent pause") {
    PauseHost host;
    host.stubInput(0, 0);
    Vm vm(&host);
    loadPauseCart(vm, "function _update() poke(0x4300,btn(4) and 1 or 0) end\n");
    host.stubInput(64, 64); vm.Step();
    REQUIRE(vm.IsPaused());
    host.stubInput(0, 0); vm.Step();
    host.stubInput(16, 16); vm.Step();
    REQUIRE_FALSE(vm.IsPaused());
    for (int i = 0; i < 8; ++i) {
        host.stubInput(0, 16); vm.Step();
        CHECK(vm.vm_peek(0x4300) == 0);
    }
    host.stubInput(64, 80); vm.Step();
    CHECK(vm.IsPaused());
}
