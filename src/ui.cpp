#include "ui.h"

#include "board.h"
#include "display_fb.h"
#include "font_ui.h"
#include "net.h"
#include "services.h"
#include "sonos.h"
#include "youtube.h"

#include <esp_heap_caps.h>
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

constexpr Box PREV{408, 608, 120, 88};
constexpr Box PLAY{540, 600, 136, 100};
constexpr Box NEXT{688, 608, 120, 88};
constexpr Box VOL{828, 612, 428, 84};
constexpr Box BROWSE{700, 18, 156, 72};
constexpr Box QUEUE{868, 18, 150, 72};
constexpr Box FAV{1030, 18, 234, 72};
constexpr Box STAR_NOW{1164, 130, 92, 92};  // star button beside the title while YouTube plays

M5Canvas canvas;
M5Canvas art;
uint8_t* jpg = nullptr;
constexpr size_t JPG_CAP = 1024 * 1024;
bool ready = false;
bool hasArt = false;
int shownGen = -1;
enum class Panel : uint8_t { Closed, Queue, Favs, Browse };
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

void text(M5Canvas& c, const char* s, int x, int y, int maxW, uint16_t col) {
    if (!s) s = "";
    c.setTextColor(col);
    int h = c.fontHeight();
    if (h < 1) h = 16;
    c.setClipRect(x, y, maxW, h + 6);
    c.drawString(s, x, y);
    c.clearClipRect();
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
        c.setClipRect(x, y, maxW, lineH);
        c.drawString(buf, x, y);
        c.clearClipRect();
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

void drawWifi(M5Canvas& c) {
    c.fillSprite(COL_BG);
    c.setTextDatum(TL_DATUM);
    c.setFont(&fontTitle);
    c.setTextColor(COL_TEXT);
    c.drawString("Sonos", 72, 120);
    c.setFont(&fontBody);
    if (net::state() == net::State::Connecting) {
        char line[80];
        snprintf(line, sizeof(line), "Joining %s", net::ssid());
        c.drawString(line, 72, 240);
        text(c, "2.4 GHz only. A 5 GHz network will not be found.", 72, 320, 1100, COL_MUTED);
    } else if (net::setupActive()) {
        c.drawString("On your phone, join", 72, 230);
        c.setFont(&fontUi);
        c.setTextColor(COL_ACCENT);
        c.drawString(net::setupName()[0] ? net::setupName() : "TabSonos", 72, 310);
        c.setFont(&fontBody);
        c.setTextColor(COL_TEXT);
        c.drawString("Then open   http://192.168.4.1", 72, 400);
        text(c, "The controller needs a 2.4 GHz network.", 72, 480, 1100, COL_MUTED);
        if (net::setupClosing()) c.drawString("Connected. Closing the setup network...", 72, 560);
    } else {
        c.drawString("Waiting for Wi-Fi", 72, 240);
    }
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

int tubeRows() { return ytStars ? tube.starCount : tube.count + (tube.more ? 1 : 0); }

constexpr int BR_X = 16;
constexpr int BR_W = 1248;
constexpr int BR_TOP = 116;
constexpr int BR_SAFE = 580;
constexpr int BR_BTN_H = 52;
constexpr int BR_SEARCH_Y = BR_TOP + BR_BTN_H + 12;
constexpr int BR_SEARCH_H = 84;
constexpr int BR_LIST = BR_SEARCH_Y + BR_SEARCH_H + 16;
constexpr int BR_H = 118;
constexpr int BR_CLEAR = 72;
constexpr int KEY_H = 64;
constexpr int KEY_ROW = 72;
constexpr int KEY_Y = BR_SAFE - 4 * KEY_ROW;

int clearX() { return BR_X + BR_W - 16 - BR_CLEAR; }
int clearY() { return BR_SEARCH_Y + (BR_SEARCH_H - BR_CLEAR) / 2; }
bool clearHit(int x, int y) {
    return x >= clearX() && x < clearX() + BR_CLEAR && y >= clearY() && y < clearY() + BR_CLEAR;
}

constexpr int CHIP_W = 200;

int youtubeChipX() { return BR_X + CHIP_W + 16; }
int listsX() { return BR_X + 2 * (CHIP_W + 16); }

int browseListTop() {
    if (youtube && ytStars) return BR_LIST;
    bool show = youtube ? (tube.status[0] && tube.count > 0) : (browse.status[0] && browse.count > 0);
    if (!keyboard && show) return BR_LIST + 56;
    return BR_LIST;
}

void sourceChip(M5Canvas& c, int x, const char* label, bool on) {
    bool hot = pressing && !moved && x0 >= x && x0 < x + CHIP_W && y0 >= BR_TOP && y0 < BR_TOP + BR_BTN_H;
    c.fillRoundRect(x, BR_TOP, CHIP_W, BR_BTN_H, 12, (on || hot) ? COL_ACCENT : COL_CARD);
    c.setFont(&fontBrowse);
    c.setTextDatum(MC_DATUM);
    c.setTextColor((on || hot) ? COL_BG : COL_TEXT);
    c.drawString(label, x + CHIP_W / 2, BR_TOP + BR_BTN_H / 2);
    c.setTextDatum(TL_DATUM);
}

char keyAt(int x, int y) {
    if (!keyboard || x < BR_X || x >= BR_X + BR_W || y < KEY_Y || y >= BR_SAFE) return 0;
    const char* rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int row = 0; row < 3; row++) {
        int cols = (int)strlen(rows[row]) + (row == 2 ? 1 : 0);
        int gap = 10;
        int w = (BR_W - (cols - 1) * gap) / cols;
        for (int col = 0; col < (int)strlen(rows[row]); col++) {
            int bx = BR_X + col * (w + gap);
            int by = KEY_Y + row * KEY_ROW;
            if (x >= bx && y >= by && x < bx + w && y < by + KEY_H) return rows[row][col];
        }
        if (row == 2) {
            int col = (int)strlen(rows[row]);
            int bx = BR_X + col * (w + gap);
            int by = KEY_Y + row * KEY_ROW;
            if (x >= bx && y >= by && x < bx + w && y < by + KEY_H) return '\b';
        }
    }
    int by = KEY_Y + 3 * KEY_ROW;
    if (y >= by && y < by + KEY_H) {
        if (x < BR_X + 860) return ' ';
        return '\n';
    }
    return 0;
}

void drawKey(M5Canvas& c, int x, int y, int w, const char* label, bool accent) {
    c.fillRoundRect(x, y, w, KEY_H, 12, accent ? COL_ACCENT : COL_CARD2);
    c.setFont(&fontBrowse);
    c.setTextColor(accent ? COL_BG : COL_TEXT);
    c.setTextDatum(MC_DATUM);
    c.drawString(label, x + w / 2, y + KEY_H / 2);
    c.setTextDatum(TL_DATUM);
}

void drawKeyboard(M5Canvas& c) {
    c.setClipRect(0, KEY_Y, c.width(), BR_SAFE - KEY_Y);
    const char* rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (int row = 0; row < 3; row++) {
        int cols = (int)strlen(rows[row]) + (row == 2 ? 1 : 0);
        int gap = 10;
        int w = (BR_W - (cols - 1) * gap) / cols;
        for (int col = 0; col < (int)strlen(rows[row]); col++) {
            char label[2] = {rows[row][col], 0};
            drawKey(c, BR_X + col * (w + gap), KEY_Y + row * KEY_ROW, w, label, false);
        }
        if (row == 2) {
            int col = (int)strlen(rows[row]);
            drawKey(c, BR_X + col * (w + gap), KEY_Y + row * KEY_ROW, w, "del", false);
        }
    }
    int by = KEY_Y + 3 * KEY_ROW;
    drawKey(c, BR_X, by, 844, "space", false);
    drawKey(c, BR_X + 860, by, BR_W - 860, "Search", true);
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

// Decodes at most one new thumbnail per frame and frees pictures of videos that are no longer starred.
void updateStarThumbs() {
    for (StarThumb& t : starThumbs) {
        if (!t.used) continue;
        bool keep = false;
        for (int i = 0; i < tube.starCount; i++)
            if (!strcmp(tube.stars[i].id, t.id)) keep = true;
        if (keep) continue;
        if (t.sprite) {
            t.sprite->deleteSprite();
            delete t.sprite;
        }
        t = StarThumb{};
    }
    if (!jpg || !youtube || !ytStars || panel != Panel::Browse) return;
    for (int i = 0; i < tube.starCount; i++) {
        if (!tube.starThumb[i] || thumbFor(tube.stars[i].id)) continue;
        StarThumb* slot = nullptr;
        for (StarThumb& t : starThumbs)
            if (!t.used) {
                slot = &t;
                break;
            }
        if (!slot) return;
        size_t n = yt::copyThumb(tube.stars[i].id, jpg, JPG_CAP);
        slot->used = true;
        strlcpy(slot->id, tube.stars[i].id, sizeof(slot->id));
        slot->sprite = nullptr;
        if (!n) return;
        M5Canvas* sp = new M5Canvas();
        sp->setColorDepth(16);
        sp->setPsram(true);
        sp->createSprite(THUMB_W, THUMB_H);
        if (sp->getBuffer()) {
            sp->fillSprite(COL_CARD2);
            auto fit = lgfx::v1::textdatum::middle_center;
            if (sp->drawJpg(jpg, n, 0, 0, THUMB_W, THUMB_H, 0, 0, 0.0f, 0.0f, fit)) slot->sprite = sp;
        }
        if (!slot->sprite) {
            sp->deleteSprite();
            delete sp;
            Serial.printf("[ui] thumbnail decode failed (%u bytes)\n", (unsigned)n);
        }
        return;
    }
}

void drawStarList(M5Canvas& c) {
    int listTop = BR_LIST;
    int listBottom = BR_SAFE;
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
        bool starHot = pressing && !moved && x0 >= starX && x0 < starX + 68 && y0 >= y + 24 && y0 < y + 94;
        bool playHot = pressing && !moved && x0 >= markX && x0 < markX + 68 && y0 >= y + 24 && y0 < y + 94;
        drawStarMark(c, starX, y + 26, 68, 66, true, starHot);
        drawMark(c, markX, y + 26, 68, 66, true, playHot);
    }
    c.clearClipRect();
}

void drawTube(M5Canvas& c) {
    if (ytStars) {
        drawStarList(c);
        return;
    }
    int listTop = browseListTop();
    int listBottom = BR_SAFE;
    c.setFont(&fontBrowse);
    if (tube.status[0]) text(c, tube.status, BR_X, BR_LIST, BR_W, COL_MUTED);
    int rows = tube.count + (tube.more ? 1 : 0);
    int viewH = listBottom - listTop;
    int maxS = listMax(rows, viewH, BR_H);
    if (browseScroll > maxS) browseScroll = maxS;
    if (browseScroll < 0) browseScroll = 0;
    c.setClipRect(BR_X, listTop, BR_W, viewH);
    if (tube.loading && tube.count == 0) {
        text(c, "Searching YouTube...", BR_X, listTop + 8, BR_W, COL_MUTED);
    } else if (!tube.loading && tube.count == 0 && !tube.status[0]) {
        text(c, "Search YouTube, then tap Search", BR_X, listTop + 8, BR_W, COL_MUTED);
    }
    for (int i = 0; i < tube.count; i++) {
        int y = listTop + i * BR_H - browseScroll;
        if (y + BR_H < listTop || y > listBottom) continue;
        int gutter = 176;
        c.fillRoundRect(BR_X, y + 4, BR_W - gutter, BR_H - 8, 14, COL_CARD);
        c.setFont(&fontBrowse);
        text(c, tube.items[i].title, BR_X + 20, y + 8, BR_W - gutter - 36, COL_TEXT);
        char clock[16] = "";
        int seconds = tube.items[i].seconds;
        if (seconds > 0) {
            if (seconds >= 3600)
                snprintf(clock, sizeof(clock), "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
            else
                snprintf(clock, sizeof(clock), "%d:%02d", seconds / 60, seconds % 60);
        }
        char line[100];
        snprintf(line, sizeof(line), "%s%s%s", tube.items[i].channel, clock[0] ? "  " : "", clock);
        c.setFont(&fontBrowseBody);
        text(c, line, BR_X + 20, y + 62, BR_W - gutter - 36, COL_MUTED);
        int markX = BR_X + BR_W - 80;
        bool playHot = pressing && !moved && x0 >= markX && x0 < markX + 68 && y0 >= y + 24 && y0 < y + 94;
        drawMark(c, markX, y + 26, 68, 66, true, playHot);
        int starX = BR_X + BR_W - 156;
        bool starHot = pressing && !moved && x0 >= starX && x0 < starX + 68 && y0 >= y + 24 && y0 < y + 94;
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
    c.fillRect(0, 109, c.width(), BR_SAFE - 109, COL_BG);
    c.setFont(&fontBrowse);
    bool queuePage = !youtube && browse.pagePlayable && canQueue(browse.pageKind);
    int playX = BR_X + BR_W - 16 - 64;
    if (queuePage) playX -= 76;
    if (youtube || !browse.canBack) {
        sourceChip(c, BR_X, "Spotify", !youtube);
        sourceChip(c, youtubeChipX(), "YouTube", youtube && !ytStars);
        if (youtube) sourceChip(c, listsX(), "Starred", ytStars);
        if (!youtube) {
            bool lists = pressing && !moved && x0 >= listsX() && x0 < listsX() + 320 && y0 >= BR_TOP && y0 < BR_TOP + BR_BTN_H;
            c.fillRoundRect(listsX(), BR_TOP, 320, BR_BTN_H, 12, lists ? COL_ACCENT : COL_CARD);
            c.setTextDatum(MC_DATUM);
            c.setTextColor(lists ? COL_BG : COL_TEXT);
            c.drawString("My playlists", listsX() + 160, BR_TOP + BR_BTN_H / 2);
            c.setTextDatum(TL_DATUM);
        }
    } else if (browse.canBack) {
        bool hot = pressing && !moved && x0 >= BR_X && x0 < BR_X + 160 && y0 >= BR_TOP && y0 < BR_TOP + BR_BTN_H;
        c.fillRoundRect(BR_X, BR_TOP, 160, BR_BTN_H, 12, hot ? COL_ACCENT : COL_CARD);
        c.setTextDatum(MC_DATUM);
        c.setTextColor(hot ? COL_BG : COL_TEXT);
        c.drawString("Back", BR_X + 80, BR_TOP + BR_BTN_H / 2);
        c.setTextDatum(TL_DATUM);
        int titleW = (browse.pagePlayable ? playX : BR_X + BR_W) - (BR_X + 176) - 12;
        if (titleW < 40) titleW = 40;
        text(c, browse.heading[0] ? browse.heading : "Spotify", BR_X + 176, BR_TOP, titleW, COL_TEXT);
    }
    if (!youtube && browse.pagePlayable) {
        bool hot = pressing && !moved && x0 >= playX && x0 < playX + 64 && y0 >= BR_TOP && y0 < BR_TOP + BR_BTN_H;
        drawMark(c, playX, BR_TOP, 64, BR_BTN_H, true, hot);
        if (queuePage) {
            int plusX = playX + 76;
            bool plusHot = pressing && !moved && x0 >= plusX && x0 < plusX + 64 && y0 >= BR_TOP && y0 < BR_TOP + BR_BTN_H;
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
    bool clearing = pressing && !moved && clearHit(x0, y0);
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
    int listBottom = BR_SAFE;
    c.setFont(&fontBrowse);
    if (browse.status[0]) text(c, browse.status, BR_X, BR_LIST, BR_W, COL_MUTED);
    int rows = browse.count + (browse.more || browse.offset > 0 ? 1 : 0);
    int viewH = listBottom - listTop;
    int maxS = listMax(rows, viewH, BR_H);
    if (browseScroll > maxS) browseScroll = maxS;
    if (browseScroll < 0) browseScroll = 0;
    c.setClipRect(BR_X, listTop, BR_W, viewH);
    if (browse.loading && browse.count == 0) {
        text(c, "Loading Spotify...", BR_X, listTop + 8, BR_W, COL_MUTED);
    }
    for (int i = 0; i < browse.count; i++) {
        int y = listTop + i * BR_H - browseScroll;
        if (y + BR_H < listTop || y > listBottom) continue;
        bool queued = canQueue(browse.items[i].kind);
        int gutter = queued ? 176 : 96;
        c.fillRoundRect(BR_X, y + 4, BR_W - gutter, BR_H - 8, 14, COL_CARD);
        c.setFont(&fontBrowse);
        text(c, browse.items[i].title, BR_X + 20, y + 8, BR_W - gutter - 36, COL_TEXT);
        char line[100];
        snprintf(line, sizeof(line), "%s%s%s", kindName(browse.items[i].kind), browse.items[i].subtitle[0] ? "  " : "",
                 browse.items[i].subtitle);
        c.setFont(&fontBrowseBody);
        text(c, line, BR_X + 20, y + 62, BR_W - gutter - 36, COL_MUTED);
        int plusX = BR_X + BR_W - 80;
        int markX = queued ? plusX - 80 : plusX;
        bool playHot = pressing && !moved && x0 >= markX && x0 < markX + 68 && y0 >= y + 24 && y0 < y + 94;
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

void drawMain(M5Canvas& c) {
    c.fillSprite(COL_BG);
    c.setTextDatum(TL_DATUM);
    c.setFont(&fontUi);
    c.setTextColor(COL_TEXT);
    c.drawString("Sonos", 24, 22);
    if (!view.household) text(c, "Searching for speakers...", 180, 22, 660, COL_MUTED);
    else if (view.groupName[0]) text(c, view.groupName, 180, 22, 500, COL_TEXT);
    c.setFont(&fontBody);
    if (net::localIp()[0]) text(c, net::localIp(), 24, 66, 280, COL_MUTED);

    auto headerButton = [&](const Box& b, const char* label, bool on) {
        bool hot = pressing && !moved && b.contains(x0, y0);
        c.fillRoundRect(b.x, b.y, b.w, b.h, 12, (on || hot) ? COL_ACCENT : COL_CARD);
        c.setFont(&fontUi);
        c.setTextColor((on || hot) ? COL_BG : COL_TEXT);
        c.setTextDatum(MC_DATUM);
        c.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
        c.setTextDatum(TL_DATUM);
    };
    headerButton(BROWSE, "Browse", panel == Panel::Browse);
    headerButton(QUEUE, "Queue", panel == Panel::Queue);
    headerButton(FAV, "Favorites", panel == Panel::Favs);

    c.drawFastHLine(0, 108, c.width(), COL_LINE);
    if (panel != Panel::Browse) {
        c.drawFastVLine(380, 108, c.height() - 108, COL_LINE);
        c.drawFastHLine(380, PLAY.y - 8, c.width() - 380, COL_LINE);
    } else {
        c.drawFastHLine(0, PLAY.y - 8, c.width(), COL_LINE);
    }

    int roomsH = LIST_B - LIST_Y;
    if (panel == Panel::Browse) {
        drawBrowse(c);
        goto controls;
    }
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
            if (selected) c.drawRoundRect(LIST_X, y, LIST_W - 8, cardH, 14, COL_ACCENT);
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
                bool hot = pressing && !moved && x0 >= ax && y0 >= ay && x0 < ax + aw && y0 < ay + ah;
                int cx = ax + aw / 2;
                int cy = ay + ah / 2;
                bool add = act == RoomAction::Add;
                c.fillCircle(cx, cy, 18, (add || hot) ? COL_ACCENT : COL_CARD2);
                uint16_t ink = (add || hot) ? COL_BG : COL_TEXT;
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
            tw -= 108;
            bool hot = pressing && !moved && STAR_NOW.contains(x0, y0);
            drawStarMark(c, STAR_NOW.x, STAR_NOW.y, STAR_NOW.w, STAR_NOW.h, isStarred(tube.playingId), hot);
        }
        int y = ART_Y;
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
            if (y > CTRL_Y - 70) y = CTRL_Y - 70;
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
                c.fillRoundRect(FAV_X, y + 4, FAV_W, FAV_H - 8, 12, current ? COL_CARD2 : COL_CARD);
                if (current) c.drawRoundRect(FAV_X, y + 4, FAV_W, FAV_H - 8, 12, COL_ACCENT);
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
    auto button = [&](const Box& b, char kind, bool accent) {
        bool hot = pressing && !moved && b.contains(x0, y0);
        uint16_t bg = accent ? COL_ACCENT : COL_CARD;
        if (hot) bg = accent ? COL_TEXT : COL_CARD2;
        c.fillRoundRect(b.x, b.y, b.w, b.h, 16, bg);
        uint16_t fg = accent && !hot ? COL_BG : COL_TEXT;
        drawTransportIcon(c, b, kind, fg);
    };
    button(PREV, '<', false);
    button(PLAY, view.playing ? 's' : 'p', true);
    button(NEXT, '>', false);

    int vol = displayVolume();
    c.setFont(&fontBody);
    char volLabel[16];
    if (view.volume < 0 && drag != Drag::Volume && !freezeVol) strlcpy(volLabel, "Volume", sizeof(volLabel));
    else snprintf(volLabel, sizeof(volLabel), "Volume  %d", vol);
    text(c, volLabel, VOL.x, VOL.y, 280, COL_MUTED);
    int barY = VOL.y + 46;
    c.fillRoundRect(VOL.x, barY, VOL.w, 14, 7, COL_CARD2);
    int fill = VOL.w * vol / 100;
    if (fill > 0) c.fillRoundRect(VOL.x, barY, fill, 14, 7, COL_ACCENT);
    int knob = VOL.x + fill;
    if (knob < VOL.x + 12) knob = VOL.x + 12;
    if (knob > VOL.x + VOL.w - 12) knob = VOL.x + VOL.w - 12;
    c.fillCircle(knob, barY + 7, 16, COL_TEXT);
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

void onTap(int x, int y) {
    if (!net::connected()) return;
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
            else if (!youtube && !browse.canBack && browse.count == 0) svc::requestPlaylists(0);
        }
        return;
    }
    if (panel == Panel::Closed && view.title[0] && youtubeNow() && STAR_NOW.contains(x, y)) {
        yt::toggleStar(tube.playingId, tube.playingTitle, tube.playingChannel);
        return;
    }
    if (PREV.contains(x, y)) {
        sonos::command(sonos::Cmd::Prev, 0, nullptr);
        return;
    }
    if (PLAY.contains(x, y)) {
        sonos::command(view.playing ? sonos::Cmd::Pause : sonos::Cmd::Play, 0, nullptr);
        return;
    }
    if (NEXT.contains(x, y)) {
        sonos::command(sonos::Cmd::Next, 0, nullptr);
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
    if (panel == Panel::Browse && x >= BR_X && x < BR_X + BR_W && y >= BR_TOP && y < BR_SAFE) {
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
                return;
            }
            if (x >= youtubeChipX() && x < youtubeChipX() + CHIP_W) {
                youtube = true;
                ytStars = false;
                keyboard = false;
                browseScroll = 0;
                return;
            }
            if (youtube && x >= listsX() && x < listsX() + CHIP_W) {
                ytStars = true;
                keyboard = false;
                browseScroll = 0;
                return;
            }
            if (!youtube && x >= listsX() && x < listsX() + 320) {
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
            int listBottom = BR_SAFE;
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
            if (y >= listTop && y < listBottom && !tube.loading) {
                int idx = (y - listTop + browseScroll) / BR_H;
                if (idx == tube.count && tube.more) {
                    browseScroll = 0;
                    yt::requestMore();
                    return;
                }
                if (idx >= 0 && idx < tube.count) {
                    if (onStar) yt::toggleStar(tube.items[idx].id, tube.items[idx].title, tube.items[idx].channel);
                    else yt::requestPlay(idx);
                }
            }
            return;
        }
        int listBottom = BR_SAFE;
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
    if (net::connected() && VOL.contains(x, y)) {
        drag = Drag::Volume;
        int vol = (x - VOL.x) * 100 / VOL.w;
        sendVolume(vol, true);
        return;
    }
    if (panel == Panel::Browse && keyboard && keyAt(x, y)) {
        drag = Drag::None;
        return;
    }
    if (panel == Panel::Browse && !keyboard && x >= BR_X && x < BR_X + BR_W && y >= browseListTop() && y < BR_SAFE) {
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
            int listBottom = BR_SAFE;
            int rows = youtube ? tubeRows()
                               : browse.count + (browse.more || browse.offset > 0 ? 1 : 0);
            int maxS = listMax(rows, listBottom - listTop, BR_H);
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

void endGesture(int x, int y) {
    if (drag == Drag::Volume) sendVolume(localVol, true);
    else if (!moved) onTap(x, y);
    pressing = false;
    moved = false;
    drag = Drag::None;
    if (view.volume == localVol) freezeVol = false;
}

}  // namespace

void begin() {
    {
        board::I2CLock lock;
        M5.Display.setRotation(1);
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

    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) beginGesture(t.x, t.y);
    if (t.isPressed()) moveGesture(t.x, t.y);
    if (t.wasReleased()) endGesture(t.x, t.y);

    if (net::connected()) maybeDecode();
    if (net::connected()) updateStarThumbs();
    if (net::connected()) drawMain(canvas);
    else drawWifi(canvas);
    fb::push(canvas, 0, 0);
}

}  // namespace ui
