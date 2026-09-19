// POSIX exported-API test. Run with both 30 fps and 60 fps cartridges.
// c++ -std=c++17 -Iplatform/libretro test/libretro/audio-cadence.cpp -o /tmp/audio-cadence
// /tmp/audio-cadence /path/to/fake08_libretro.dylib /path/to/cart.p8
// On Linux, add -ldl when compiling.
#include "libretro.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static size_t delivered = 0;
static unsigned calls = 0;
static size_t nonzeroSamples = 0;
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
static void video(const void*, unsigned, unsigned, size_t) {}
static void audio(int16_t, int16_t) { ++delivered; }
static size_t audioBatch(const int16_t* data, size_t frames) {
    ++calls;
    delivered += frames;
    for (size_t i = 0; i < frames; ++i) {
        if (data[i * 2] != data[i * 2 + 1]) std::abort();
        if (data[i * 2]) ++nonzeroSamples;
    }
    return frames;
}
static void poll() {}
static int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    require(argc == 3 || argc == 4, "usage: audio-cadence CORE CART [--require-sound]");
    const bool requireSound = argc == 4 && std::strcmp(argv[3], "--require-sound") == 0;
    void* core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(core != nullptr, "cannot load core");
#define CORE(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(core, #name)); require(name != nullptr, #name)
    CORE(retro_set_environment); CORE(retro_set_video_refresh);
    CORE(retro_set_audio_sample); CORE(retro_set_audio_sample_batch);
    CORE(retro_set_input_poll); CORE(retro_set_input_state);
    CORE(retro_init); CORE(retro_load_game);
    CORE(retro_run); CORE(retro_reset); CORE(retro_get_system_av_info);
    retro_set_environment(environment); retro_set_video_refresh(video);
    retro_set_audio_sample(audio); retro_set_audio_sample_batch(audioBatch);
    retro_set_input_poll(poll); retro_set_input_state(input);
    retro_game_info game{};
    game.path = argv[2];
    retro_init(); require(retro_load_game(&game), "load failed");
    retro_system_av_info av{};
    retro_get_system_av_info(&av);
    require(av.timing.fps == 60 && av.timing.sample_rate == 22050, "unexpected timing");
    unsigned emptyFrames = 0, oversizedFrames = 0;
    // Run for an odd number of frames, reset, and continue across the boundary.
    for (int i = 0; i < 3600; ++i) {
        if (i == 1801) {
            std::printf("nonzero_samples_before_reset=%zu\n", nonzeroSamples);
            if (requireSound) require(nonzeroSamples > 0, "sound commands were lost before reset");
            nonzeroSamples = 0;
            retro_reset();
        }
        size_t before = delivered;
        calls = 0;
        retro_run();
        size_t count = delivered - before;
        if (!count) ++emptyFrames;
        if (count > 368) ++oversizedFrames;
        require(count == 0 || calls == 1, "multiple batches in one frame");
        if (i % 2 == 1) require(delivered == size_t(i + 1) / 2 * 735, "sample clock drift");
    }
    std::printf("frames=3600 samples=%zu empty_frames=%u oversized_frames=%u\n",
                delivered, emptyFrames, oversizedFrames);
    std::printf("nonzero_samples_after_reset=%zu\n", nonzeroSamples);
    if (requireSound) require(nonzeroSamples > 0, "sound commands were lost after reset");
    require(emptyFrames == 0, "audio must be delivered every host frame");
    require(oversizedFrames == 0, "audio batches must cover at most one host frame");
    std::printf("Libretro audio cadence passed: %s\n", argv[2]);
}
