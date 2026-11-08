#include "display_fb.h"

#include <esp_cache.h>
#include <lgfx/v1/panel/Panel_FrameBufferBase.hpp>

#include <utility>

namespace fb {

namespace {

ppa_client_handle_t ppa = nullptr;
bool ppaFailed = false;

// The framebuffer pointer is protected in M5GFX; a member pointer taken through a
// derived class reads it without changing the library.
struct FrameBufferAccess : lgfx::Panel_FrameBufferBase {
    static constexpr uint8_t** lgfx::Panel_FrameBufferBase::*lines = &FrameBufferAccess::_lines_buffer;
};

bool ensurePpa() {
    if (ppa) return true;
    if (ppaFailed) return false;
    ppa_client_config_t pc = {};
    pc.oper_type = PPA_OPERATION_SRM;
    pc.max_pending_trans_num = 1;
    ppaFailed = ppa_register_client(&pc, &ppa) != ESP_OK;
    return !ppaFailed;
}

}  // namespace

bool map(int x, int y, int w, int h, Target& t) {
    int dispW = M5.Display.width(), dispH = M5.Display.height();
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > dispW || y + h > dispH) return false;
    auto* panel = static_cast<lgfx::Panel_FrameBufferBase*>(M5.Display.getPanel());
    uint8_t** lines = panel->*FrameBufferAccess::lines;
    if (!lines) return false;
    const auto& pc = panel->config();
    int r = (M5.Display.getRotation() + pc.offset_rotation) & 3;

    // Same mapping as Panel_FrameBufferBase::display().
    int px = x, py = y;
    if ((1u << r) & 0b0110) py = dispH - (y + h);
    if (r & 2) px = dispW - (x + w);
    if (r & 1) std::swap(px, py);

    t.buffer = lines[0];
    t.picW = pc.panel_width;
    t.picH = pc.panel_height;
    t.size = (uint32_t)t.picW * t.picH * 2;
    t.x = px;
    t.y = py;
    t.rotation = r;
    return true;
}

ppa_srm_rotation_angle_t angle(const Target& t, int extraQuarterTurns) {
    static constexpr ppa_srm_rotation_angle_t ANGLES[] = {PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
                                                          PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270};
    return ANGLES[(4 - t.rotation + extraQuarterTurns) & 3];
}

void push(M5Canvas& canvas, int x, int y) {
    int w = canvas.width(), h = canvas.height();
    Target t;
    if (canvas.getColorDepth() != 16 || !canvas.getBuffer() || !map(x, y, w, h, t) || !ensurePpa()) {
        canvas.pushSprite(x, y);
        return;
    }
    ppa_srm_oper_config_t op = {};
    op.in.buffer = canvas.getBuffer();
    op.in.pic_w = w;
    op.in.pic_h = h;
    op.in.block_w = w;
    op.in.block_h = h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = t.buffer;
    op.out.buffer_size = t.size;
    op.out.pic_w = t.picW;
    op.out.pic_h = t.picH;
    op.out.block_offset_x = t.x;
    op.out.block_offset_y = t.y;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = angle(t);
    op.scale_x = 1;
    op.scale_y = 1;
    op.byte_swap = true;  // M5GFX canvases hold RGB565 big-endian; the panel wants little-endian
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(ppa, &op) != ESP_OK) canvas.pushSprite(x, y);
}

void syncForRead() {
    Target t;
    if (!map(0, 0, M5.Display.width(), M5.Display.height(), t)) return;
    esp_cache_msync(t.buffer, t.size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

}  // namespace fb
