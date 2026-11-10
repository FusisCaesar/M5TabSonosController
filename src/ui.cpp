#include "ui.h"

#include "board.h"
#include "display_fb.h"
#include "font_ui.h"
#include "net.h"
#include "services.h"
#include "sonos.h"
#include "youtube.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui {
namespace {

uint16_t COL_BG, COL_CARD, COL_CARD2, COL_ACCENT, COL_TEXT, COL_MUTED, COL_LINE, COL_WARN;

constexpr int LIST_X = 16;
constexpr int LIST_Y = 120;
constexpr int LIST_W = 360;
constexpr int LIST_B = 704;
constexpr int ROW_H = 76;

constexpr int PANE_X = 392;
constexpr int CTRL_Y = 600;
constexpr int BAR_Y = 608;

constexpr int FAV_X = 408;
constexpr int FAV_Y = 120;
constexpr int FAV_W = 856;
constexpr int FAV_B = CTRL_Y - 12;
constexpr int FAV_H = 76;
constexpr int FAV_LIST = FAV_Y + 52;

constexpr int ART_X = 424;
constexpr int ART_Y = 136;
constexpr int ART = 180;

struct Box {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

constexpr Box PREV{626, 498, 120, 88};
constexpr Box PLAY{762, 486, 136, 108};
constexpr Box NEXT{914, 498, 120, 88};
constexpr Box VOL{400, 612, 760, 92};
constexpr Box MUTE{1184, 612, 80, 92};
constexpr Box HOME{16, 18, 72, 72};
constexpr Box BROWSE{864, 18, 72, 72};
constexpr Box QUEUE{948, 18, 72, 72};
constexpr Box FAV{1032, 18, 72, 72};
constexpr Box SET{1116, 18, 72, 72};
constexpr int BATT_X = 1208;
constexpr int BATT_Y = 41;
constexpr Box STAR_NOW{1210, 136, 46, 46};  // star beside the title while YouTube plays

M5Canvas canvas;
M5Canvas art;
uint8_t* jpg = nullptr;
constexpr size_t JPG_CAP = 1024 * 1024;
bool ready = false;
bool hasArt = false;
int shownGen = -1;
enum class Panel : uint8_t { Closed, Queue, Favs, Browse, Settings };
Panel panel = Panel::Closed;
svc::View browse;
yt::View tube;
bool youtube = false;
bool ytStars = false;  // YouTube mode shows the starred list instead of search results
bool keyboard = false;
char query[64] = "";
int browseScroll = 0;
int roomScroll = 0;
int favScroll = 0;

bool pressing = false;
bool moved = false;
int x0 = 0, y0 = 0, lastY = 0, scroll0 = 0;
enum class Drag : uint8_t { None, Rooms, Favs, Volume };
Drag drag = Drag::None;
int localVol = 0;
bool freezeVol = false;
uint32_t volFreezeUntil = 0;
uint32_t lastVolSend = 0;

uint32_t flashUntil = 0;
int flashX = 0, flashY = 0;
uint32_t lastActivity = 0;
int sleepMin = 5;
int clickLevel = 1;  // 0 off, 1 low, 2 high
bool asleep = false;
bool swallowTouch = false;
bool speakerReady = false;

// The wall mount is upside down relative to rotation 1, and that pose never changes, so the screen
// has to start there. The resting pose keeps this rotation. Turning the tablet over selects the other one.
constexpr int HOME_ROT = 3;

// True while a finger is down on the control, and for a short time after the tap, so a quick tap
// still shows.
bool pressedAt(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    bool down = pressing && !moved && x0 >= x && x0 < x + w && y0 >= y && y0 < y + h;
    bool flash = millis() < flashUntil && flashX >= x && flashX < x + w && flashY >= y && flashY < y + h;
    return down || flash;
}

int clickHz() {
    if (clickLevel == 2) return 880;
    if (clickLevel == 1) return 440;
    return 0;
}

void buzz() {
    int hz = clickHz();
    if (!speakerReady || asleep || hz <= 0) return;
    M5.Speaker.tone(hz, 40);
}

void startSpeaker() {
    // Called from setup(), before the network task exists, so the codec init does not need the I2C lock.
    // Holding the lock here can deadlock: begin() starts a task that also uses that bus.
    speakerReady = M5.Speaker.begin();
    if (speakerReady) M5.Speaker.setVolume(80);
    Serial.printf("[ui] speaker %s\n", speakerReady ? "ready" : "did not start");
    buzz();
}

struct PowerView {
    bool known = false;
    bool onUsb = false;
    bool charging = false;
    int pct = -1;
    int mv = 0;
    int ma = 0;
    int packSamples = 0;
} power;

int cellPercent(int packMv) {
    int cell = packMv / 2;
    int p = (cell - 3300) * 100 / (4150 - 3300);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

void servicePower() {
    static uint32_t next = 0;
    if (millis() < next) return;
    next = millis() + 5000;
    int mv = 0, ma = 0;
    bool charging = false;
    {
        board::I2CLock lock;
        mv = M5.Power.getBatteryVoltage();
        ma = M5.Power.getBatteryCurrent();
        charging = M5.Power.isCharging();
    }
    power.mv = mv;
    power.ma = ma;
    power.charging = charging;
    power.known = true;
    // The fuel gauge sits on the charger rail when no pack is fitted, so a single high reading is
    // not a battery. The thresholds are the ones that settled on this board.
    if (mv >= 6000 && mv <= 8800) {
        power.packSamples++;
        if (power.packSamples >= 2) {
            power.onUsb = false;
            power.pct = cellPercent(mv);
        }
    } else if (mv < 5500) {
        power.onUsb = true;
        power.pct = -1;
        power.packSamples = 0;
    }
}

// Both landscape rotations are the same 1280x720 layout, so the canvas stays. Held on its short edge,
// the last landscape orientation is kept.
struct Orient {
    bool learned = false;
    int axis = 0;
    int sign = 1;
    int hits = 0;
    int learnAxis = -1;
    int learnSign = 0;
    int candidate = HOME_ROT;
    int stable = 0;
} orient;

void serviceOrientation() {
    static uint32_t next = 0;
    static bool logged = false;
    if (pressing || millis() < 2000 || millis() < next) return;
    next = millis() + 250;
    if (!M5.Imu.isEnabled()) {
        if (!logged) {
            logged = true;
            Serial.println("[ui] no motion sensor, rotation stays landscape");
        }
        return;
    }
    float ax = 0, ay = 0, az = 0;
    bool ok;
    {
        board::I2CLock lock;
        ok = M5.Imu.getAccel(&ax, &ay, &az);
    }
    if (!ok) return;
    float axa = fabsf(ax), aya = fabsf(ay);
    int axis = axa >= aya ? 0 : 1;
    float dom = axis ? aya : axa;
    float other = axis ? axa : aya;
    if (dom < 0.7f || dom < other * 1.5f) return;
    int sgn = (axis ? ay : ax) > 0 ? 1 : -1;
    if (!orient.learned) {
        if (orient.learnAxis == axis && orient.learnSign == sgn) orient.hits++;
        else {
            orient.learnAxis = axis;
            orient.learnSign = sgn;
            orient.hits = 1;
        }
        if (orient.hits >= 4) {
            orient.learned = true;
            orient.axis = axis;
            orient.sign = sgn;
            Serial.printf("[ui] landscape axis %c sign %d\n", axis ? 'y' : 'x', sgn);
        }
        return;
    }
    if (axis != orient.axis) return;
    int rot = sgn == orient.sign ? HOME_ROT : (HOME_ROT == 3 ? 1 : 3);
    if (rot == orient.candidate) orient.stable++;
    else {
        orient.candidate = rot;
        orient.stable = 1;
    }
    if (orient.stable < 6 || M5.Display.getRotation() == rot) return;
    {
        board::I2CLock lock;
        M5.Display.setRotation(rot);
    }
    Serial.printf("[ui] rotation %d\n", rot);
}

void setBrightness(int level) {
    board::I2CLock lock;
    M5.Display.setBrightness(level);
}

void wakeScreen() {
    asleep = false;
    lastActivity = millis();
    setBrightness(200);
}

void serviceSleep() {
    if (sleepMin <= 0) {
        if (asleep) wakeScreen();
        return;
    }
    if (!asleep && lastActivity && millis() - lastActivity > (uint32_t)sleepMin * 60000UL) {
        asleep = true;
        setBrightness(0);
        Serial.printf("[ui] screen off after %d min\n", sleepMin);
    }
}

void loadSleep() {
    Preferences prefs;
    prefs.begin("ui", true);
    int saved = prefs.getInt("sleep", 5);
    int click = prefs.getInt("click", 1);
    prefs.end();
    if (saved == 0 || saved == 1 || saved == 5 || saved == 15 || saved == 30) sleepMin = saved;
    if (click >= 0 && click <= 2) clickLevel = click;
}

void storeSleep(int minutes) {
    sleepMin = minutes;
    lastActivity = millis();
    Preferences prefs;
    prefs.begin("ui", false);
    prefs.putInt("sleep", minutes);
    prefs.end();
}

void storeClick(int level) {
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    clickLevel = level;
    Preferences prefs;
    prefs.begin("ui", false);
    prefs.putInt("click", level);
    prefs.end();
}

char wifiSsid[33] = "";
char wifiPass[65] = "";
int wifiField = 0;  // 1 network name, 2 password

void drawBattery(M5Canvas& c, int x, int y) {
    c.drawRoundRect(x, y, 44, 22, 3, COL_TEXT);
    c.fillRoundRect(x + 44, y + 6, 4, 10, 1, COL_TEXT);
    if (!power.known || power.onUsb || power.pct < 0) return;
    int fill = 36 * power.pct / 100;
    uint16_t col = power.pct < 15 ? COL_WARN : (power.charging ? COL_ACCENT : COL_TEXT);
    if (fill > 0) c.fillRoundRect(x + 3, y + 3, fill, 16, 2, col);
}

void drawSpeakerGlyph(M5Canvas& c, int x, int y, uint16_t body, uint16_t hole) {
    c.fillRoundRect(x, y, 34, 44, 4, body);
    c.fillCircle(x + 17, y + 16, 10, hole);
    c.fillCircle(x + 17, y + 16, 4, body);
    c.fillRoundRect(x + 11, y + 32, 12, 5, 2, hole);
}

void drawKindIcon(M5Canvas& c, svc::Kind kind, int x, int y, uint16_t ink, uint16_t hole) {
    if (kind == svc::Kind::Track) {
        c.fillCircle(x + 9, y + 18, 6, ink);
        c.fillRect(x + 13, y + 4, 3, 16, ink);
        c.fillTriangle(x + 16, y + 4, x + 26, y + 9, x + 16, y + 11, ink);
    } else if (kind == svc::Kind::Album) {
        c.fillCircle(x + 14, y + 14, 12, ink);
        c.fillCircle(x + 14, y + 14, 4, hole);
    } else if (kind == svc::Kind::Artist) {
        c.fillCircle(x + 14, y + 8, 6, ink);
        c.fillRoundRect(x + 4, y + 15, 20, 12, 6, ink);
    }
}

void drawBrowseIcon(M5Canvas& c, int cx, int cy, uint16_t ink, uint16_t hole) {
    c.fillCircle(cx - 4, cy - 4, 14, ink);
    c.fillCircle(cx - 4, cy - 4, 9, hole);
    for (int i = 0; i < 4; i++) c.drawLine(cx + 6, cy + 6 + i, cx + 16, cy + 16 + i, ink);
}

void drawQueueIcon(M5Canvas& c, int cx, int cy, uint16_t ink) {
    for (int i = 0; i < 3; i++) c.fillRoundRect(cx - 18, cy - 16 + i * 12, 22, 5, 2, ink);
    c.fillTriangle(cx + 10, cy - 12, cx + 10, cy + 12, cx + 24, cy, ink);
}

void drawHeartIcon(M5Canvas& c, int cx, int cy, uint16_t ink) {
    c.fillCircle(cx - 7, cy - 3, 8, ink);
    c.fillCircle(cx + 7, cy - 3, 8, ink);
    c.fillTriangle(cx - 15, cy, cx + 15, cy, cx, cy + 16, ink);
}

void drawGearIcon(M5Canvas& c, int cx, int cy, uint16_t ink, uint16_t hole) {
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        c.fillCircle(cx + (int)lroundf(cosf(a) * 15), cy + (int)lroundf(sinf(a) * 15), 4, ink);
    }
    c.fillCircle(cx, cy, 11, ink);
    c.fillCircle(cx, cy, 4, hole);
}

void drawHeaderButton(M5Canvas& c, const Box& b, int kind, bool on) {
    bool hot = pressedAt(b.x, b.y, b.w, b.h);
    uint16_t bg = (on || hot) ? COL_ACCENT : COL_CARD;
    uint16_t ink = (on || hot) ? COL_BG : COL_TEXT;
    c.fillRoundRect(b.x, b.y, b.w, b.h, 16, bg);
    int cx = b.x + b.w / 2;
    int cy = b.y + b.h / 2;
    if (kind == 0) drawBrowseIcon(c, cx, cy, ink, bg);
    else if (kind == 1) drawQueueIcon(c, cx, cy, ink);
    else if (kind == 2) drawHeartIcon(c, cx, cy, ink);
    else drawGearIcon(c, cx, cy, ink, bg);
}

sonos::View view;

int listMax(int count, int viewH, int row) {
    int content = count * row;
    return content > viewH ? content - viewH : 0;
}

constexpr int ROOM_GAP = 10;
constexpr int ROOM_SOLO = ROW_H - ROOM_GAP;
constexpr int ROOM_MEMBER = 64;
int roomTop[sonos::MAX_ROOMS];
int roomH[sonos::MAX_ROOMS];
int roomsContent = 0;

int groupRun(int i) {
    if (i < 0 || i >= view.roomCount || !view.rooms[i].inGroup || !view.rooms[i].group[0]) return 1;
    int n = 1;
    while (i + n < view.roomCount && view.rooms[i + n].inGroup &&
           !strcmp(view.rooms[i + n].group, view.rooms[i].group))
        n++;
    return n;
}

void layoutRooms() {
    roomsContent = 0;
    int i = 0;
    while (i < view.roomCount && i < sonos::MAX_ROOMS) {
        int n = groupRun(i);
        int slice = n == 1 ? ROOM_SOLO : ROOM_MEMBER;
        for (int k = 0; k < n; k++) {
            roomTop[i + k] = roomsContent + k * slice;
            roomH[i + k] = slice;
        }
        roomsContent += n * slice + ROOM_GAP;
        i += n;
    }
    int viewH = LIST_B - LIST_Y;
    int maxS = roomsContent > viewH ? roomsContent - viewH : 0;
    if (roomScroll > maxS) roomScroll = maxS;
    if (roomScroll < 0) roomScroll = 0;
}

int roomIndexAt(int y) {
    if (y < LIST_Y || y >= LIST_B) return -1;
    int contentY = y - LIST_Y + roomScroll;
    for (int i = 0; i < view.roomCount; i++)
        if (contentY >= roomTop[i] && contentY < roomTop[i] + roomH[i]) return i;
    return -1;
}

enum class RoomAction : uint8_t { None, Add, Remove };

RoomAction roomAction(int i) {
    if (i < 0 || i >= view.roomCount) return RoomAction::None;
    const char* selectedGroup = "";
    for (int k = 0; k < view.roomCount; k++)
        if (view.rooms[k].coordinator) selectedGroup = view.rooms[k].group;
    if (!selectedGroup[0] || !view.rooms[i].group[0]) return RoomAction::None;
    if (!strcmp(view.rooms[i].group, selectedGroup))
        return view.rooms[i].inGroup ? RoomAction::Remove : RoomAction::None;
    return RoomAction::Add;
}

void roomActionBox(int i, int* x, int* y, int* w, int* h) {
    int rowY = LIST_Y + roomTop[i] - roomScroll;
    *w = 44;
    *h = 44;
    *x = LIST_X + LIST_W - 60;
    *y = rowY + (roomH[i] - *h) / 2;
}

bool roomActionAt(int x, int y, int* idx) {
    int i = roomIndexAt(y);
    if (i < 0 || roomAction(i) == RoomAction::None) return false;
    int ax, ay, aw, ah;
    roomActionBox(i, &ax, &ay, &aw, &ah);
    if (x < ax || y < ay || x >= ax + aw || y >= ay + ah) return false;
    *idx = i;
    return true;
}

// Draw one line, clipped to the given box and to whatever clip is already set.
// The outer clip is put back, so a scrolling list stays below the search field.
void drawClipped(M5Canvas& c, const char* s, int x, int y, int w, int h) {
    int32_t cx, cy, cw, ch;
    c.getClipRect(&cx, &cy, &cw, &ch);
    int x1 = x > cx ? x : cx;
    int y1 = y > cy ? y : cy;
    int x2 = x + w;
    int y2 = y + h;
    int ex = cx + cw;
    int ey = cy + ch;
    if (x2 > ex) x2 = ex;
    if (y2 > ey) y2 = ey;
    if (x2 > x1 && y2 > y1) {
        c.setClipRect(x1, y1, x2 - x1, y2 - y1);
        c.drawString(s, x, y);
    }
    c.setClipRect(cx, cy, cw, ch);
}

void text(M5Canvas& c, const char* s, int x, int y, int maxW, uint16_t col) {
    if (!s) s = "";
    c.setTextColor(col);
    int h = c.fontHeight();
    if (h < 1) h = 16;
    drawClipped(c, s, x, y, maxW, h + 6);
}

int homeNameRight = 88;

void drawHomeHeader(M5Canvas& c, const char* label, bool muted) {
    bool onIcon = pressedAt(HOME.x, HOME.y, HOME.w, HOME.h);
    bool onName = homeNameRight > 104 && pressedAt(104, HOME.y, homeNameRight - 104, HOME.h);
    if (onIcon || onName) c.fillRoundRect(HOME.x, HOME.y, HOME.w, HOME.h, 16, COL_ACCENT);
    uint16_t body = (onIcon || onName) ? COL_BG : (muted ? COL_MUTED : COL_TEXT);
    uint16_t hole = (onIcon || onName) ? COL_ACCENT : COL_BG;
    drawSpeakerGlyph(c, HOME.x + 20, HOME.y + 14, body, hole);
    c.setFont(&fontUi);
    c.setTextDatum(TL_DATUM);
    int y = HOME.y + (HOME.h - c.fontHeight()) / 2;
    text(c, label, 104, y, BROWSE.x - 120, muted ? COL_MUTED : COL_TEXT);
    int right = 104 + c.textWidth(label ? label : "");
    homeNameRight = right < BROWSE.x - 16 ? right : BROWSE.x - 16;
}

bool homeHit(int x, int y) {
    if (HOME.contains(x, y)) return true;
    return y >= HOME.y && y < HOME.y + HOME.h && x >= 104 && x < homeNameRight;
}

int utf8Len(const char* s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80 || !s[1]) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return s[2] ? 3 : 1;
    if ((c & 0xF8) == 0xF0) return (s[2] && s[3]) ? 4 : 1;
    return 1;
}

int drawWrapped(M5Canvas& c, const char* s, int x, int y, int maxW, int maxLines, uint16_t col) {
    if (!s || !s[0] || maxLines < 1 || maxW < 8) return 0;
    c.setTextColor(col);
    int lineH = c.fontHeight() + 4;
    int drawn = 0;
    const char* p = s;
    while (*p && drawn < maxLines) {
        while (*p == ' ') p++;
        if (!*p) break;
        int take = c.textLength(p, maxW);
        if (take <= 0) take = utf8Len(p);
        if (drawn + 1 < maxLines && p[take]) {
            int space = -1;
            for (int i = 0; i < take; i++)
                if (p[i] == ' ') space = i;
            if (space > 0) take = space;
        }
        char buf[140];
        int n = take < (int)sizeof(buf) ? take : (int)sizeof(buf) - 1;
        memcpy(buf, p, n);
        buf[n] = 0;
        drawClipped(c, buf, x, y, maxW, lineH);
        y += lineH;
        drawn++;
        p += take;
    }
    return drawn * lineH;
}

uint16_t sourceColor(M5Canvas& c, const char* name) {
    if (!strcmp(name, "Spotify")) return c.color565(29, 185, 84);
    if (!strcmp(name, "SoundCloud")) return c.color565(255, 85, 0);
    if (!strcmp(name, "Radio") || !strcmp(name, "Sonos Radio")) return c.color565(70, 150, 255);
    if (!strcmp(name, "TV")) return c.color565(180, 90, 255);
    if (!strcmp(name, "Line-in")) return c.color565(190, 190, 190);
    if (!strcmp(name, "AirPlay")) return c.color565(170, 170, 180);
    if (!strcmp(name, "Library")) return c.color565(80, 170, 130);
    if (!strcmp(name, "YouTube")) return c.color565(255, 48, 48);
    return COL_ACCENT;
}

void drawSource(M5Canvas& c, int x, int y, const char* name) {
    uint16_t col = sourceColor(c, name);
    int cx = x + 20;
    int cy = y + 20;
    uint16_t ink = COL_BG;
    c.fillCircle(cx, cy, 20, col);
    if (!strcmp(name, "Spotify")) {
        c.fillRoundRect(cx - 10, cy - 10, 20, 4, 2, ink);
        c.fillRoundRect(cx - 8, cy - 2, 18, 4, 2, ink);
        c.fillRoundRect(cx - 6, cy + 6, 16, 4, 2, ink);
    } else if (!strcmp(name, "SoundCloud")) {
        c.fillCircle(cx - 6, cy + 2, 6, ink);
        c.fillCircle(cx + 4, cy + 1, 8, ink);
        c.fillCircle(cx - 1, cy - 5, 5, ink);
        c.fillRoundRect(cx - 12, cy + 2, 24, 8, 2, ink);
    } else if (!strcmp(name, "Radio") || !strcmp(name, "Sonos Radio")) {
        c.fillCircle(cx, cy, 4, ink);
        c.drawCircle(cx, cy, 8, ink);
        c.drawCircle(cx, cy, 13, ink);
    } else if (!strcmp(name, "TV")) {
        c.fillRoundRect(cx - 12, cy - 8, 24, 16, 2, ink);
    } else if (!strcmp(name, "YouTube")) {
        c.fillTriangle(cx - 6, cy - 9, cx - 6, cy + 9, cx + 10, cy, ink);
    } else if (!strcmp(name, "AirPlay")) {
        c.fillTriangle(cx, cy - 10, cx - 10, cy + 8, cx + 10, cy + 8, ink);
    } else {
        c.fillCircle(cx, cy, 6, ink);
    }
    c.setFont(&fontBody);
    text(c, name, x + 48, y + 4, 360, COL_TEXT);
}

int clockSeconds(const char* t) {
    int a = 0, b = 0, c = 0;
    int n = sscanf(t ? t : "", "%d:%d:%d", &a, &b, &c);
    if (n == 3) return a * 3600 + b * 60 + c;
    if (n == 2) return a * 60 + b;
    return 0;
}

void shortClock(const char* t, char* out, size_t n) {
    int a = 0, b = 0, c = 0;
    if (sscanf(t ? t : "", "%d:%d:%d", &a, &b, &c) == 3) {
        if (a) snprintf(out, n, "%d:%02d:%02d", a, b, c);
        else snprintf(out, n, "%d:%02d", b, c);
    } else {
        strlcpy(out, (t && t[0]) ? t : "0:00", n);
    }
}

int displayVolume() {
    if (drag == Drag::Volume || (freezeVol && millis() < volFreezeUntil)) return localVol;
    if (view.volume < 0) return 0;
    return view.volume;
}

void sendVolume(int vol, bool force) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    localVol = vol;
    freezeVol = true;
    volFreezeUntil = millis() + 2500;
    if (!force && millis() - lastVolSend < 120) return;
    lastVolSend = millis();
    sonos::command(sonos::Cmd::Volume, vol, nullptr);
}

