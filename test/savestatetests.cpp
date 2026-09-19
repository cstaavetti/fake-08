#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include "doctest.h"
#include "stubhost.h"
#include "../source/vm.h"

static const std::string stateCart =
    "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
    "counter=0\nlocal secret=100\n"
    "function _update60() counter+=1 secret+=3 poke2(0x4300,counter) poke2(0x4302,secret) end\n";

TEST_CASE("Lua state restores globals and coroutine locals") {
    StubHost host;
    host.stubInput(0, 0);
    Vm vm(&host);
    REQUIRE(vm.LoadCart(reinterpret_cast<const unsigned char*>(stateCart.data()), stateCart.size(), false));
    vm.vm_run();
    for (int i = 0; i < 10; ++i) vm.Step();
    std::vector<char> buffer(1024 * 1024);
    size_t size = vm.serializeLuaState(buffer.data(), buffer.size());
    REQUIRE(size > 0);
    for (int i = 0; i < 10; ++i) vm.Step();
    vm.deserializeLuaState(buffer.data(), size);
    vm.Step();
    CHECK(vm.vm_peek2(0x4300) == 11);
    CHECK(vm.vm_peek2(0x4302) == 133);
}

static void startStateCart(Vm& vm, const std::string& cart = stateCart) {
    REQUIRE(vm.LoadCart(reinterpret_cast<const unsigned char*>(cart.data()), cart.size(), false));
    vm.vm_run();
    for (int i = 0; i < 10; ++i) REQUIRE(vm.Step());
}

TEST_CASE("Save states restore into the current and a fresh VM") {
    StubHost host;
    host.stubInput(0, 0);
    Vm vm(&host);
    startStateCart(vm);
    std::vector<uint8_t> state(Vm::SaveStateSize());
    REQUIRE(vm.SerializeState(state.data(), state.size()));
    for (int i = 0; i < 10; ++i) vm.Step();
    REQUIRE(vm.DeserializeState(state.data(), state.size()));
    CHECK(vm.GetFrameCount() == 10);
    vm.Step();
    CHECK(vm.vm_peek2(0x4300) == 11);
    CHECK(vm.vm_peek2(0x4302) == 133);
    Vm fresh(&host);
    startStateCart(fresh);
    REQUIRE(fresh.DeserializeState(state.data(), state.size()));
    fresh.Step();
    CHECK(fresh.vm_peek2(0x4300) == 11);
    CHECK(fresh.vm_peek2(0x4302) == 133);
    // These APIs use the restored sandbox, including the native fallback.
    CHECK(fresh.ExecuteLua("function check() return counter==11 end", "check"));
    REQUIRE(fresh.SerializeState(state.data(), state.size()));
}

TEST_CASE("Invalid save states fail without changing the game") {
    StubHost host;
    host.stubInput(0, 0);
    Vm vm(&host);
    startStateCart(vm);
    std::vector<uint8_t> state(Vm::SaveStateSize());
    REQUIRE(vm.SerializeState(state.data(), state.size()));
    for (size_t size : {size_t(0), size_t(3), size_t(31), size_t(32), size_t(100)}) {
        CHECK_FALSE(vm.DeserializeState(state.data(), size));
    }
    for (size_t index : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), size_t(20), size_t(24), size_t(28), size_t(100)}) {
        auto bad = state;
        bad[index] ^= 255;
        CHECK_FALSE(vm.DeserializeState(bad.data(), bad.size()));
    }
    CHECK_FALSE(vm.SerializeState(state.data(), state.size() - 1));
    CHECK_FALSE(vm.DeserializeState(nullptr, state.size()));
    CHECK_FALSE(vm.DeserializeState("f8\0\1", 4));
    char small[2] = {42, 43};
    for (int i = 0; i < 50; ++i) {
        CHECK(vm.serializeLuaState(small, 1) == 0);
        CHECK_FALSE(vm.deserializeLuaState("invalid", 7));
    }
    CHECK(small[0] == 42);
    CHECK(small[1] == 43);
    vm.Step();
    CHECK(vm.vm_peek2(0x4300) == 11);
    REQUIRE(vm.SerializeState(state.data(), state.size()));
    Vm other(&host);
    startStateCart(other, stateCart + "\n-- different cartridge\n");
    CHECK_FALSE(other.DeserializeState(state.data(), state.size()));
}

