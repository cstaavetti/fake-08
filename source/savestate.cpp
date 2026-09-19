#include "vm.h"
#include "logger.h"
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
// Version 2 stores full audio and VM state. Old snapshots may have no Lua
// payload at all; reject them rather than reporting a successful partial load.
constexpr size_t luaCapacity = 4 * 1024 * 1024;
constexpr size_t metadataCapacity = 64 * 1024;
constexpr size_t headerSize = 32;
constexpr uint32_t magic = 0x00023866; // f8, version 2, little endian

uint32_t hashBytes(const void* ptr, size_t size, uint32_t hash = 2166136261u) {
    const auto* bytes = static_cast<const uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

uint32_t cartHash(const Cart& cart) {
    return hashBytes(cart.CartRom.data, sizeof(cart.CartRom.data),
        hashBytes(cart.LuaString.data(), cart.LuaString.size()));
}

// Audio contains no pointers, but its raw layout is build/architecture specific.
// Eris also checks the Lua bytecode ABI when restoring its own payload.
uint32_t audioABI() {
    const uint32_t endian = 1;
    return (static_cast<uint32_t>(*reinterpret_cast<const uint8_t*>(&endian)) << 24)
        | (sizeof(void*) << 16) | (sizeof(double) << 8) | alignof(audioState_t);
}

struct Writer {
    std::vector<uint8_t> bytes;
    void u32(uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(value >> (8 * i));
    }
    void raw(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void str(const std::string& value) { u32(value.size()); raw(value.data(), value.size()); }
};

struct Reader {
    const uint8_t* data;
    size_t left;
    bool ok = true;
    bool raw(void* dest, size_t size) {
        if (!ok || size > left) { ok = false; return false; }
        std::memcpy(dest, data, size);
        data += size; left -= size;
        return true;
    }
    uint32_t u32() {
        uint8_t b[4] = {};
        raw(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    std::string str() {
        const size_t size = u32();
        if (!ok || size > left) { ok = false; return {}; }
        std::string result(reinterpret_cast<const char*>(data), size);
        data += size; left -= size;
        return result;
    }
};
}

size_t Vm::SaveStateSize() {
    return headerSize + luaCapacity + sizeof(PicoRam) + sizeof(audioState_t) + metadataCapacity;
}

bool Vm::SerializeState(void* data, size_t size) {
    static_assert(std::is_trivially_copyable<audioState_t>::value, "audio must not contain owned resources");
    if (!data || size < SaveStateSize() || !_loadedCart || !_luaState || _cartChangeQueued) return false;

    Writer metadata;
    metadata.u32(_picoFrameCount);
    metadata.u32(_targetFps);
    metadata.u32(_pauseMenu);
    metadata.u32(_clearInputOnResume);
    metadata.raw(_drawStateCopy, sizeof(_drawStateCopy));
    metadata.u32(_cartdataKeyCount);
    for (int i = 0; i < _cartdataKeyCount; ++i) metadata.str(_cartdataKeys[i]);
    metadata.str(_currentCartdataKey);
    metadata.str(_cartBreadcrumb);
    metadata.str(_cartParam);
    if (metadata.bytes.size() > metadataCapacity) return false;

    std::vector<char> lua(luaCapacity);
    const size_t luaSize = serializeLuaState(lua.data(), lua.size());
    if (!luaSize) return false;
    Writer body;
    body.raw(lua.data(), luaSize);
    body.raw(_memory->data, sizeof(PicoRam));
    body.raw(_audio->getAudioState(), sizeof(audioState_t));
    body.raw(metadata.bytes.data(), metadata.bytes.size());

    Writer header;
    header.u32(magic);
    header.u32(luaSize);
    header.u32(sizeof(PicoRam));
    header.u32(sizeof(audioState_t));
    header.u32(metadata.bytes.size());
    header.u32(cartHash(*_loadedCart));
    header.u32(audioABI());
    header.u32(hashBytes(body.bytes.data(), body.bytes.size()));
    std::memset(data, 0, SaveStateSize());
    std::memcpy(data, header.bytes.data(), headerSize);
    std::memcpy(static_cast<uint8_t*>(data) + headerSize, body.bytes.data(), body.bytes.size());
    return true;
}

bool Vm::DeserializeState(const void* data, size_t size) {
    if (!data || size < headerSize || !_loadedCart || !_luaState) return false;
    Reader header{static_cast<const uint8_t*>(data), headerSize};
    if (header.u32() != magic) {
        Logger_Write("Unsupported FAKE-08 save-state version; restart the cart and create a new save\n");
        return false;
    }
    const size_t luaSize = header.u32();
    const size_t ramSize = header.u32();
    const size_t audioSize = header.u32();
    const size_t metadataSize = header.u32();
    const uint32_t identity = header.u32();
    const uint32_t abi = header.u32();
    const uint32_t checksum = header.u32();
    if (!luaSize || luaSize > luaCapacity || ramSize != sizeof(PicoRam) ||
        audioSize != sizeof(audioState_t) || metadataSize > metadataCapacity ||
        identity != cartHash(*_loadedCart) || abi != audioABI()) return false;
    const size_t bodySize = luaSize + ramSize + audioSize + metadataSize;
    if (bodySize > size - headerSize) return false;
    const auto* body = static_cast<const uint8_t*>(data) + headerSize;
    if (checksum != hashBytes(body, bodySize)) return false;

    Reader metadata{body + luaSize + ramSize + audioSize, metadataSize};
    const uint32_t frameCount = metadata.u32();
    const uint32_t fps = metadata.u32();
    const uint32_t paused = metadata.u32();
    const uint32_t clearInput = metadata.u32();
    uint8_t drawState[64];
    metadata.raw(drawState, sizeof(drawState));
    const uint32_t keyCount = metadata.u32();
    if (!metadata.ok || keyCount > 4 || frameCount > uint32_t(std::numeric_limits<int>::max()) ||
        (fps != 30 && fps != 60) || paused > 1 || clearInput > 1) return false;
    std::string keys[4];
    for (uint32_t i = 0; i < keyCount; ++i) {
        keys[i] = metadata.str();
        if (keys[i].empty() || keys[i].size() > 64) return false;
    }
    const std::string currentKey = metadata.str();
    const std::string breadcrumb = metadata.str();
    const std::string param = metadata.str();
    if (!metadata.ok || metadata.left || currentKey.size() > 64) return false;

    // Validate everything before touching the running game's RAM/audio.
    if (!deserializeLuaState(reinterpret_cast<const char*>(body), luaSize)) return false;
    std::memcpy(_memory->data, body + luaSize, ramSize);
    std::memcpy(_audio->getAudioState(), body + luaSize + ramSize, audioSize);
    _picoFrameCount = frameCount;
    _targetFps = fps;
    _pauseMenu = paused;
    _clearInputOnResume = clearInput;
    std::memcpy(_drawStateCopy, drawState, sizeof(drawState));
    _cartdataKeyCount = keyCount;
    for (int i = 0; i < 4; ++i) _cartdataKeys[i] = keys[i];
    _currentCartdataKey = currentKey;
    _cartBreadcrumb = breadcrumb;
    _cartParam = param;
    _cartChangeQueued = false;
    _cartLoadError.clear();
    // Physical controls belong to the current host, not the saved session.
    _input->SetState(0, 0);
    _input->SetMouse(0, 0, 0);
    _input->SetKeyboard(false, "");
    _audio->setPaused(_pauseMenu);
    return true;
}