void drawTransportIcon(M5Canvas& c, const Box& b, char kind, uint16_t col) {
    int cx = b.x + b.w / 2;
    int cy = b.y + b.h / 2;
    if (kind == 'p') {
        c.fillTriangle(cx - 10, cy - 20, cx - 10, cy + 20, cx + 22, cy, col);
    } else if (kind == 's') {
        c.fillRoundRect(cx - 20, cy - 20, 12, 40, 3, col);
        c.fillRoundRect(cx + 8, cy - 20, 12, 40, 3, col);
    } else if (kind == '<') {
        c.fillRoundRect(cx - 22, cy - 16, 8, 32, 2, col);
        c.fillTriangle(cx + 20, cy - 16, cx + 20, cy + 16, cx - 10, cy, col);
    } else if (kind == '>') {
        c.fillRoundRect(cx + 14, cy - 16, 8, 32, 2, col);
        c.fillTriangle(cx - 20, cy - 16, cx - 20, cy + 16, cx + 10, cy, col);
    }
}

void drawSpotifyIcon(M5Canvas& c, int x, int y, uint16_t ink, uint16_t hole);
void drawYouTubeIcon(M5Canvas& c, int x, int y, uint16_t ink, uint16_t hole);

void centerLine(M5Canvas& c, const char* s, int y, uint16_t col) {
    c.setTextColor(col);
    c.setTextDatum(MC_DATUM);
    c.drawString(s ? s : "", c.width() / 2, y);
    c.setTextDatum(TL_DATUM);
}

// The header speaker, drawn large, with a play mark in the driver.
void drawSplashSpeaker(M5Canvas& c, int x, int y) {
    constexpr int s = 4;
    c.fillRoundRect(x, y, 34 * s, 44 * s, 4 * s, COL_TEXT);
    int cx = x + 17 * s;
    int cy = y + 16 * s;
    c.fillCircle(cx, cy, 10 * s, COL_BG);
    c.fillTriangle(cx - 12, cy - 16, cx - 12, cy + 16, cx + 18, cy, COL_ACCENT);
    c.fillRoundRect(x + 11 * s, y + 32 * s, 12 * s, 5 * s, 2 * s, COL_BG);
}

void drawWifi(M5Canvas& c) {
    c.fillSprite(COL_BG);
    int mid = c.width() / 2;
    int speakerX = mid - (34 * 4) / 2;
    drawSplashSpeaker(c, speakerX, 88);

    c.setFont(&fontTitle);
    centerLine(c, "Sonos Control", 318, COL_TEXT);
    c.setFont(&fontBody);
    centerLine(c, "Rooms, Spotify and YouTube", 372, COL_MUTED);

    int markY = 420;
    int row = 34 + 48 + 26 + 48 + 26;
    int x = mid - row / 2;
    drawSpeakerGlyph(c, x, markY, COL_TEXT, COL_BG);
    x += 34 + 48;
    drawSpotifyIcon(c, x, markY + 9, COL_TEXT, COL_BG);
    x += 26 + 48;
    drawYouTubeIcon(c, x, markY + 9, COL_TEXT, COL_BG);

    c.setFont(&fontBody);
    int statusY = 508;
    if (net::state() == net::State::Connecting) {
        char line[80];
        snprintf(line, sizeof(line), "Joining %s", net::ssid());
        centerLine(c, line, statusY, COL_TEXT);
    } else if (net::setupActive()) {
        centerLine(c, "On your phone, join", statusY, COL_TEXT);
        c.setFont(&fontUi);
        centerLine(c, net::setupName()[0] ? net::setupName() : "TabSonos", statusY + 42, COL_ACCENT);
        c.setFont(&fontBody);
        centerLine(c, "Then open   http://192.168.4.1", statusY + 84, COL_TEXT);
        centerLine(c, "The controller needs a 2.4 GHz network.", statusY + 126, COL_MUTED);
        if (net::setupClosing()) centerLine(c, "Connected. Closing the setup network...", statusY + 168, COL_TEXT);
    } else {
        centerLine(c, "Waiting for Wi-Fi", statusY, COL_MUTED);
    }
    drawBattery(c, BATT_X, BATT_Y);
    drawHeaderButton(c, SET, 3, false);
}

