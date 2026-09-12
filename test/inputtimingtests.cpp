#include <string>
#include <vector>
#include "doctest.h"
#include "stubhost.h"
#include "../source/vm.h"

TEST_CASE("Input timing through the real VM game loop") {
    for (int fps : {30, 60}) {
        for (int phase : {0, 1}) {
            CAPTURE(fps);
            CAPTURE(phase);
            StubHost host;
            Vm vm(&host);
            host.stubInput(0, 0);
            REQUIRE(vm.LoadCart("input-timing-" + std::to_string(fps) + ".p8", false));
            vm.vm_run();
            bool previous = false;
            // Model libretro's 60 Hz, one-host-frame rising edge.
            // Try a held button beginning on both host-frame parities.
            for (int frame = 0; frame < 150; ++frame) {
                bool held = frame >= 4 + phase && frame < 129 + phase;
                host.stubInput(held && !previous ? 1 : 0, held ? 1 : 0);
                vm.Step();
                previous = held;
            }
            REQUIRE(vm.GetBiosError().empty());
            CHECK(vm.vm_peek2(0x4300) == 1); // held-state rising edges
            CHECK(vm.vm_peek2(0x4304) == 0); // no missing initial btnp
            int events = vm.vm_peek2(0x4302);
            REQUIRE(events >= 10); // repeats continue beyond the first one
            int first = vm.vm_peek2(0x4400);
            CHECK(vm.vm_peek2(0x4402) - first == fps / 2);
            for (int i = 2; i < events; ++i) {
                CHECK(vm.vm_peek2(0x4400 + i * 2) -
                      vm.vm_peek2(0x4400 + (i - 1) * 2) == fps * 4 / 30);
            }
        }
    }
}

TEST_CASE("Input repeat registers and sampled edges") {
    for (int fps : {30, 60}) {
        PicoRam ram;
        ram.Reset();
        Input input(&ram);
        const int scale = fps / 30;
        ram.hwState.btnpRepeatDelay = 3;
        ram.hwState.btnpRepeatInterval = 5;
        std::vector<int> events;
        for (int frame = 0; frame <= 14 * scale; ++frame) {
            // No host edge: detect the press from the sampled held state.
            input.SetState(0, 1, fps);
            CHECK(input.btn(0));
            if (input.btnp(0)) events.push_back(frame);
        }
        CHECK(events == std::vector<int>{0, 3 * scale, 8 * scale, 13 * scale});
        // Disabled repeat must still reset on release and detect new presses.
        ram.hwState.btnpRepeatDelay = 255;
        input.SetState(0, 0, fps);
        input.SetState(0, 1, fps);
        CHECK(input.btnp(0));
        bool repeated = false;
        for (int frame = 0; frame < 1000; ++frame) {
            input.SetState(0, 1, fps);
            repeated |= input.btnp(0);
        }
        CHECK_FALSE(repeated);
        input.SetState(0, 0, fps);
        ram.hwState.btnpRepeatDelay = 0;
        ram.hwState.btnpRepeatInterval = 0;
        input.SetState(0, 1, fps);
        CHECK(input.btnp(0));
        int firstRepeat = 0;
        for (int frame = 1; frame <= fps; ++frame) {
            input.SetState(0, 1, fps);
            if (input.btnp(0) && !firstRepeat) firstRepeat = frame;
        }
        CHECK(firstRepeat == fps / 2);
        // Held pause must not generate repeated pause presses.
        input.SetState(64, 64, fps);
        CHECK(input.btnp(6));
        repeated = false;
        for (int frame = 0; frame < fps * 2; ++frame) {
            input.SetState(0, 64, fps);
            repeated |= input.btnp(6);
        }
        CHECK_FALSE(repeated);
        input.SetState(0, 0, fps); // menu transition clears the game input
        input.SetState(0, 64, fps); // physical pause button is still held
        CHECK_FALSE(input.btnp(6));
    }
}
