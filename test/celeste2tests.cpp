#include <cstdlib>
#include <string>
#include "doctest.h"
#include "stubhost.h"
#include "../source/vm.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

TEST_CASE("Missing table fields do not resolve to cart globals") {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int result = luaL_dostring(L,
        "debug.getregistry().__PICO8_SANDBOX={dd=42}\n"
        "local entity={}\n"
        "local missing=entity.dd\n"
        "entity.used=true\n"
        "assert(missing==nil,'missing field resolved to global')\n");
    CHECK_MESSAGE(result == LUA_OK, lua_tostring(L, -1));
    lua_close(L);
}

// Optional local integration test; the third-party cart is not bundled here.
TEST_CASE("Celeste 2 starts and runs with scripted input") {
    const char* path = std::getenv("FAKE08_CELESTE2_CART");
    if (!path) return;
    StubHost host;
    Vm vm(&host);
    host.stubInput(0, 0);
    REQUIRE(vm.LoadCart(path, false));
    vm.vm_run();
    REQUIRE(vm.GetBiosError().empty());
    unsigned previous = 0;
    for (int frame = 0; frame < 7200; ++frame) {
        // Start from title, then exercise movement, jump and grapple for 2 min.
        unsigned held = frame < 120 ? 0 :
            ((frame / 240) % 2 ? 1 : 2) |
            (frame % 90 < 12 ? 16 : 0) |
            (frame % 120 < 24 ? 32 : 0);
        host.stubInput(held & ~previous, held);
        vm.Step();
        previous = held;
        if (!vm.GetBiosError().empty()) {
            INFO("host frame: " << frame << "; " << vm.GetBiosError());
            REQUIRE(vm.GetBiosError().empty());
        }
    }
    CHECK(vm.GetBiosError().empty());
}