const char* kindName(svc::Kind kind) {
    if (kind == svc::Kind::Album) return "Album";
    if (kind == svc::Kind::Playlist) return "Playlist";
    if (kind == svc::Kind::Artist) return "Artist";
    if (kind == svc::Kind::Radio) return "Radio";
    return "Song";
}

bool canQueue(svc::Kind kind) {
    return kind == svc::Kind::Track || kind == svc::Kind::Album || kind == svc::Kind::Playlist;
}

void drawMark(M5Canvas& c, int x, int y, int w, int h, bool play, bool hot) {
    c.fillRoundRect(x, y, w, h, 12, hot ? COL_ACCENT : COL_CARD2);
    uint16_t ink = hot ? COL_BG : COL_TEXT;
    c.setTextDatum(MC_DATUM);
    c.setTextColor(ink);
    if (play) {
        int cx = x + w / 2 + 2;
        int cy = y + h / 2;
        int s = h / 5;
        if (s < 8) s = 8;
        c.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + s + 2, cy, ink);
    } else {
        c.setFont(&fontBrowse);
        c.drawString("+", x + w / 2, y + h / 2);
    }
    c.setTextDatum(TL_DATUM);
}

void drawStarShape(M5Canvas& c, int cx, int cy, int r, uint16_t col, bool filled) {
    int px[10], py[10];
    for (int k = 0; k < 10; k++) {
        float a = (-90.0f + 36.0f * k) * 0.0174533f;
        float rad = (k % 2 == 0) ? (float)r : r * 0.42f;
        px[k] = cx + (int)lroundf(cosf(a) * rad);
        py[k] = cy + (int)lroundf(sinf(a) * rad);
    }
    for (int k = 0; k < 10; k++) {
        int n = (k + 1) % 10;
        if (filled) c.fillTriangle(cx, cy, px[k], py[k], px[n], py[n], col);
        else c.drawLine(px[k], py[k], px[n], py[n], col);
    }
}

void drawStarMark(M5Canvas& c, int x, int y, int w, int h, bool on, bool hot) {
    c.fillRoundRect(x, y, w, h, 12, hot ? COL_ACCENT : COL_CARD2);
    uint16_t ink = hot ? COL_BG : (on ? COL_ACCENT : COL_TEXT);
    int r = (h < w ? h : w) / 2 - 8;
    drawStarShape(c, x + w / 2, y + h / 2 + 1, r, ink, on);
}

bool isStarred(const char* id) {
    for (int i = 0; i < tube.starCount; i++)
        if (!strcmp(tube.stars[i].id, id)) return true;
    return false;
}

// The speaker is playing something the Tab5 resolved from YouTube.
bool youtubeNow() { return !strcmp(view.source, "YouTube") && tube.playingId[0]; }

bool ytGrid = false;

constexpr int BR_X = 16;
constexpr int BR_W = 1248;
constexpr int BR_TOP = 116;
constexpr int BR_SAFE = 580;
constexpr int BR_BOTTOM = 712;
constexpr int BR_BTN_H = 52;
constexpr int BR_SEARCH_Y = BR_TOP + BR_BTN_H + 17;
constexpr int BR_SEARCH_H = 84;
constexpr int BR_LIST = BR_SEARCH_Y + BR_SEARCH_H + 16;
constexpr int BR_H = 118;
constexpr int BR_CLEAR = 72;
constexpr int KEY_H = 64;
constexpr int KEY_ROW = 72;
constexpr int KEY_Y = BR_SAFE - 4 * KEY_ROW;

int searchRowH() { return (BR_BOTTOM - KEY_Y) / 5; }
int searchKeyH() { return searchRowH() - 8; }

int clearX() { return BR_X + BR_W - 16 - BR_CLEAR; }
int clearY() { return BR_SEARCH_Y + (BR_SEARCH_H - BR_CLEAR) / 2; }
bool clearHit(int x, int y) {
    return x >= clearX() && x < clearX() + BR_CLEAR && y >= clearY() && y < clearY() + BR_CLEAR;
}

constexpr int CHIP_W = 248;

int youtubeChipX() { return BR_X + CHIP_W + 16; }
int listsX() { return BR_X + 2 * (CHIP_W + 16); }

constexpr int GRID_COLS = 3;
constexpr int GRID_GAP = 12;
int gridCardW() { return (BR_W - GRID_GAP * (GRID_COLS - 1)) / GRID_COLS; }
int gridCardH() { return gridCardW() * 9 / 16; }
constexpr int ICON_W = BR_BTN_H;
int gridChipX() { return listsX() + ICON_W + 16; }
bool tubeGrid() { return youtube && ytGrid && !ytStars; }
int tubePitch() { return tubeGrid() ? gridCardH() + GRID_GAP : BR_H; }
int tubeRows() {
    if (ytStars) return tube.starCount;
    if (ytGrid) {
        int n = (tube.count + GRID_COLS - 1) / GRID_COLS;
        if (tube.more) n++;
        return n;
    }
    return tube.count + (tube.more ? 1 : 0);
}

bool trackDetail() { return !youtube && browse.canBack && browse.pageKind == svc::Kind::Track; }

int browseListTop() {
    if (youtube && ytStars) return BR_LIST;
    if (trackDetail()) return BR_LIST + 112;
    bool show = youtube ? (tube.status[0] && tube.count > 0) : (browse.status[0] && browse.count > 0);
    if (!keyboard && show) return BR_LIST + 56;
    return BR_LIST;
}

void drawSpotifyIcon(M5Canvas& c, int x, int y, uint16_t ink, uint16_t hole) {
    c.fillCircle(x + 13, y + 13, 13, ink);
    c.fillRoundRect(x + 6, y + 7, 15, 3, 1, hole);
    c.fillRoundRect(x + 7, y + 12, 13, 3, 1, hole);
    c.fillRoundRect(x + 8, y + 17, 10, 3, 1, hole);
}

void drawYouTubeIcon(M5Canvas& c, int x, int y, uint16_t ink, uint16_t hole) {
    c.fillRoundRect(x, y, 26, 26, 6, ink);
    c.fillTriangle(x + 9, y + 7, x + 9, y + 19, x + 19, y + 13, hole);
}

void drawGridIcon(M5Canvas& c, int x, int y, uint16_t ink) {
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 3; col++) c.fillRoundRect(x + col * 9, y + 3 + row * 12, 7, 9, 2, ink);
}

void drawPlaylistsIcon(M5Canvas& c, int x, int y, uint16_t ink) {
    for (int i = 0; i < 3; i++) {
        int yy = y + 2 + i * 8;
        c.fillRoundRect(x, yy, 6, 6, 2, ink);
        c.fillRoundRect(x + 9, yy + 1, 17, 4, 2, ink);
    }
}

void iconChip(M5Canvas& c, int x, int kind, bool on) {
    bool hot = pressedAt(x, BR_TOP, ICON_W, BR_BTN_H);
    bool lit = on || hot;
    c.fillRoundRect(x, BR_TOP, ICON_W, BR_BTN_H, 12, lit ? COL_ACCENT : COL_CARD);
    uint16_t ink = lit ? COL_BG : COL_TEXT;
    int ix = x + (ICON_W - 26) / 2;
    int iy = BR_TOP + (BR_BTN_H - 26) / 2;
    if (kind == 0) drawStarShape(c, x + ICON_W / 2, BR_TOP + BR_BTN_H / 2 + 1, 12, ink, true);
    else if (kind == 1) drawGridIcon(c, ix, iy, ink);
    else drawPlaylistsIcon(c, ix, iy, ink);
}

void sourceChip(M5Canvas& c, int x, const char* label, bool on) {
    bool hot = pressedAt(x, BR_TOP, CHIP_W, BR_BTN_H);
    bool lit = on || hot;
    c.fillRoundRect(x, BR_TOP, CHIP_W, BR_BTN_H, 12, lit ? COL_ACCENT : COL_CARD);
    uint16_t ink = lit ? COL_BG : COL_TEXT;
    uint16_t hole = lit ? COL_ACCENT : COL_CARD;
    bool marked = !strcmp(label, "Spotify") || !strcmp(label, "YouTube");
    c.setFont(&fontBrowse);
    c.setTextColor(ink);
    if (marked) {
        int labelW = c.textWidth(label);
        int block = 26 + 10 + labelW;
        int ix = x + (CHIP_W - block) / 2;
        int iy = BR_TOP + (BR_BTN_H - 26) / 2;
        if (label[0] == 'S') drawSpotifyIcon(c, ix, iy, ink, hole);
        else drawYouTubeIcon(c, ix, iy, ink, hole);
        c.setTextDatum(ML_DATUM);
        c.setClipRect(x + 8, BR_TOP, CHIP_W - 16, BR_BTN_H);
        c.drawString(label, ix + 36, BR_TOP + BR_BTN_H / 2);
        c.clearClipRect();
    } else {
        c.setTextDatum(MC_DATUM);
        c.drawString(label, x + CHIP_W / 2, BR_TOP + BR_BTN_H / 2);
    }
    c.setTextDatum(TL_DATUM);
}

char settingsKeyAt(int x, int y) {
    constexpr int rowH = 56;
    constexpr int keyH = 50;
    const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int row = 0; row < 4; row++) {
        int extra = row == 3 ? 1 : 0;
        int cols = (int)strlen(rows[row]) + extra;
        int gap = 8;
        int w = (BR_W - (cols - 1) * gap) / cols;
        int by = KEY_Y + row * rowH;
        if (y < by || y >= by + keyH) continue;
        for (int col = 0; col < (int)strlen(rows[row]); col++) {
            int bx = BR_X + col * (w + gap);
            if (x >= bx && x < bx + w) return rows[row][col];
        }
        if (extra) {
            int bx = BR_X + (int)strlen(rows[row]) * (w + gap);
            if (x >= bx && x < bx + w) return '\b';
        }
        return 0;
    }
    int by = KEY_Y + 4 * rowH;
    if (y < by || y >= by + keyH) return 0;
    if (x < BR_X + 160) return '\t';
    if (x >= BR_X + 860) return '\n';
    return ' ';
}

char keyAt(int x, int y) {
    if (!keyboard || x < BR_X || x >= BR_X + BR_W || y < KEY_Y) return 0;
    if (panel == Panel::Settings) return y < BR_SAFE ? settingsKeyAt(x, y) : 0;
    if (y >= BR_BOTTOM) return 0;
    int rowH = searchRowH();
    int keyH = searchKeyH();
    const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int row = 0; row < 4; row++) {
        int extra = row == 3 ? 1 : 0;
        int cols = (int)strlen(rows[row]) + extra;
        int gap = 8;
        int w = (BR_W - (cols - 1) * gap) / cols;
        int by = KEY_Y + row * rowH;
        if (y < by || y >= by + keyH) continue;
        for (int col = 0; col < (int)strlen(rows[row]); col++) {
            int bx = BR_X + col * (w + gap);
            if (x >= bx && x < bx + w) return rows[row][col];
        }
        if (extra) {
            int bx = BR_X + (int)strlen(rows[row]) * (w + gap);
            if (x >= bx && x < bx + w) return '\b';
        }
        return 0;
    }
    int by = KEY_Y + 4 * rowH;
    if (y < by || y >= by + keyH) return 0;
    if (x < BR_X + 860) return ' ';
    return '\n';
}

void drawKey(M5Canvas& c, int x, int y, int w, const char* label, bool accent, int h = KEY_H) {
    bool on = accent || pressedAt(x, y, w, h);
    c.fillRoundRect(x, y, w, h, 12, on ? COL_ACCENT : COL_CARD2);
    c.setFont(&fontBrowse);
    c.setTextColor(on ? COL_BG : COL_TEXT);
    c.setTextDatum(MC_DATUM);
    c.drawString(label, x + w / 2, y + h / 2);
    c.setTextDatum(TL_DATUM);
}

bool wifiShift = false;

void drawSettingsKeyboard(M5Canvas& c) {
    constexpr int rowH = 56;
    constexpr int keyH = 50;
    c.setClipRect(0, KEY_Y, c.width(), BR_SAFE - KEY_Y);
    const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int row = 0; row < 4; row++) {
        int extra = row == 3 ? 1 : 0;
        int cols = (int)strlen(rows[row]) + extra;
        int gap = 8;
        int w = (BR_W - (cols - 1) * gap) / cols;
        for (int col = 0; col < (int)strlen(rows[row]); col++) {
            char label[2] = {rows[row][col], 0};
            if (wifiShift && label[0] >= 'a' && label[0] <= 'z') label[0] = label[0] - 'a' + 'A';
            drawKey(c, BR_X + col * (w + gap), KEY_Y + row * rowH, w, label, false, keyH);
        }
        if (extra) {
            int col = (int)strlen(rows[row]);
            drawKey(c, BR_X + col * (w + gap), KEY_Y + row * rowH, w, "del", false, keyH);
        }
    }
    int by = KEY_Y + 4 * rowH;
    drawKey(c, BR_X, by, 150, wifiShift ? "Aa" : "aa", wifiShift, keyH);
    drawKey(c, BR_X + 166, by, 680, "space", false, keyH);
    drawKey(c, BR_X + 860, by, BR_W - 860, "Done", true, keyH);
    c.clearClipRect();
}

void drawKeyboard(M5Canvas& c) {
    if (panel == Panel::Settings) {
        drawSettingsKeyboard(c);
        return;
    }
    int rowH = searchRowH();
    int keyH = searchKeyH();
    c.setClipRect(0, KEY_Y, c.width(), BR_BOTTOM - KEY_Y);
    const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int row = 0; row < 4; row++) {
        int extra = row == 3 ? 1 : 0;
        int cols = (int)strlen(rows[row]) + extra;
        int gap = 8;
        int w = (BR_W - (cols - 1) * gap) / cols;
        for (int col = 0; col < (int)strlen(rows[row]); col++) {
            char label[2] = {rows[row][col], 0};
            drawKey(c, BR_X + col * (w + gap), KEY_Y + row * rowH, w, label, false, keyH);
        }
        if (extra) {
            int col = (int)strlen(rows[row]);
            drawKey(c, BR_X + col * (w + gap), KEY_Y + row * rowH, w, "del", false, keyH);
        }
    }
    int by = KEY_Y + 4 * rowH;
    drawKey(c, BR_X, by, 844, "space", false, keyH);
    drawKey(c, BR_X + 860, by, BR_W - 860, "Search", true, keyH);
    c.clearClipRect();
}

