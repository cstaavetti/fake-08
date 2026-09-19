// POSIX smoke test of the exported core API, including immediate startup load.
// c++ -std=c++17 -Iplatform/libretro test/libretro/state-roundtrip.cpp -ldl -o /tmp/state-roundtrip
// /tmp/state-roundtrip /path/to/fake08_libretro.so /path/to/cart.p8.png
#include "libretro.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<uint64_t> frames;
static void logMessage(retro_log_level, const char*, ...) {}
static bool environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = "/tmp"; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = logMessage; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = false; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        static_cast<retro_variable*>(data)->value = nullptr; return false;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 0; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: return true;
    default: return false;
    }
}
static void video(const void* data, unsigned width, unsigned height, size_t pitch) {
    uint64_t hash = 14695981039346656037ull;
    auto* p = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width * 2; ++x) hash = (hash ^ p[y * pitch + x]) * 1099511628211ull;
    frames.push_back(hash);
}
static void audio(int16_t, int16_t) {}
static size_t audioBatch(const int16_t*, size_t frames) { return frames; }
static void poll() {}
static int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    require(argc == 3, "usage: state-roundtrip CORE CART");
    void* core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(core != nullptr, "cannot load core");
#define CORE(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(core, #name)); require(name != nullptr, #name)
    CORE(retro_set_environment); CORE(retro_set_video_refresh);
    CORE(retro_set_audio_sample); CORE(retro_set_audio_sample_batch);
    CORE(retro_set_input_poll); CORE(retro_set_input_state);
    CORE(retro_init); CORE(retro_deinit); CORE(retro_load_game); CORE(retro_unload_game);
    CORE(retro_run); CORE(retro_serialize_size); CORE(retro_serialize); CORE(retro_unserialize);
    retro_set_environment(environment); retro_set_video_refresh(video);
    retro_set_audio_sample(audio); retro_set_audio_sample_batch(audioBatch);
    retro_set_input_poll(poll); retro_set_input_state(input);
    retro_game_info game{};
    game.path = argv[2];
    retro_init(); require(retro_load_game(&game), "load failed");
    for (int i = 0; i < 120; ++i) retro_run();
    std::vector<uint8_t> state(retro_serialize_size());
    require(retro_serialize(state.data(), state.size()), "save failed");
    auto advance = [&] {
        frames.clear(); std::srand(42);
        for (int i = 0; i < 120; ++i) retro_run();
        return frames;
    };
    const auto expected = advance();
    require(retro_unserialize(state.data(), state.size()), "restore failed");
    require(advance() == expected, "same-instance video replay differs");
    retro_unload_game(); retro_deinit();
    retro_init(); require(retro_load_game(&game), "fresh load failed");
    require(retro_unserialize(state.data(), state.size()), "immediate startup restore failed");
    require(advance() == expected, "fresh-instance video replay differs");
    state[100] ^= 1;
    require(!retro_unserialize(state.data(), state.size()), "corrupt state accepted");
    retro_unload_game(); retro_deinit();
    dlclose(core);
    std::printf("Libretro round trip passed: %s\n", argv[2]);
}
