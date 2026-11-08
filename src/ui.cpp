#include "ui.h"

#include "board.h"
#include "display_fb.h"
#include "font_ui.h"
#include "net.h"
#include "sonos.h"

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
constexpr Box QUEUE{868, 18, 186, 72};
constexpr Box FAV{1068, 18, 196, 72};

M5Canvas canvas;
M5Canvas art;
uint8_t* jpg = nullptr;
constexpr size_t JPG_CAP = 1024 * 1024;
bool ready = false;
bool hasArt = false;
int shownGen = -1;
enum class Panel : uint8_t { Closed, Queue, Favs };
Panel panel = Panel::Closed;
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

void drawMain(M5Canvas& c) {
    c.fillSprite(COL_BG);
    c.setTextDatum(TL_DATUM);
    c.setFont(&fontUi);
    c.setTextColor(COL_TEXT);
    c.drawString("Sonos", 24, 22);
    if (!view.household) text(c, "Searching for speakers...", 180, 22, 660, COL_MUTED);
    else if (view.groupName[0]) text(c, view.groupName, 180, 22, 660, COL_TEXT);
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
    headerButton(QUEUE, "Queue", panel == Panel::Queue);
    headerButton(FAV, "Favorites", panel == Panel::Favs);

    c.drawFastHLine(0, 108, c.width(), COL_LINE);
    c.drawFastVLine(380, 108, c.height() - 108, COL_LINE);
    c.drawFastHLine(380, PLAY.y - 8, c.width() - 380, COL_LINE);

    int roomsH = LIST_B - LIST_Y;
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
    if (QUEUE.contains(x, y) || FAV.contains(x, y)) {
        Panel next = QUEUE.contains(x, y) ? Panel::Queue : Panel::Favs;
        if (panel == next) panel = Panel::Closed;
        else {
            panel = next;
            favScroll = 0;
            sonos::command(next == Panel::Queue ? sonos::Cmd::LoadQueue : sonos::Cmd::LoadFavorites, 0, nullptr);
        }
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
    if (x >= LIST_X && x < LIST_X + LIST_W && y >= LIST_Y && y < LIST_B) {
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
    if (panel != Panel::Closed && x >= FAV_X && x < FAV_X + FAV_W && y >= FAV_LIST && y < FAV_B && !view.loadingFavs) {
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
    if (panel != Panel::Closed && x >= FAV_X && x < FAV_X + FAV_W && y >= FAV_LIST && y < FAV_B) {
        drag = Drag::Favs;
        scroll0 = favScroll;
        return;
    }
    if (x >= LIST_X && x < LIST_X + LIST_W && y >= LIST_Y && y < LIST_B) {
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
        int maxS = listMax(view.favCount, FAV_B - FAV_LIST, FAV_H);
        favScroll = scroll0 - (y - y0);
        if (favScroll < 0) favScroll = 0;
        if (favScroll > maxS) favScroll = maxS;
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
    if (net::connected()) drawMain(canvas);
    else drawWifi(canvas);
    fb::push(canvas, 0, 0);
}

}  // namespace ui