constexpr int THUMB_W = 160;
constexpr int THUMB_H = 90;
struct StarThumb {
    char id[16];
    M5Canvas* sprite;  // null when the picture could not be decoded
    bool used;
};
StarThumb starThumbs[yt::MAX_STARS];

StarThumb* thumbFor(const char* id) {
    for (StarThumb& t : starThumbs)
        if (t.used && !strcmp(t.id, id)) return &t;
    return nullptr;
}

bool tubeThumbWanted(const char* id) {
    if (!id || !id[0] || panel != Panel::Browse || !youtube) return false;
    if (ytStars) {
        for (int i = 0; i < tube.starCount; i++)
            if (!strcmp(tube.stars[i].id, id)) return true;
        return false;
    }
    for (int i = 0; i < tube.count; i++)
        if (!strcmp(tube.items[i].id, id)) return true;
    return false;
}

// Decodes a few thumbnails per frame for the YouTube list that is on screen.
void updateStarThumbs() {
    int wantW = tubeGrid() ? gridCardW() : THUMB_W;
    int wantH = tubeGrid() ? gridCardH() : THUMB_H;
    for (StarThumb& t : starThumbs) {
        if (!t.used) continue;
        bool sizeOk = !t.sprite || (t.sprite->width() == wantW && t.sprite->height() == wantH);
        if (tubeThumbWanted(t.id) && sizeOk) continue;
        if (t.sprite) {
            t.sprite->deleteSprite();
            delete t.sprite;
        }
        t = StarThumb{};
    }
    if (!jpg || panel != Panel::Browse || !youtube) return;
    int made = 0;
    int nItems = ytStars ? tube.starCount : tube.count;
    for (int i = 0; i < nItems; i++) {
        const char* id = ytStars ? tube.stars[i].id : tube.items[i].id;
        StarThumb* have = thumbFor(id);
        if (have && have->sprite) continue;
        StarThumb* slot = have;
        if (!slot) {
            for (StarThumb& t : starThumbs)
                if (!t.used) {
                    slot = &t;
                    break;
                }
        }
        if (!slot) return;
        size_t n = yt::copyThumb(id, jpg, JPG_CAP);
        slot->used = true;
        strlcpy(slot->id, id, sizeof(slot->id));
        if (slot->sprite) {
            slot->sprite->deleteSprite();
            delete slot->sprite;
            slot->sprite = nullptr;
        }
        if (!n) continue;
        M5Canvas* sp = new M5Canvas();
        sp->setColorDepth(16);
        sp->setPsram(true);
        sp->createSprite(wantW, wantH);
        if (sp->getBuffer()) {
            sp->fillSprite(COL_CARD2);
            auto fit = lgfx::v1::textdatum::middle_center;
            if (sp->drawJpg(jpg, n, 0, 0, wantW, wantH, 0, 0, 0.0f, 0.0f, fit)) slot->sprite = sp;
        }
        if (!slot->sprite) {
            sp->deleteSprite();
            delete sp;
        }
        if (++made >= 3) return;
    }
}

constexpr int SPOT_S = 90;
struct SpotThumb {
    char url[svc::IMAGE_CAP];
    M5Canvas* sprite;
    bool used;
};
SpotThumb spotThumbs[16];

SpotThumb* spotThumbFor(const char* url) {
    if (!url || !url[0]) return nullptr;
    for (SpotThumb& t : spotThumbs)
        if (t.used && !strcmp(t.url, url)) return &t;
    return nullptr;
}

void updateSpotifyThumbs() {
    auto wanted = [&](const char* url) {
        if (!url || !url[0] || panel != Panel::Browse || youtube) return false;
        for (int i = 0; i < browse.count; i++)
            if (!strcmp(browse.items[i].image, url)) return true;
        return false;
    };
    for (SpotThumb& t : spotThumbs) {
        if (!t.used) continue;
        if (wanted(t.url)) continue;
        if (t.sprite) {
            t.sprite->deleteSprite();
            delete t.sprite;
        }
        t = {};
    }
    if (!jpg || panel != Panel::Browse || youtube) return;
    int made = 0;
    for (int i = 0; i < browse.count; i++) {
        const char* url = browse.items[i].image;
        if (!url[0] || spotThumbFor(url)) continue;
        SpotThumb* slot = nullptr;
        for (SpotThumb& t : spotThumbs)
            if (!t.used) {
                slot = &t;
                break;
            }
        if (!slot) return;
        bool failed = false;
        size_t n = svc::copyArt(url, jpg, JPG_CAP, &failed);
        if (!n && !failed) continue;
        slot->used = true;
        strlcpy(slot->url, url, sizeof(slot->url));
        slot->sprite = nullptr;
        if (!failed && n) {
            M5Canvas* sp = new M5Canvas();
            sp->setColorDepth(16);
            sp->setPsram(true);
            sp->createSprite(SPOT_S, SPOT_S);
            if (sp->getBuffer()) {
                sp->fillSprite(COL_CARD2);
                auto fit = lgfx::v1::textdatum::middle_center;
                bool jpeg = jpg[0] == (char)0xFF && jpg[1] == (char)0xD8;
                bool png = n > 8 && jpg[0] == (char)0x89 && jpg[1] == 'P' && jpg[2] == 'N' && jpg[3] == 'G';
                bool drew = false;
                if (jpeg) drew = sp->drawJpg(jpg, n, 0, 0, SPOT_S, SPOT_S, 0, 0, 0.0f, 0.0f, fit);
                else if (png) drew = sp->drawPng(jpg, n, 0, 0, SPOT_S, SPOT_S, 0, 0, 0.0f, 0.0f, fit);
                if (drew) slot->sprite = sp;
            }
            if (!slot->sprite) {
                sp->deleteSprite();
                delete sp;
            }
        }
        if (++made >= 8) return;
    }
}

void drawStarList(M5Canvas& c) {
    int listTop = BR_LIST;
    int listBottom = BR_BOTTOM;
    int rows = tube.starCount;
    int viewH = listBottom - listTop;
    int maxS = listMax(rows, viewH, BR_H);
    if (browseScroll > maxS) browseScroll = maxS;
    if (browseScroll < 0) browseScroll = 0;
    c.setClipRect(BR_X, listTop, BR_W, viewH);
    if (rows == 0) {
        c.setFont(&fontBrowse);
        text(c, "No starred videos yet. Tap the star on a result or on the playing video.", BR_X, listTop + 8, BR_W,
             COL_MUTED);
    }
    for (int i = 0; i < rows; i++) {
        int y = listTop + i * BR_H - browseScroll;
        if (y + BR_H < listTop || y > listBottom) continue;
        const int gutter = 176;
        c.fillRoundRect(BR_X, y + 4, BR_W - gutter, BR_H - 8, 14, COL_CARD);
        int tx = BR_X + 12;
        StarThumb* th = thumbFor(tube.stars[i].id);
        if (th && th->sprite) th->sprite->pushSprite(&c, tx, y + 14);
        else c.fillRoundRect(tx, y + 14, THUMB_W, THUMB_H, 8, COL_CARD2);
        int textX = tx + THUMB_W + 20;
        int textW = BR_W - gutter - (textX - BR_X) - 16;
        c.setFont(&fontBrowse);
        text(c, tube.stars[i].title, textX, y + 12, textW, COL_TEXT);
        c.setFont(&fontBrowseBody);
        text(c, tube.stars[i].channel, textX, y + 62, textW, COL_MUTED);
        int starX = BR_X + BR_W - 156;
        int markX = BR_X + BR_W - 80;
        bool starHot = pressedAt(starX, y + 24, 68, 70);
        bool playHot = pressedAt(markX, y + 24, 68, 70);
        drawStarMark(c, starX, y + 26, 68, 66, true, starHot);
        drawMark(c, markX, y + 26, 68, 66, true, playHot);
    }
    c.clearClipRect();
}

void appendMeta(char* out, size_t cap, const char* part) {
    if (!out || !part || !part[0] || cap < 2) return;
    size_t n = strlen(out);
    if (n) {
        if (n + 3 >= cap) return;
        out[n++] = ' ';
        out[n++] = ' ';
        out[n] = 0;
    }
    strlcpy(out + n, part, cap - n);
}

