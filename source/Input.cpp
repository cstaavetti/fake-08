#include <algorithm>

#include "Input.h"
#include "hostVmShared.h"
#include "PicoRam.h"

Input::Input(PicoRam* memory):
    _currentKDown(0),
    _currentKHeld(0),
    _previousKHeld(0)
{
    _memory = memory;

    std::fill(_framesHeld, _framesHeld + 8, 0);
}

void Input::SetState(uint8_t kdown, uint8_t kheld, int fps){
    // Host edges can be only one 60 Hz frame wide. A 30 Hz cart must
    // also detect edges against its own previous input sample.
    const uint8_t previousHeld = _previousKHeld;
    // Pause remains host-edge driven: toggling the menu clears game input,
    // and must not turn a still-held pause button into another press.
    _currentKDown = kdown | ((kheld & ~previousHeld) & 0x3f);
    _currentKHeld = kheld;
    _previousKHeld = kheld;
    //key 6 (PAUSE MENU) only fires for one frame, even if held
    if ((_currentKHeld & BITMASK(6)) && !(_currentKDown & BITMASK(6))) {
        _currentKHeld = _currentKHeld & ~(BITMASK(6));
    }
    //memory only stores buttons 0-5, not 6 or 7
    _memory->hwState.buttonStates[0] = kheld & 0x3F;

    uint8_t repeatDelay = _memory->hwState.btnpRepeatDelay == 0 
        ? 15 
        : _memory->hwState.btnpRepeatDelay;

    uint8_t repeatInterval = _memory->hwState.btnpRepeatInterval == 0 
        ? 4 
        : _memory->hwState.btnpRepeatInterval;

    // Repeat registers are measured in 30 Hz frames, even for _update60.
    const uint32_t scale = fps == 60 ? 2 : 1;
    const uint32_t delay = repeatDelay * scale;
    const uint32_t interval = repeatInterval * scale;
    for (int i = 0; i < 6; i ++) {
        bool down = BITMASK(i) & kheld;
        // Count elapsed updates from the initial press, which is frame zero.
        // Reset on release even when repeat is disabled.
        if (!down || !(previousHeld & BITMASK(i))) {
            _framesHeld[i] = 0;
        } else {
            ++_framesHeld[i];
        }
        bool repeatPressed = down && repeatDelay != 255 &&
            _framesHeld[i] >= delay &&
            (_framesHeld[i] - delay) % interval == 0;

        if (repeatPressed) {
            _currentKDown = _currentKDown | BITMASK(i);
        }
    }
}

void Input::SetMouse(int16_t mouseX, int16_t mouseY, uint8_t mouseBtnState){
    _mouseX = mouseX;
    _mouseY = mouseY;
    _mouseBtnState = mouseBtnState;
}

void Input::SetKeyboard(bool kbDown, std::string kbKey){
	_kbDown = kbDown;
	_kbKey = kbKey;
}

uint8_t Input::btn(){
    return _currentKHeld;
}

//todo: repetition behavior to match pico 8
uint8_t Input::btnp(){
    return _currentKDown;
}

bool Input::btn(uint8_t i){
    return BITMASK(i) & btn();
}

//todo: repetition behavior to match pico 8
bool Input::btnp(uint8_t i){
    return BITMASK(i) & btnp();
}

bool Input::btn(uint8_t i, uint8_t p){
    //no multiplayer support for now
    return p == 0 ? btn(i) : 0;
}

//todo: repetition behavior to match pico 8
bool Input::btnp(uint8_t i, uint8_t p){
    //no multiplayer support for now
    return p == 0 ? btnp(i) : 0;
}

int16_t Input::getMouseX() {
    return _mouseX;
}

int16_t Input::getMouseY() {
    return _mouseY;
}

uint8_t Input::getMouseBtnState() {
    return _mouseBtnState;
}

bool Input::getKeyDown() {
    return _kbDown;
}

const char* Input::getKey() {
    // Found this bug in Terra
    // PICO-8 behavior: stat(31) appears to consume the key from the buffer
    // After reading, stat(30) should return false until next key press
    _kbDown = false;
    return _kbKey.c_str();
}

