#pragma once

#include <M5Unified.h>
#include <driver/ppa.h>

// Direct access to the Tab5's DSI framebuffer. The panel is portrait natively, so
// drawing in landscape makes M5GFX copy pixel by pixel across cache lines; the
// PPA rotates in hardware instead. Use only from the task that draws the UI.
namespace fb {

struct Target {
    void* buffer = nullptr;
    uint32_t size = 0;
    int picW = 0, picH = 0;  // native panel size
    int x = 0, y = 0;        // top-left of the rectangle in the native framebuffer
    int rotation = 0;        // display rotation as quarter turns clockwise from native
};

// Maps a rectangle in display coordinates to the native framebuffer. False if it
// does not fit on the screen or the panel has no framebuffer.
bool map(int x, int y, int w, int h, Target& t);

// PPA angle (counter-clockwise) that undoes the display rotation, plus extra quarter turns.
ppa_srm_rotation_angle_t angle(const Target& t, int extraQuarterTurns = 0);

// Same as canvas.pushSprite(x, y), done by the PPA.
void push(M5Canvas& canvas, int x, int y);

// The PPA writes the framebuffer behind the CPU cache; call before reading pixels back.
void syncForRead();

}  // namespace fb