void drawTube(M5Canvas& c) {
    if (ytStars) {
        drawStarList(c);
        return;
    }
    int listTop = browseListTop();
    int listBottom = BR_BOTTOM;
    c.setFont(&fontBrowse);
    bool tubeWait = tube.loading && tube.count == 0;
    if (tube.status[0] && !tubeWait) text(c, tube.status, BR_X, BR_LIST, BR_W, COL_MUTED);
    int pitch = tubeGrid() ? tubePitch() : BR_H;
    int rows = tubeGrid() ? tubeRows() : tube.count + (tube.more ? 1 : 0);
    int viewH = listBottom - listTop;
    int maxS = listMax(rows, viewH, pitch);
    if (browseScroll > maxS) browseScroll = maxS;
    if (browseScroll < 0) browseScroll = 0;
    c.setClipRect(BR_X, listTop, BR_W, viewH);
    if (tube.loading && tube.count == 0) {
        text(c, "Searching YouTube...", BR_X, listTop + 8, BR_W, COL_MUTED);
    } else if (!tube.loading && tube.count == 0 && !tube.status[0]) {
        text(c, "Search YouTube, then tap Search", BR_X, listTop + 8, BR_W, COL_MUTED);
    }
    if (tubeGrid()) {
        int cardW = gridCardW();
        int cardH = gridCardH();
        int stride = cardH + GRID_GAP;
        for (int i = 0; i < tube.count; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;
            int x = BR_X + col * (cardW + GRID_GAP);
            int y = listTop + row * stride - browseScroll;
            if (y + cardH < listTop || y > listBottom) continue;
            StarThumb* th = thumbFor(tube.items[i].id);
            if (th && th->sprite) th->sprite->pushSprite(&c, x, y);
            else c.fillRoundRect(x, y, cardW, cardH, 12, COL_CARD2);
            constexpr int barH = 88;
            c.fillRect(x, y + cardH - barH, cardW, barH, COL_CARD);
            c.setFont(&fontBrowseBody);
            text(c, tube.items[i].title, x + 12, y + cardH - barH + 4, cardW - 24, COL_TEXT);
            char meta[80] = "";
            appendMeta(meta, sizeof(meta), tube.items[i].age);
            char viewLine[24] = "";
            if (tube.items[i].views[0]) snprintf(viewLine, sizeof(viewLine), "%s views", tube.items[i].views);
            appendMeta(meta, sizeof(meta), viewLine);
            if (meta[0]) {
                c.setFont(&fontUi);
                text(c, meta, x + 12, y + cardH - 40, cardW - 24, COL_MUTED);
            }
            int starX = x + cardW - 64;
            bool starHot = pressedAt(starX, y + 8, 56, 56);
            drawStarMark(c, starX, y + 8, 56, 56, isStarred(tube.items[i].id), starHot);
        }
        if (tube.more) {
            int row = (tube.count + GRID_COLS - 1) / GRID_COLS;
            int y = listTop + row * stride - browseScroll;
            if (y + cardH >= listTop && y <= listBottom) {
                c.setFont(&fontBrowse);
                text(c, "More", BR_X + 20, y + 12, BR_W, COL_ACCENT);
            }
        }
        c.clearClipRect();
        return;
    }
    for (int i = 0; i < tube.count; i++) {
        int y = listTop + i * BR_H - browseScroll;
        if (y + BR_H < listTop || y > listBottom) continue;
        int gutter = 176;
        c.fillRoundRect(BR_X, y + 4, BR_W - gutter, BR_H - 8, 14, COL_CARD);
        int tx = BR_X + 12;
        StarThumb* th = thumbFor(tube.items[i].id);
        if (th && th->sprite) th->sprite->pushSprite(&c, tx, y + 14);
        else c.fillRoundRect(tx, y + 14, THUMB_W, THUMB_H, 8, COL_CARD2);
        int textX = tx + THUMB_W + 16;
        int textW = BR_W - gutter - (textX - BR_X) - 12;
        if (textW < 40) textW = 40;
        c.setFont(&fontBrowse);
        text(c, tube.items[i].title, textX, y + 8, textW, COL_TEXT);
        char clock[16] = "";
        int seconds = tube.items[i].seconds;
        if (seconds > 0) {
            if (seconds >= 3600)
                snprintf(clock, sizeof(clock), "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
            else
                snprintf(clock, sizeof(clock), "%d:%02d", seconds / 60, seconds % 60);
        }
        char line[140] = "";
        appendMeta(line, sizeof(line), tube.items[i].channel);
        appendMeta(line, sizeof(line), clock);
        appendMeta(line, sizeof(line), tube.items[i].age);
        char viewLine[24] = "";
        if (tube.items[i].views[0]) snprintf(viewLine, sizeof(viewLine), "%s views", tube.items[i].views);
        appendMeta(line, sizeof(line), viewLine);
        c.setFont(&fontBrowseBody);
        text(c, line, textX, y + 62, textW, COL_MUTED);
        int markX = BR_X + BR_W - 80;
        bool playHot = pressedAt(markX, y + 24, 68, 70);
        drawMark(c, markX, y + 26, 68, 66, true, playHot);
        int starX = BR_X + BR_W - 156;
        bool starHot = pressedAt(starX, y + 24, 68, 70);
        drawStarMark(c, starX, y + 26, 68, 66, isStarred(tube.items[i].id), starHot);
    }
    if (tube.more) {
        int y = listTop + tube.count * BR_H - browseScroll;
        if (y + BR_H >= listTop && y <= listBottom) {
            c.setFont(&fontBrowse);
            text(c, "More", BR_X + 20, y + 32, BR_W, COL_ACCENT);
        }
    }
    c.clearClipRect();
}

void drawBrowse(M5Canvas& c) {
    c.fillRect(0, 109, c.width(), BR_BOTTOM - 109, COL_BG);
    c.setFont(&fontBrowse);
    bool queuePage = !youtube && browse.pagePlayable && canQueue(browse.pageKind);
    int playX = BR_X + BR_W - 16 - 64;
    if (queuePage) playX -= 76;
    if (youtube || !browse.canBack) {
        sourceChip(c, BR_X, "Spotify", !youtube);
        sourceChip(c, youtubeChipX(), "YouTube", youtube && !ytStars);
        if (youtube) iconChip(c, listsX(), 0, ytStars);
        if (youtube && !ytStars) iconChip(c, gridChipX(), 1, ytGrid);
        if (!youtube) iconChip(c, listsX(), 2, !browse.canBack && !strcmp(browse.heading, "My playlists"));
    } else if (browse.canBack) {
        bool hot = pressedAt(BR_X, BR_TOP, 160, BR_BTN_H);
        c.fillRoundRect(BR_X, BR_TOP, 160, BR_BTN_H, 12, hot ? COL_ACCENT : COL_CARD);
        c.setTextDatum(MC_DATUM);
        c.setTextColor(hot ? COL_BG : COL_TEXT);
        c.drawString("Back", BR_X + 80, BR_TOP + BR_BTN_H / 2);
        c.setTextDatum(TL_DATUM);
        if (!trackDetail()) {
            int titleW = (browse.pagePlayable ? playX : BR_X + BR_W) - (BR_X + 176) - 12;
            if (titleW < 40) titleW = 40;
            text(c, browse.heading[0] ? browse.heading : "Spotify", BR_X + 176, BR_TOP, titleW, COL_TEXT);
        }
    }
    if (!youtube && browse.pagePlayable) {
        bool hot = pressedAt(playX, BR_TOP, 64, BR_BTN_H);
        drawMark(c, playX, BR_TOP, 64, BR_BTN_H, true, hot);
        if (queuePage) {
            int plusX = playX + 76;
            bool plusHot = pressedAt(plusX, BR_TOP, 64, BR_BTN_H);
            drawMark(c, plusX, BR_TOP, 64, BR_BTN_H, false, plusHot);
        }
    }

    c.fillRoundRect(BR_X, BR_SEARCH_Y, BR_W, BR_SEARCH_H, 14, COL_CARD2);
    c.setFont(&fontTitle);
    c.setTextColor(query[0] ? COL_TEXT : COL_MUTED);
    int textY = BR_SEARCH_Y + (BR_SEARCH_H - c.fontHeight()) / 2;
    c.setClipRect(BR_X + 24, BR_SEARCH_Y + 4, clearX() - BR_X - 48, BR_SEARCH_H - 8);
    c.drawString(query[0] ? query : (youtube ? "Search YouTube" : "Search Spotify"), BR_X + 24, textY);
    c.clearClipRect();
    bool clearing = pressedAt(clearX(), clearY(), BR_CLEAR, BR_CLEAR);
    c.fillRoundRect(clearX(), clearY(), BR_CLEAR, BR_CLEAR, 12, clearing ? COL_ACCENT : COL_CARD);
    c.setTextDatum(MC_DATUM);
    c.setTextColor(clearing ? COL_BG : COL_TEXT);
    c.drawString("x", clearX() + BR_CLEAR / 2, clearY() + BR_CLEAR / 2);
    c.setTextDatum(TL_DATUM);

    if (keyboard) {
        drawKeyboard(c);
        return;
    }
    if (youtube) {
        drawTube(c);
        return;
    }

    int listTop = browseListTop();
    int listBottom = BR_BOTTOM;
    c.setFont(&fontBrowse);
    if (trackDetail()) {
        c.setFont(&fontTitle);
        text(c, browse.heading[0] ? browse.heading : "Song", BR_X + 8, BR_LIST, BR_W - 16, COL_TEXT);
        c.setFont(&fontBrowse);
        text(c, browse.status, BR_X + 8, BR_LIST + 64, BR_W - 16, COL_MUTED);
    } else if (browse.status[0] && !(browse.loading && browse.count == 0)) {
        text(c, browse.status, BR_X, BR_LIST, BR_W, COL_MUTED);
    }
    int rows = browse.count + (browse.more || browse.offset > 0 ? 1 : 0);
    int viewH = listBottom - listTop;
    int maxS = listMax(rows, viewH, BR_H);
    if (browseScroll > maxS) browseScroll = maxS;
    if (browseScroll < 0) browseScroll = 0;
    c.setClipRect(BR_X, listTop, BR_W, viewH);
    if (browse.loading && browse.count == 0) {
        text(c, "Loading Spotify...", BR_X, listTop + 8, BR_W, COL_MUTED);
    } else if (!browse.canBack && browse.count == 0 && !browse.status[0] && !browse.query[0]) {
        text(c, "Search for an artist, album, or song.", BR_X, listTop + 8, BR_W, COL_MUTED);
    }
    for (int i = 0; i < browse.count; i++) {
        int y = listTop + i * BR_H - browseScroll;
        if (y + BR_H < listTop || y > listBottom) continue;
        bool queued = canQueue(browse.items[i].kind);
        int gutter = queued ? 176 : 96;
        c.fillRoundRect(BR_X, y + 4, BR_W - gutter, BR_H - 8, 14, COL_CARD);
        bool hasArt = browse.items[i].image[0] != 0;
        int textX = BR_X + 20;
        int textW = BR_W - gutter - 36;
        if (hasArt) {
            int ax = BR_X + 12;
            int ay = y + (BR_H - SPOT_S) / 2;
            c.fillRoundRect(ax, ay, SPOT_S, SPOT_S, 8, COL_CARD2);
            SpotThumb* th = spotThumbFor(browse.items[i].image);
            if (th && th->sprite) th->sprite->pushSprite(&c, ax, ay);
            textX = ax + SPOT_S + 16;
            textW = BR_W - gutter - (textX - BR_X) - 16;
            if (textW < 40) textW = 40;
        }
        c.setFont(&fontBrowse);
        text(c, browse.items[i].title, textX, y + 8, textW, COL_TEXT);
        svc::Kind kind = browse.items[i].kind;
        bool iconKind = kind == svc::Kind::Track || kind == svc::Kind::Album || kind == svc::Kind::Artist;
        const char* sub = browse.items[i].subtitle;
        bool subReal = sub[0] && strcmp(sub, kindName(kind));
        int lineX = textX;
        int lineW = textW;
        if (iconKind) {
            drawKindIcon(c, kind, textX, y + 58, COL_MUTED, COL_CARD);
            lineX = textX + 36;
            lineW = textW - 36;
            if (lineW < 20) lineW = 20;
        }
        char line[100];
        if (iconKind) strlcpy(line, subReal ? sub : "", sizeof(line));
        else
            snprintf(line, sizeof(line), "%s%s%s", kindName(kind), sub[0] ? "  " : "", sub);
        c.setFont(&fontBrowseBody);
        if (line[0]) text(c, line, lineX, y + 62, lineW, COL_MUTED);
        int plusX = BR_X + BR_W - 80;
        int markX = queued ? plusX - 80 : plusX;
        bool playHot = pressedAt(markX, y + 24, 68, 70);
        drawMark(c, markX, y + 26, 68, 66, true, playHot);
        if (queued) {
            bool plusHot = pressing && !moved && x0 >= plusX && x0 < plusX + 68 && y0 >= y + 24 && y0 < y + 94;
            drawMark(c, plusX, y + 26, 68, 66, false, plusHot);
        }
    }
    if (browse.more || browse.offset > 0) {
        int y = listTop + browse.count * BR_H - browseScroll;
        if (y + BR_H >= listTop && y <= listBottom) {
            c.setFont(&fontBrowse);
            char page[40];
            snprintf(page, sizeof(page), "%s%s", browse.offset > 0 ? "Previous" : "",
                     browse.more ? (browse.offset > 0 ? "    More" : "More") : "");
            text(c, page, BR_X + 20, y + 32, BR_W, COL_ACCENT);
        }
    }
    c.clearClipRect();
}

constexpr int FIELD_X = 24;
constexpr int FIELD_W = 1232;
constexpr int SET_TAB_Y = 120;
constexpr int SET_TAB_H = 56;
constexpr int SET_TAB_W = 396;
constexpr int ACT_H = 80;

int settingsTab = 0;  // 0 Wi-Fi, 1 Screen, 2 Device

int setTabX(int i) { return FIELD_X + i * (SET_TAB_W + 16); }
int fieldH() { return keyboard ? 64 : 88; }
int ssidY() { return keyboard ? 124 : 236; }
int passY() { return keyboard ? 200 : 340; }
int actY() { return 456; }

const int SLEEP_MINS[] = {0, 1, 5, 15, 30};
const char* SLEEP_LABELS[] = {"Off", "1 min", "5 min", "15 min", "30 min"};
constexpr int SLEEP_N = 5;
constexpr int SLEEP_Y = 286;
constexpr int SLEEP_H = 88;
constexpr int CLICK_Y = 460;
constexpr int CLICK_H = 80;
constexpr int CLICK_W = 400;
constexpr int SLEEP_W = 232;

int sleepX(int i) { return FIELD_X + i * (SLEEP_W + 16); }

void drawField(M5Canvas& c, int y, int h, const char* label, const char* value, bool secret, bool on) {
    c.fillRoundRect(FIELD_X, y, FIELD_W, h, 14, on ? COL_CARD2 : COL_CARD);
    if (on) c.drawRoundRect(FIELD_X, y, FIELD_W, h, 14, COL_ACCENT);
    c.setFont(&fontBody);
    c.setTextColor(COL_MUTED);
    c.drawString(label, FIELD_X + 22, y + 10);
    c.setFont(&fontUi);
    c.setTextColor(value[0] ? COL_TEXT : COL_MUTED);
    const char* shown = value[0] ? value : "tap to type";
    char dots[28];
    if (secret) {
        size_t n = strlen(value);
        if (n > sizeof(dots) - 1) n = sizeof(dots) - 1;
        if (n) {
            memset(dots, '*', n);
            dots[n] = 0;
            shown = dots;
        }
    }
    c.drawString(shown, FIELD_X + 22, y + h / 2 + 4);
}

void drawAction(M5Canvas& c, int x, int y, int w, int h, const char* label) {
    bool hot = pressedAt(x, y, w, h);
    c.fillRoundRect(x, y, w, h, 14, hot ? COL_ACCENT : COL_CARD2);
    c.setFont(&fontUi);
    c.setTextColor(hot ? COL_BG : COL_TEXT);
    c.setTextDatum(MC_DATUM);
    c.drawString(label, x + w / 2, y + h / 2);
    c.setTextDatum(TL_DATUM);
}

void drawSettingsTabs(M5Canvas& c) {
    const char* labels[] = {"Wi-Fi", "Screen", "Device"};
    for (int i = 0; i < 3; i++) {
        int x = setTabX(i);
        bool on = settingsTab == i;
        bool hot = pressedAt(x, SET_TAB_Y, SET_TAB_W, SET_TAB_H);
        c.fillRoundRect(x, SET_TAB_Y, SET_TAB_W, SET_TAB_H, 14, (on || hot) ? COL_ACCENT : COL_CARD);
        c.setFont(&fontUi);
        c.setTextColor((on || hot) ? COL_BG : COL_TEXT);
        c.setTextDatum(MC_DATUM);
        c.drawString(labels[i], x + SET_TAB_W / 2, SET_TAB_Y + SET_TAB_H / 2);
        c.setTextDatum(TL_DATUM);
    }
}

void drawStat(M5Canvas& c, int x, int y, int w, int h, const char* label, const char* value) {
    c.fillRoundRect(x, y, w, h, 14, COL_CARD);
    c.setFont(&fontBody);
    c.setTextColor(COL_MUTED);
    c.drawString(label, x + 22, y + 12);
    c.setFont(&fontUi);
    c.setTextColor(COL_TEXT);
    text(c, value, x + 22, y + 44, w - 44, COL_TEXT);
}

void drawSettings(M5Canvas& c) {
    char line[160];
    if (!(keyboard && settingsTab == 0)) drawSettingsTabs(c);

    if (settingsTab == 0) {
        if (!keyboard) {
            if (net::setupActive()) {
                snprintf(line, sizeof(line), "Join %s from a phone, then open http://192.168.4.1",
                         net::setupName()[0] ? net::setupName() : "TabSonos");
            } else if (net::state() == net::State::Connecting) {
                snprintf(line, sizeof(line), "Joining %s", wifiSsid[0] ? wifiSsid : net::ssid());
            } else if (net::connected()) {
                snprintf(line, sizeof(line), "Connected    %s    %d dBm", net::localIp(), net::rssi());
            } else {
                snprintf(line, sizeof(line), "Not connected");
            }
            text(c, line, FIELD_X, 192, FIELD_W, COL_MUTED);
        }
        int h = fieldH();
        drawField(c, ssidY(), h, "Wi-Fi network", wifiSsid, false, wifiField == 1);
        drawField(c, passY(), h, "Password", wifiPass, true, wifiField == 2);
        if (keyboard) return;
        int y = actY();
        drawAction(c, 24, y, 392, ACT_H, "Join");
        drawAction(c, 432, y, 392, ACT_H, "Forget");
        drawAction(c, 840, y, 416, ACT_H, "Reconnect");
        return;
    }

    if (settingsTab == 1) {
        c.setFont(&fontBody);
        text(c, "The backlight turns off after the screen is left alone. Music keeps playing.", FIELD_X, 200,
             FIELD_W, COL_MUTED);
        text(c, "The next tap only wakes it.", FIELD_X, 240, FIELD_W, COL_MUTED);
        for (int i = 0; i < SLEEP_N; i++) {
            int x = sleepX(i);
            bool on = sleepMin == SLEEP_MINS[i];
            bool hot = pressedAt(x, SLEEP_Y, SLEEP_W, SLEEP_H);
            c.fillRoundRect(x, SLEEP_Y, SLEEP_W, SLEEP_H, 16, (on || hot) ? COL_ACCENT : COL_CARD);
            c.setFont(&fontUi);
            c.setTextColor((on || hot) ? COL_BG : COL_TEXT);
            c.setTextDatum(MC_DATUM);
            c.drawString(SLEEP_LABELS[i], x + SLEEP_W / 2, SLEEP_Y + SLEEP_H / 2);
            c.setTextDatum(TL_DATUM);
        }
        c.setFont(&fontBody);
        c.setTextColor(COL_MUTED);
        c.drawString("Button click", FIELD_X, CLICK_Y - 36);
        const char* clickLabels[] = {"Off", "Low", "High"};
        for (int i = 0; i < 3; i++) {
            int x = FIELD_X + i * (CLICK_W + 16);
            bool on = clickLevel == i;
            bool hot = pressedAt(x, CLICK_Y, CLICK_W, CLICK_H);
            c.fillRoundRect(x, CLICK_Y, CLICK_W, CLICK_H, 16, (on || hot) ? COL_ACCENT : COL_CARD);
            c.setFont(&fontUi);
            c.setTextColor((on || hot) ? COL_BG : COL_TEXT);
            c.setTextDatum(MC_DATUM);
            c.drawString(clickLabels[i], x + CLICK_W / 2, CLICK_Y + CLICK_H / 2);
            c.setTextDatum(TL_DATUM);
        }
        return;
    }

    const char* pack = !power.known ? "Reading" : (power.onUsb || power.pct < 0) ? "USB, no pack" : "Pack";
    char bat[64], up[32], mem[48], sig[32], rooms[24], screen[40];
    if (power.known && power.pct >= 0) {
        snprintf(bat, sizeof(bat), "%d%%   %d mV   %d mA%s", power.pct, power.mv, power.ma,
                 power.charging ? "   charging" : "");
    } else if (power.known) {
        snprintf(bat, sizeof(bat), "%s   %d mV", pack, power.mv);
    } else {
        strlcpy(bat, "Reading", sizeof(bat));
    }
    uint32_t sec = millis() / 1000;
    snprintf(up, sizeof(up), "%um %us", sec / 60, sec % 60);
    snprintf(mem, sizeof(mem), "%u KB free,  %u KB PSRAM", (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
    if (net::connected()) snprintf(sig, sizeof(sig), "%d dBm", net::rssi());
    else strlcpy(sig, "Not connected", sizeof(sig));
    snprintf(rooms, sizeof(rooms), "%d", view.roomCount);
    const char* clickName = clickLevel == 2 ? "High" : (clickLevel == 1 ? "Low" : "Off");
    snprintf(screen, sizeof(screen), "Rotation %d,  click %s", M5.Display.getRotation(), clickName);

    const char* labels[] = {"Network", "Address", "Signal", "Rooms", "Battery", "Up time", "Memory", "Screen"};
    const char* values[] = {net::ssid()[0] ? net::ssid() : "None", net::localIp()[0] ? net::localIp() : "None",
                            sig, rooms, bat, up, mem, screen};
    int cardW = 600;
    int cardH = 96;
    for (int i = 0; i < 8; i++) {
        int col = i / 4;
        int row = i % 4;
        drawStat(c, FIELD_X + col * (cardW + 32), 196 + row * (cardH + 16), cardW, cardH, labels[i], values[i]);
    }
}

void drawMain(M5Canvas& c) {
    c.fillSprite(COL_BG);
    c.setTextDatum(TL_DATUM);
    c.setFont(&fontUi);
    c.setTextColor(COL_TEXT);
    const char* room = !view.household ? "Searching for speakers"
                       : (view.groupName[0] ? view.groupName : "No room");
    drawHomeHeader(c, room, !view.household);
    drawBattery(c, BATT_X, BATT_Y);
    drawHeaderButton(c, BROWSE, 0, panel == Panel::Browse);
    drawHeaderButton(c, QUEUE, 1, panel == Panel::Queue);
    drawHeaderButton(c, FAV, 2, panel == Panel::Favs);
    drawHeaderButton(c, SET, 3, panel == Panel::Settings);

    c.drawFastHLine(0, 108, c.width(), COL_LINE);
    int roomsH = LIST_B - LIST_Y;
    if (panel == Panel::Settings) {
        drawSettings(c);
        if (keyboard) drawKeyboard(c);
        c.drawFastHLine(0, BAR_Y, c.width(), COL_LINE);
        goto controls;
    }
    if (panel == Panel::Browse) {
        drawBrowse(c);
        return;
    }
    c.drawFastVLine(380, 108, BAR_Y - 108, COL_LINE);
    c.drawFastHLine(380, BAR_Y, c.width() - 380, COL_LINE);
    c.setClipRect(LIST_X, LIST_Y, LIST_W, roomsH);
    for (int i = 0; i < view.roomCount;) {
        int n = groupRun(i);
        int y = LIST_Y + roomTop[i] - roomScroll;
        int cardH = n * roomH[i];
        bool visible = y + cardH >= LIST_Y && y <= LIST_B;
        bool group = n > 1;
        bool playing = false;
        bool selected = false;
        for (int k = 0; k < n; k++) {
            if (view.rooms[i + k].playing) playing = true;
            if (view.rooms[i + k].coordinator) selected = true;
        }
        if (visible) {
            c.fillRoundRect(LIST_X, y, LIST_W - 8, cardH, 14, COL_CARD);
            if (!group && selected)
                c.fillRoundRect(LIST_X, y, LIST_W - 8, cardH, 14, COL_CARD2);
            for (int k = 0; k < n; k++) {
                const sonos::RoomView& r = view.rooms[i + k];
                if (!(group && r.coordinator)) continue;
                int rowY = LIST_Y + roomTop[i + k] - roomScroll;
                c.fillRoundRect(LIST_X + 8, rowY + 4, LIST_W - 24, roomH[i + k] - 8, 10, COL_CARD2);
            }
            if (group) {
                int railX = LIST_X + 16;
                int railTop = y + 22;
                int railBot = y + cardH - 22;
                if (railBot > railTop) c.fillRoundRect(railX, railTop, 4, railBot - railTop, 2, COL_ACCENT);
            }
            for (int k = 0; k < n; k++) {
                const sonos::RoomView& r = view.rooms[i + k];
                int rowY = LIST_Y + roomTop[i + k] - roomScroll;
                int rh = roomH[i + k];
                if (group) c.fillCircle(LIST_X + 18, rowY + rh / 2, r.leader ? 6 : 4, COL_ACCENT);
                c.setFont(&fontUi);
                int textX = LIST_X + (group ? 36 : 18);
                int nameRight = LIST_X + LIST_W - (playing ? 108 : 68);
                int nameW = nameRight - textX;
                if (nameW < 40) nameW = 40;
                int textY = rowY + (rh - c.fontHeight()) / 2;
                text(c, r.name[0] ? r.name : "Room", textX, textY, nameW,
                     r.coordinator ? COL_ACCENT : COL_TEXT);
            }
            if (selected || pressedAt(LIST_X, y, LIST_W - 8, cardH))
                c.drawRoundRect(LIST_X, y, LIST_W - 8, cardH, 14, COL_ACCENT);
            if (playing) {
                int cx = LIST_X + LIST_W - 92;
                int cy = y + cardH / 2;
                c.fillCircle(cx, cy, 15, selected ? COL_ACCENT : COL_CARD2);
                uint16_t ink = selected ? COL_BG : COL_ACCENT;
                c.fillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 8, cy, ink);
            }
            for (int k = 0; k < n; k++) {
                RoomAction act = roomAction(i + k);
                if (act == RoomAction::None) continue;
                int ax, ay, aw, ah;
                roomActionBox(i + k, &ax, &ay, &aw, &ah);
                bool hot = pressedAt(ax, ay, aw, ah);
                int cx = ax + aw / 2;
                int cy = ay + ah / 2;
                bool add = act == RoomAction::Add;
                uint16_t ink = hot ? COL_TEXT : (add ? COL_ACCENT : COL_TEXT);
                if (!add) {
                    c.fillCircle(cx, cy, 18, hot ? COL_ACCENT : COL_CARD2);
                    ink = hot ? COL_BG : COL_TEXT;
                }
                c.fillRoundRect(cx - 9, cy - 2, 18, 5, 2, ink);
                if (add) c.fillRoundRect(cx - 2, cy - 9, 5, 18, 2, ink);
            }
        }
        i += n;
    }
    c.clearClipRect();
    if (view.household && view.roomCount == 0) {
        c.setFont(&fontBody);
        text(c, "No rooms", LIST_X, LIST_Y + 8, LIST_W, COL_MUTED);
    }

    if (panel == Panel::Closed) {
        c.fillRoundRect(ART_X, ART_Y, ART, ART, 16, COL_CARD);
        if (hasArt && art.getBuffer()) art.pushSprite(&c, ART_X, ART_Y);
        else {
            c.setFont(&fontBody);
            c.setTextColor(COL_MUTED);
            c.setTextDatum(MC_DATUM);
            c.drawString("No art", ART_X + ART / 2, ART_Y + ART / 2);
            c.setTextDatum(TL_DATUM);
        }

        int tx = ART_X + ART + 28;
        int tw = c.width() - tx - 24;
        bool haveTrack = view.title[0] != 0;
        if (haveTrack && youtubeNow()) {
            tw -= 62;
            bool hot = pressedAt(STAR_NOW.x, STAR_NOW.y, STAR_NOW.w, STAR_NOW.h);
            bool on = isStarred(tube.playingId);
            uint16_t ink = (hot || on) ? COL_ACCENT : COL_TEXT;
            drawStarShape(c, STAR_NOW.x + STAR_NOW.w / 2, STAR_NOW.y + STAR_NOW.h / 2, 19, ink, on || hot);
        }
        int y = ART_Y;
        c.setClipRect(tx, ART_Y, tw + 8, PLAY.y - 8 - ART_Y);
        if (haveTrack && view.source[0]) {
            drawSource(c, tx, y, view.source);
            y += 50;
        }
        c.setFont(&fontTitle);
        y += drawWrapped(c, haveTrack ? view.title : "Nothing playing", tx, y, tw, 2, COL_TEXT);
        y += 10;
        c.setFont(&fontBody);
        if (view.artist[0]) {
            y += drawWrapped(c, view.artist, tx, y, tw, 1, COL_TEXT);
            y += 6;
        }
        if (view.album[0]) {
            y += drawWrapped(c, view.album, tx, y, tw, 1, COL_MUTED);
            y += 6;
        }
        if (view.status[0]) {
            y += drawWrapped(c, view.status, tx, y, tw, 1, COL_WARN);
            y += 6;
        }

        if (haveTrack) {
            char left[16], right[16], line[40];
            shortClock(view.rel, left, sizeof(left));
            shortClock(view.dur, right, sizeof(right));
            snprintf(line, sizeof(line), "%s   /   %s", left, right);
            if (y > PLAY.y - 90) y = PLAY.y - 90;
            y += 8;
            text(c, line, tx, y, tw, COL_MUTED);
            y += c.fontHeight() + 10;
            int total = clockSeconds(view.dur);
            int pos = clockSeconds(view.rel);
            int bw = tw > 40 ? tw : 40;
            c.fillRoundRect(tx, y, bw, 10, 5, COL_CARD2);
            if (total > 0) {
                if (pos > total) pos = total;
                int fill = (int)((int64_t)bw * pos / total);
                if (fill > 0) c.fillRoundRect(tx, y, fill, 10, 5, COL_ACCENT);
            }
        }
        c.clearClipRect();

        auto transport = [&](const Box& b, char kind, bool accent) {
            bool hot = pressedAt(b.x, b.y, b.w, b.h);
            uint16_t bg = accent ? COL_ACCENT : COL_CARD;
            if (hot) bg = accent ? COL_TEXT : COL_CARD2;
            c.fillRoundRect(b.x, b.y, b.w, b.h, 16, bg);
            uint16_t fg = accent && !hot ? COL_BG : COL_TEXT;
            drawTransportIcon(c, b, kind, fg);
        };
        transport(PREV, '<', false);
        transport(PLAY, view.playing ? 's' : 'p', true);
        transport(NEXT, '>', false);
    } else {
        c.setFont(&fontUi);
        const char* heading = "Sonos favorites";
        if (panel == Panel::Queue) heading = view.queueName[0] ? view.queueName : "Queue";
        text(c, heading, FAV_X, FAV_Y, FAV_W, COL_TEXT);
        int favH = FAV_B - FAV_LIST;
        int favMax = listMax(view.favCount, favH, FAV_H);
        if (favScroll > favMax) favScroll = favMax;
        if (favScroll < 0) favScroll = 0;
        c.setClipRect(FAV_X, FAV_LIST, FAV_W, favH);
        if (view.loadingFavs && view.favCount == 0) {
            c.setFont(&fontBody);
            text(c, panel == Panel::Queue ? "Loading queue..." : "Loading favorites...", FAV_X, FAV_LIST + 8, FAV_W, COL_MUTED);
        } else if (view.favCount == 0) {
            c.setFont(&fontBody);
            if (panel == Panel::Queue) {
                bool station = !strcmp(view.source, "Radio") || !strcmp(view.source, "SoundCloud") ||
                               !strcmp(view.source, "TV") || !strcmp(view.source, "Line-in");
                bool liveRadio = !strcmp(view.queueName, "Spotify artist radio") || !strcmp(view.queueName, "Spotify radio") ||
                                 !strcmp(view.queueName, "Sonos Radio") || !strcmp(view.queueName, "Radio");
                if (!view.source[0] && !view.title[0]) {
                    text(c, "Nothing playing in this room.", FAV_X, FAV_LIST + 8, FAV_W, COL_TEXT);
                } else if (liveRadio) {
                    text(c, "This plays on its own.", FAV_X, FAV_LIST + 8, FAV_W, COL_TEXT);
                    text(c, "There is no playlist to open.", FAV_X, FAV_LIST + 52, FAV_W, COL_MUTED);
                } else if (!strcmp(view.source, "Spotify")) {
                    text(c, "Couldn't read this Spotify playlist.", FAV_X, FAV_LIST + 8, FAV_W, COL_TEXT);
                } else if (station) {
                    char line[80];
                    snprintf(line, sizeof(line), "No track list from %s.", view.source[0] ? view.source : "this source");
                    text(c, line, FAV_X, FAV_LIST + 8, FAV_W, COL_TEXT);
                    text(c, "A station is one continuous stream.", FAV_X, FAV_LIST + 52, FAV_W, COL_MUTED);
                } else {
                    text(c, "This room has no queue.", FAV_X, FAV_LIST + 8, FAV_W, COL_TEXT);
                }
            } else {
                text(c, "No favorites yet.", FAV_X, FAV_LIST + 8, FAV_W, COL_TEXT);
                text(c, "Save a Spotify playlist in the Sonos app,", FAV_X, FAV_LIST + 52, FAV_W, COL_MUTED);
                text(c, "then open this list again.", FAV_X, FAV_LIST + 100, FAV_W, COL_MUTED);
            }
        } else {
            for (int i = 0; i < view.favCount; i++) {
                int y = FAV_LIST + i * FAV_H - favScroll;
                if (y + FAV_H < FAV_LIST || y > FAV_B) continue;
                bool current = panel == Panel::Queue && view.track == i + 1;
                bool hot = pressedAt(FAV_X, y + 4, FAV_W, FAV_H - 8);
                c.fillRoundRect(FAV_X, y + 4, FAV_W, FAV_H - 8, 12, (current || hot) ? COL_CARD2 : COL_CARD);
                if (current || hot) c.drawRoundRect(FAV_X, y + 4, FAV_W, FAV_H - 8, 12, COL_ACCENT);
                c.setFont(&fontBody);
                int labelX = FAV_X + 18;
                if (current) {
                    int cy = y + FAV_H / 2;
                    c.fillTriangle(FAV_X + 18, cy - 9, FAV_X + 18, cy + 9, FAV_X + 34, cy, COL_ACCENT);
                    labelX = FAV_X + 46;
                }
                text(c, view.favs[i], labelX, y + 18, FAV_W - (labelX - FAV_X) - 16, current ? COL_ACCENT : COL_TEXT);
            }
        }
        c.clearClipRect();
    }

controls:
    int vol = displayVolume();
    c.setFont(&fontBody);
    char volLabel[16];
    if (view.muted) strlcpy(volLabel, "Muted", sizeof(volLabel));
    else if (view.volume < 0 && drag != Drag::Volume && !freezeVol) strlcpy(volLabel, "Volume", sizeof(volLabel));
    else snprintf(volLabel, sizeof(volLabel), "Volume  %d", vol);
    text(c, volLabel, VOL.x, VOL.y, 280, view.muted ? COL_WARN : COL_MUTED);
    int barY = VOL.y + 46;
    c.fillRoundRect(VOL.x, barY, VOL.w, 14, 7, COL_CARD2);
    int fill = VOL.w * vol / 100;
    if (fill > 0) c.fillRoundRect(VOL.x, barY, fill, 14, 7, view.muted ? COL_MUTED : COL_ACCENT);
    int knob = VOL.x + fill;
    if (knob < VOL.x + 12) knob = VOL.x + 12;
    if (knob > VOL.x + VOL.w - 12) knob = VOL.x + VOL.w - 12;
    c.fillCircle(knob, barY + 7, 16, view.muted ? COL_MUTED : COL_TEXT);

    bool muteHot = pressedAt(MUTE.x, MUTE.y, MUTE.w, MUTE.h);
    uint16_t muteBg = (view.muted || muteHot) ? COL_ACCENT : COL_CARD;
    uint16_t muteInk = (view.muted || muteHot) ? COL_BG : COL_TEXT;
    c.fillRoundRect(MUTE.x, MUTE.y, MUTE.w, MUTE.h, 16, muteBg);
    int mx = MUTE.x + MUTE.w / 2;
    int my = MUTE.y + MUTE.h / 2;
    c.fillRoundRect(mx - 14, my - 18, 28, 36, 4, muteInk);
    c.fillCircle(mx, my - 4, 8, muteBg);
    c.fillCircle(mx, my - 4, 3, muteInk);
    c.fillRoundRect(mx - 6, my + 10, 12, 4, 2, muteBg);
    if (view.muted) {
        for (int i = 0; i < 3; i++) c.drawLine(mx - 18 + i, my + 20, mx + 16 + i, my - 20, muteInk);
    }
}

void maybeDecode() {
    if (!jpg || !art.getBuffer()) return;
    if (view.artGen == shownGen) return;
    int gen = 0;
    size_t n = sonos::copyArt(jpg, JPG_CAP, &gen);
    shownGen = gen;
    art.fillSprite(COL_CARD);
    hasArt = false;
    bool jpeg = n > 2 && jpg[0] == 0xFF && jpg[1] == 0xD8;
    bool png = n > 8 && jpg[0] == 0x89 && jpg[1] == 'P' && jpg[2] == 'N' && jpg[3] == 'G';
    auto fit = lgfx::v1::textdatum::middle_center;
    if (jpeg) hasArt = art.drawJpg(jpg, n, 0, 0, ART, ART, 0, 0, 0.0f, 0.0f, fit);
    else if (png) hasArt = art.drawPng(jpg, n, 0, 0, ART, ART, 0, 0, 0.0f, 0.0f, fit);
    if ((jpeg || png) && !hasArt) Serial.printf("[ui] art decode failed (%u bytes)\n", (unsigned)n);
}

void onSettingsTap(int x, int y) {
    char key = keyAt(x, y);
    if (key && wifiField) {
        char* buf = wifiField == 1 ? wifiSsid : wifiPass;
        size_t cap = wifiField == 1 ? sizeof(wifiSsid) : sizeof(wifiPass);
        size_t n = strlen(buf);
        if (key == '\b') {
            if (n) buf[n - 1] = 0;
        } else if (key == '\n') {
            keyboard = false;
        } else if (key == '\t') {
            wifiShift = !wifiShift;
        } else if (n + 1 < cap) {
            char ch = key;
            if (wifiShift && ch >= 'a' && ch <= 'z') {
                ch = ch - 'a' + 'A';
                wifiShift = false;
            }
            buf[n] = ch;
            buf[n + 1] = 0;
        }
        return;
    }
    if (!keyboard && y >= SET_TAB_Y && y < SET_TAB_Y + SET_TAB_H) {
        for (int i = 0; i < 3; i++) {
            int tx = setTabX(i);
            if (x >= tx && x < tx + SET_TAB_W) {
                settingsTab = i;
                keyboard = false;
                wifiField = 0;
                return;
            }
        }
    }
    if (keyboard) return;
    if (settingsTab == 0) {
        int h = fieldH();
        if (x >= FIELD_X && x < FIELD_X + FIELD_W && y >= ssidY() && y < ssidY() + h) {
            wifiField = 1;
            keyboard = true;
            return;
        }
        if (x >= FIELD_X && x < FIELD_X + FIELD_W && y >= passY() && y < passY() + h) {
            wifiField = 2;
            keyboard = true;
            return;
        }
        int yb = actY();
        if (y >= yb && y < yb + ACT_H) {
            if (x >= 24 && x < 416 && wifiSsid[0]) {
                if (!wifiPass[0] && net::ssid()[0] && strcmp(wifiSsid, net::ssid()) == 0) net::requestConnect("", "");
                else net::requestConnect(wifiSsid, wifiPass);
            } else if (x >= 432 && x < 824) {
                wifiSsid[0] = wifiPass[0] = 0;
                net::requestForget();
            } else if (x >= 840 && x < 1256) {
                net::requestConnect("", "");
            }
        }
        return;
    }
    if (settingsTab == 1 && y >= SLEEP_Y && y < SLEEP_Y + SLEEP_H) {
        for (int i = 0; i < SLEEP_N; i++) {
            int cx = sleepX(i);
            if (x >= cx && x < cx + SLEEP_W) {
                storeSleep(SLEEP_MINS[i]);
                if (SLEEP_MINS[i] == 0 && asleep) wakeScreen();
                return;
            }
        }
    }
    if (settingsTab == 1 && y >= CLICK_Y && y < CLICK_Y + CLICK_H) {
        for (int i = 0; i < 3; i++) {
            int cx = FIELD_X + i * (CLICK_W + 16);
            if (x >= cx && x < cx + CLICK_W) {
                storeClick(i);
                return;
            }
        }
    }
}

void onTap(int x, int y) {
    if (homeHit(x, y)) {
        panel = Panel::Closed;
        keyboard = false;
        wifiField = 0;
        return;
    }
    if (SET.contains(x, y)) {
        if (panel == Panel::Settings) {
            panel = Panel::Closed;
            keyboard = false;
            wifiField = 0;
        } else {
            panel = Panel::Settings;
            keyboard = false;
            wifiField = 0;
            if (!wifiSsid[0] && net::ssid()[0]) strlcpy(wifiSsid, net::ssid(), sizeof(wifiSsid));
        }
        return;
    }
    if (!net::connected()) {
        if (panel == Panel::Settings) onSettingsTap(x, y);
        return;
    }
    if (BROWSE.contains(x, y) || QUEUE.contains(x, y) || FAV.contains(x, y)) {
        Panel next = Panel::Favs;
        if (BROWSE.contains(x, y)) next = Panel::Browse;
        else if (QUEUE.contains(x, y)) next = Panel::Queue;
        if (panel == next) panel = Panel::Closed;
        else {
            panel = next;
            favScroll = 0;
            if (next != Panel::Browse) browseScroll = 0;
            keyboard = false;
            if (next == Panel::Queue) sonos::command(sonos::Cmd::LoadQueue, 0, nullptr);
            else if (next == Panel::Favs) sonos::command(sonos::Cmd::LoadFavorites, 0, nullptr);
            else if (!youtube && !browse.canBack && (browse.count == 0 || !strcmp(browse.heading, "My playlists")))
                svc::requestSearch("", 0);
        }
        return;
    }
    if (panel == Panel::Closed && view.title[0] && youtubeNow() && STAR_NOW.contains(x, y)) {
        yt::toggleStar(tube.playingId, tube.playingTitle, tube.playingChannel);
        return;
    }
    if (panel != Panel::Browse && MUTE.contains(x, y)) {
        sonos::command(sonos::Cmd::Mute, view.muted ? 0 : 1, nullptr);
        return;
    }
    if (panel == Panel::Closed && PREV.contains(x, y)) {
        sonos::command(sonos::Cmd::Prev, 0, nullptr);
        return;
    }
    if (panel == Panel::Closed && PLAY.contains(x, y)) {
        sonos::command(view.playing ? sonos::Cmd::Pause : sonos::Cmd::Play, 0, nullptr);
        return;
    }
    if (panel == Panel::Closed && NEXT.contains(x, y)) {
        sonos::command(sonos::Cmd::Next, 0, nullptr);
        return;
    }
    if (panel == Panel::Settings) {
        onSettingsTap(x, y);
        return;
    }
    if (panel != Panel::Browse && x >= LIST_X && x < LIST_X + LIST_W && y >= LIST_Y && y < LIST_B) {
        int idx = -1;
        if (roomActionAt(x, y, &idx)) {
            sonos::command(roomAction(idx) == RoomAction::Add ? sonos::Cmd::JoinRoom : sonos::Cmd::LeaveRoom,
                           0, view.rooms[idx].uuid);
            return;
        }
        idx = roomIndexAt(y);
        if (idx >= 0) sonos::command(sonos::Cmd::ToggleRoom, 0, view.rooms[idx].uuid);
        return;
    }
    if (panel == Panel::Browse && x >= BR_X && x < BR_X + BR_W && y >= BR_TOP && y < BR_BOTTOM) {
        char key = keyAt(x, y);
        if (key == '\b') {
            size_t n = strlen(query);
            if (n) query[n - 1] = 0;
        } else if (key == '\n') {
            keyboard = false;
            browseScroll = 0;
            if (query[0]) {
                if (youtube) yt::requestSearch(query);
                else svc::requestSearch(query, 0);
            }
        } else if (key == ' ') {
            size_t n = strlen(query);
            if (n + 1 < sizeof(query)) {
                query[n] = ' ';
                query[n + 1] = 0;
            }
        } else if (key) {
            size_t n = strlen(query);
            if (n + 1 < sizeof(query)) {
                query[n] = key;
                query[n + 1] = 0;
            }
            return;
        }
        if (key) return;
        if (clearHit(x, y)) {
            query[0] = 0;
            return;
        }
        if ((youtube || !browse.canBack) && y >= BR_TOP && y < BR_TOP + BR_BTN_H) {
            if (x >= BR_X && x < BR_X + CHIP_W) {
                youtube = false;
                keyboard = false;
                browseScroll = 0;
                if (!browse.canBack && !strcmp(browse.heading, "My playlists")) svc::requestSearch("", 0);
                return;
            }
            if (x >= youtubeChipX() && x < youtubeChipX() + CHIP_W) {
                youtube = true;
                ytStars = false;
                keyboard = false;
                browseScroll = 0;
                return;
            }
            if (youtube && x >= listsX() && x < listsX() + ICON_W) {
                ytStars = true;
                keyboard = false;
                browseScroll = 0;
                return;
            }
            if (youtube && !ytStars && x >= gridChipX() && x < gridChipX() + ICON_W) {
                ytGrid = !ytGrid;
                keyboard = false;
                browseScroll = 0;
                return;
            }
            if (!youtube && x >= listsX() && x < listsX() + ICON_W) {
                query[0] = 0;
                keyboard = false;
                browseScroll = 0;
                svc::requestPlaylists(0);
                return;
            }
        }
        if (!youtube && browse.canBack && x >= BR_X && x < BR_X + 160 && y >= BR_TOP && y < BR_TOP + BR_BTN_H) {
            keyboard = false;
            browseScroll = 0;
            svc::requestBack();
            return;
        }
        if (!youtube && browse.pagePlayable && y >= BR_TOP && y < BR_TOP + BR_BTN_H) {
            bool queuePage = canQueue(browse.pageKind);
            int playX = BR_X + BR_W - 16 - 64;
            if (queuePage) playX -= 76;
            if (x >= playX && x < playX + 64) {
                svc::requestPagePlay(false);
                panel = Panel::Closed;
                return;
            }
            if (queuePage && x >= playX + 76 && x < playX + 140) {
                svc::requestPagePlay(true);
                return;
            }
        }
        if (y >= BR_SEARCH_Y && y < BR_SEARCH_Y + BR_SEARCH_H) {
            if (youtube && ytStars) {
                ytStars = false;
                browseScroll = 0;
                keyboard = true;
                return;
            }
            keyboard = !keyboard;
            return;
        }
        if (keyboard) return;
        int listTop = browseListTop();
        if (youtube) {
            int listBottom = BR_BOTTOM;
            bool onStar = x >= BR_X + BR_W - 164 && x < BR_X + BR_W - 84;
            if (ytStars) {
                if (y >= listTop && y < listBottom) {
                    int idx = (y - listTop + browseScroll) / BR_H;
                    if (idx >= 0 && idx < tube.starCount) {
                        if (onStar) yt::toggleStar(tube.stars[idx].id, tube.stars[idx].title, tube.stars[idx].channel);
                        else {
                            yt::playStar(idx);
                            panel = Panel::Closed;
                        }
                    }
                }
                return;
            }
            if (tubeGrid() && y >= listTop && y < listBottom && !tube.loading) {
                int cardW = gridCardW();
                int cardH = gridCardH();
                int strideX = cardW + GRID_GAP;
                int strideY = cardH + GRID_GAP;
                int relY = y - listTop + browseScroll;
                if (relY < 0) return;
                int row = relY / strideY;
                int contentRows = (tube.count + GRID_COLS - 1) / GRID_COLS;
                if (tube.more && row == contentRows) {
                    browseScroll = 0;
                    yt::requestMore();
                    return;
                }
                if (x < BR_X) return;
                int col = (x - BR_X) / strideX;
                if (col < 0 || col >= GRID_COLS) return;
                int localX = (x - BR_X) - col * strideX;
                int localY = relY - row * strideY;
                if (localX >= cardW || localY >= cardH) return;
                int idx = row * GRID_COLS + col;
                if (idx < 0 || idx >= tube.count) return;
                if (localX >= cardW - 72 && localY < 72) {
                    yt::toggleStar(tube.items[idx].id, tube.items[idx].title, tube.items[idx].channel);
                } else {
                    yt::requestPlay(idx);
                    panel = Panel::Closed;
                }
                return;
            }
            if (y >= listTop && y < listBottom && !tube.loading) {
                int idx = (y - listTop + browseScroll) / BR_H;
                if (idx == tube.count && tube.more) {
                    browseScroll = 0;
                    yt::requestMore();
                    return;
                }
                if (idx >= 0 && idx < tube.count) {
                    if (onStar) yt::toggleStar(tube.items[idx].id, tube.items[idx].title, tube.items[idx].channel);
                    else {
                        yt::requestPlay(idx);
                        panel = Panel::Closed;
                    }
                }
            }
            return;
        }
        int listBottom = BR_BOTTOM;
        if (y >= listTop && y < listBottom && !browse.loading) {
            int idx = (y - listTop + browseScroll) / BR_H;
            if (idx == browse.count && (browse.more || browse.offset > 0)) {
                bool back = x < BR_X + BR_W / 2 && browse.offset > 0;
                int step = browse.pageSize > 0 ? browse.pageSize : 10;
                int next = browse.offset + (back ? -step : step);
                if (next < 0) next = 0;
                browseScroll = 0;
                svc::requestPage(next);
                return;
            }
            if (idx >= 0 && idx < browse.count) {
                bool queued = canQueue(browse.items[idx].kind);
                int plusX = BR_X + BR_W - 84;
                int playX = queued ? plusX - 80 : plusX;
                if (queued && x >= plusX) {
                    svc::requestPlay(idx, true);
                    return;
                }
                if (x >= playX || browse.items[idx].kind == svc::Kind::Radio) {
                    svc::requestPlay(idx, false);
                    panel = Panel::Closed;
                    return;
                }
                browseScroll = 0;
                svc::requestOpen(idx);
            }
        }
        return;
    }
    if ((panel == Panel::Queue || panel == Panel::Favs) && x >= FAV_X && x < FAV_X + FAV_W && y >= FAV_LIST && y < FAV_B && !view.loadingFavs) {
        int idx = (y - FAV_LIST + favScroll) / FAV_H;
        if (idx >= 0 && idx < view.favCount) {
            sonos::command(sonos::Cmd::PlayFavorite, idx, nullptr);
            panel = Panel::Closed;
        }
    }
}

void beginGesture(int x, int y) {
    pressing = true;
    moved = false;
    x0 = x;
    y0 = y;
    lastY = y;
    lastActivity = millis();
    if (net::connected() && panel != Panel::Browse && VOL.contains(x, y)) {
        drag = Drag::Volume;
        int vol = (x - VOL.x) * 100 / VOL.w;
        sendVolume(vol, true);
        return;
    }
    if (!net::connected() || panel == Panel::Settings) {
        drag = Drag::None;
        return;
    }
    if (panel == Panel::Browse && keyboard && keyAt(x, y)) {
        drag = Drag::None;
        return;
    }
    if (panel == Panel::Browse && !keyboard && x >= BR_X && x < BR_X + BR_W && y >= browseListTop() && y < BR_BOTTOM) {
        drag = Drag::Favs;
        scroll0 = browseScroll;
        return;
    }
    if ((panel == Panel::Queue || panel == Panel::Favs) && x >= FAV_X && x < FAV_X + FAV_W && y >= FAV_LIST && y < FAV_B) {
        drag = Drag::Favs;
        scroll0 = favScroll;
        return;
    }
    if (panel != Panel::Browse && x >= LIST_X && x < LIST_X + LIST_W && y >= LIST_Y && y < LIST_B) {
        int idx = -1;
        if (roomActionAt(x, y, &idx)) {
            drag = Drag::None;
            return;
        }
        drag = Drag::Rooms;
        scroll0 = roomScroll;
        return;
    }
    drag = Drag::None;
}

void moveGesture(int x, int y) {
    if (!pressing) return;
    if (abs(x - x0) > 12 || abs(y - y0) > 12) moved = true;
    if (drag == Drag::Volume) {
        sendVolume((x - VOL.x) * 100 / VOL.w, false);
        return;
    }
    if (!moved) return;
    if (drag == Drag::Rooms) {
        int viewH = LIST_B - LIST_Y;
        int maxS = roomsContent > viewH ? roomsContent - viewH : 0;
        roomScroll = scroll0 - (y - y0);
        if (roomScroll < 0) roomScroll = 0;
        if (roomScroll > maxS) roomScroll = maxS;
    } else if (drag == Drag::Favs) {
        if (panel == Panel::Browse) {
            int listTop = browseListTop();
            int listBottom = BR_BOTTOM;
            int rows = youtube ? tubeRows()
                               : browse.count + (browse.more || browse.offset > 0 ? 1 : 0);
            int pitch = youtube ? tubePitch() : BR_H;
            int maxS = listMax(rows, listBottom - listTop, pitch);
            browseScroll = scroll0 - (y - y0);
            if (browseScroll < 0) browseScroll = 0;
            if (browseScroll > maxS) browseScroll = maxS;
        } else {
            int maxS = listMax(view.favCount, FAV_B - FAV_LIST, FAV_H);
            favScroll = scroll0 - (y - y0);
            if (favScroll < 0) favScroll = 0;
            if (favScroll > maxS) favScroll = maxS;
        }
    }
    lastY = y;
}

volatile bool shotPending = false;
volatile bool tapPending = false;
volatile int tapX = 0;
volatile int tapY = 0;

// Writes the finished frame to the USB serial port for README screenshots.
void sendShot() {
    const uint8_t* px = (const uint8_t*)canvas.getBuffer();
    if (!px) return;
    const size_t total = (size_t)canvas.width() * canvas.height() * 2;
    Serial.setTxTimeoutMs(1000);
    Serial.printf("\nSHOT %d %d\n", canvas.width(), canvas.height());
    // Other tasks print log lines at any time. Each chunk goes out in one write with a header
    // (A5 5A A5 5A, chunk index, length) so the receiver can skip anything between chunks.
    constexpr size_t CHUNK = 2048;
    static uint8_t packet[8 + CHUNK];
    size_t sent = 0;
    uint16_t index = 0;
    while (sent < total) {
        size_t n = total - sent < CHUNK ? total - sent : CHUNK;
        packet[0] = 0xA5;
        packet[1] = 0x5A;
        packet[2] = 0xA5;
        packet[3] = 0x5A;
        packet[4] = index & 0xFF;
        packet[5] = index >> 8;
        packet[6] = n & 0xFF;
        packet[7] = n >> 8;
        memcpy(packet + 8, px + sent, n);
        size_t w = Serial.write(packet, 8 + n);
        if (w != 8 + n) break;
        sent += n;
        index++;
    }
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    Serial.printf("\nSHOT end %u\n", (unsigned)sent);
}

void endGesture(int x, int y) {
    lastActivity = millis();
    if (drag == Drag::Volume) sendVolume(localVol, true);
    else if (!moved) {
        flashUntil = millis() + 180;
        flashX = x;
        flashY = y;
        onTap(x, y);
        buzz();
    }
    pressing = false;
    moved = false;
    drag = Drag::None;
    if (view.volume == localVol) freezeVol = false;
}

}  // namespace

void begin() {
    {
        board::I2CLock lock;
        M5.Display.setRotation(HOME_ROT);
        M5.Display.setBrightness(200);
    }
    M5.Touch.setHoldThresh(800);
    Serial.printf("[ui] display %d x %d\n", M5.Display.width(), M5.Display.height());

    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    if (!canvas.getBuffer()) {
        Serial.println("[ui] screen canvas failed");
        ready = false;
        return;
    }
    art.setColorDepth(16);
    art.setPsram(true);
    art.createSprite(ART, ART);
    jpg = (uint8_t*)heap_caps_malloc(JPG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!art.getBuffer() || !jpg) Serial.println("[ui] album art buffer failed");

    COL_BG = canvas.color565(14, 16, 20);
    COL_CARD = canvas.color565(28, 32, 40);
    COL_CARD2 = canvas.color565(42, 48, 58);
    COL_ACCENT = canvas.color565(232, 196, 120);
    COL_TEXT = canvas.color565(244, 244, 242);
    COL_MUTED = canvas.color565(154, 160, 170);
    COL_LINE = canvas.color565(48, 54, 64);
    COL_WARN = canvas.color565(232, 150, 120);
    loadSleep();
    lastActivity = millis();
    startSpeaker();
    ready = true;
    Serial.printf("[ui] canvas ok  psram free %u\n", (unsigned)ESP.getFreePsram());
}

void loop() {
    if (!ready) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            M5.Display.drawString("Not enough memory for the UI", 40, 40);
        }
        return;
    }

    sonos::copyView(view);
    svc::copyView(browse);
    yt::copyView(tube);
    layoutRooms();
    char selected[40] = "";
    for (int i = 0; i < view.roomCount; i++)
        if (view.rooms[i].coordinator) strlcpy(selected, view.rooms[i].uuid, sizeof(selected));
    static char shownRoom[40] = "";
    static int shownList = -1;
    bool roomChanged = strcmp(selected, shownRoom) != 0;
    if (roomChanged) strlcpy(shownRoom, selected, sizeof(shownRoom));
    bool listChanged = view.listGen != shownList;
    shownList = view.listGen;
    if (shownRoom[0] && panel == Panel::Queue && (roomChanged || listChanged))
        sonos::command(sonos::Cmd::LoadQueue, 0, nullptr);
    else if (shownRoom[0] && panel == Panel::Favs && roomChanged)
        sonos::command(sonos::Cmd::LoadFavorites, 0, nullptr);
    if (!freezeVol || view.volume == localVol || millis() > volFreezeUntil) {
        if (drag != Drag::Volume) freezeVol = false;
    }

    servicePower();
    serviceOrientation();
    serviceSleep();

    auto t = M5.Touch.getDetail();
    if (asleep) {
        if (t.wasPressed()) {
            wakeScreen();
            swallowTouch = true;
        }
    } else if (swallowTouch) {
        if (!t.isPressed()) swallowTouch = false;
    } else {
        if (t.wasPressed()) beginGesture(t.x, t.y);
        if (t.isPressed()) moveGesture(t.x, t.y);
        if (t.wasReleased()) endGesture(t.x, t.y);
    }

    if (tapPending) {
        tapPending = false;
        lastActivity = millis();
        if (asleep) wakeScreen();
        else onTap(tapX, tapY);
    }
    if (net::connected()) maybeDecode();
    if (net::connected()) updateStarThumbs();
    if (net::connected()) updateSpotifyThumbs();
    if (net::connected()) drawMain(canvas);
    else if (panel == Panel::Settings) {
        canvas.fillSprite(COL_BG);
        canvas.setTextDatum(TL_DATUM);
        canvas.setFont(&fontUi);
        canvas.setTextColor(COL_TEXT);
        drawHomeHeader(canvas, "Not connected", true);
        drawBattery(canvas, BATT_X, BATT_Y);
        drawHeaderButton(canvas, SET, 3, true);
        canvas.drawFastHLine(0, 108, canvas.width(), COL_LINE);
        drawSettings(canvas);
        if (keyboard) drawKeyboard(canvas);
    } else drawWifi(canvas);
    if (shotPending) {
        shotPending = false;
        sendShot();
    }
    fb::push(canvas, 0, 0);
}

void requestShot() { shotPending = true; }

void injectTap(int x, int y) {
    tapX = x;
    tapY = y;
    tapPending = true;
}

}  // namespace ui