TEST_CASE("Save states resume manual coroutine loops and paused menus") {
    StubHost host;
    host.stubInput(0, 0);
    Vm vm(&host);
    const std::string cart =
        "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n"
        "cartdata('state-test')\nlocal n=0\n"
        "menuitem(1,'increment',function() n+=100 end)\n"
        "while true do n+=1 poke2(0x4300,n) dset(0,n) flip() end\n";
    startStateCart(vm, cart);
    const int saved = vm.vm_peek2(0x4300);
    vm.togglePauseMenu();
    std::vector<uint8_t> state(Vm::SaveStateSize());
    REQUIRE(vm.SerializeState(state.data(), state.size()));
    Vm fresh(&host);
    startStateCart(fresh, cart);
    REQUIRE(fresh.DeserializeState(state.data(), state.size()));
    REQUIRE(fresh.IsPaused());
    fresh.Step();
    CHECK(fresh.vm_peek2(0x4300) == saved);
    // Select the saved custom callback, then invoke it with O.
    host.stubInput(8, 8); fresh.Step();
    host.stubInput(0, 0); fresh.Step();
    host.stubInput(16, 16); fresh.Step();
    host.stubInput(0, 0);
    REQUIRE_FALSE(fresh.IsPaused());
    fresh.Step();
    CHECK(fresh.vm_peek2(0x4300) == saved + 101);
    CHECK(fresh.vm_dget(0) == fix32(saved + 101));
}

TEST_CASE("Real cartridges replay identically after state restoration") {
    const char* directory = std::getenv("FAKE08_STATE_CART_DIR");
    if (!directory) { MESSAGE("Set FAKE08_STATE_CART_DIR to run real-cart save-state replay"); return; }
    for (const char* name : {"001 - Celeste Classic.p8.png", "002 - Just One Boss.p8.png", "003 - Celeste Classic 2.p8.png"}) {
        CAPTURE(name);
        std::ifstream file(std::string(directory) + "/" + name, std::ios::binary);
        std::vector<unsigned char> cart((std::istreambuf_iterator<char>(file)), {});
        REQUIRE_FALSE(cart.empty());
        StubHost host;
        host.stubInput(0, 0);
        Vm vm(&host);
        REQUIRE(vm.LoadCart(cart.data(), cart.size(), false));
        vm.vm_run();
        for (int i = 0; i < 120; ++i) REQUIRE(vm.Step());
        std::vector<uint8_t> state(Vm::SaveStateSize());
        REQUIRE(vm.SerializeState(state.data(), state.size()));
        auto advance = [&](Vm& game) {
            // Waveform noise uses the process RNG, outside the emulator state.
            std::srand(42);
            std::vector<int16_t> sound(735 * 2);
            std::vector<int16_t> allSound;
            for (int i = 0; i < 120; ++i) {
                uint8_t held = i < 20 ? 16 : (i < 80 ? 2 : 0);
                uint8_t down = i == 0 ? 16 : (i == 20 ? 2 : 0);
                host.stubInput(down, held);
                REQUIRE(game.Step());
                game.FillAudioBuffer(sound.data(), 0, 735);
                allSound.insert(allSound.end(), sound.begin(), sound.end());
            }
            host.stubInput(0, 0);
            return allSound;
        };
        const auto sound = advance(vm);
        std::vector<uint8_t> ram(vm.getPicoRam()->data, vm.getPicoRam()->data + sizeof(PicoRam));
        REQUIRE(vm.DeserializeState(state.data(), state.size()));
        CHECK(advance(vm) == sound);
        CHECK(std::memcmp(ram.data(), vm.getPicoRam()->data, ram.size()) == 0);
        Vm fresh(&host);
        REQUIRE(fresh.LoadCart(cart.data(), cart.size(), false));
        fresh.vm_run();
        // A frontend may load before the first game frame has run.
        REQUIRE(fresh.DeserializeState(state.data(), state.size()));
        CHECK(advance(fresh) == sound);
        CHECK(std::memcmp(ram.data(), fresh.getPicoRam()->data, ram.size()) == 0);
    }
}
