#include "sonos.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <ctype.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace sonos {
namespace {

constexpr size_t SOAP_CAP = 24 * 1024;
constexpr size_t RESP_CAP = 160 * 1024;
constexpr size_t TRACK_CAP = 16 * 1024;
constexpr size_t DIDL_CAP = 2048;
constexpr size_t ART_CAP = 1024 * 1024;
constexpr size_t META_CAP = 8000;

constexpr const char* AVT = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* AVT_PATH = "/MediaRenderer/AVTransport/Control";
constexpr const char* RC = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr const char* RC_PATH = "/MediaRenderer/RenderingControl/Control";
constexpr const char* ZGT = "urn:schemas-upnp-org:service:ZoneGroupTopology:1";
constexpr const char* ZGT_PATH = "/ZoneGroupTopology/Control";
constexpr const char* CD = "urn:schemas-upnp-org:service:ContentDirectory:1";
constexpr const char* CD_PATH = "/MediaServer/ContentDirectory/Control";

struct Command {
    Cmd type;
    int value;
    char text[40];
};

struct Room {
    char name[40];
    char uuid[40];
    char group[40];
    char ip[16];
    bool coordinator;
    bool playing;
};

struct Fav {
    char title[72];
    char uri[512];
    char* meta;
    bool container;
};

struct Xml {
    char* b = nullptr;
    size_t n = 0;
    size_t cap = 0;
    bool overflow = false;

    void reset(char* buf, size_t c) {
        b = buf;
        n = 0;
        cap = c;
        overflow = false;
        if (b && cap) b[0] = 0;
    }
    void ch(char c) {
        if (!b || n + 1 >= cap) {
            overflow = true;
            return;
        }
        b[n++] = c;
        b[n] = 0;
    }
    void s(const char* p) {
        if (p)
            while (*p) ch(*p++);
    }
    void xml(const char* p) {
        if (!p) return;
        for (; *p; p++) {
            if (*p == '&') s("&amp;");
            else if (*p == '<') s("&lt;");
            else if (*p == '>') s("&gt;");
            else if (*p == '"') s("&quot;");
            else if (*p == '\'') s("&apos;");
            else ch(*p);
        }
    }
};

SemaphoreHandle_t mu = nullptr;
QueueHandle_t q = nullptr;
View published;
Xml w;

char* soapBuf = nullptr;
char* resp = nullptr;
char* trackBuf = nullptr;
char* didlBuf = nullptr;
uint8_t* art[2] = {nullptr, nullptr};
size_t artLen[2] = {0, 0};
int artSlot = 0;

Room rooms[MAX_ROOMS];
int roomCount = 0;
char active[40] = "";
bool haveHouse = false;

Fav favs[MAX_FAVS];
int favCount = 0;
bool listIsQueue = false;
bool localQueue = true;
char activeListId[240] = "";
const char* browseId = "FV:2";
char pickedArt[1400];
char fetchUrl[2000];
// What the Tab5 itself is relaying (YouTube). The speaker only knows a bare address, so the name,
// channel and thumbnail shown come from here while its current address is the relay.
char directTitle[100] = "";
char directArtist[100] = "";
char directArt[160] = "";

uint32_t lastDiscover = 0;
uint32_t lastTopo = 0;
uint32_t lastPoll = 0;
uint32_t lastSweep = 0;
uint32_t statusUntil = 0;
uint32_t volumeHoldUntil = 0;
int heldVolume = 0;
int playFails = 0;
int topoFails = 0;

char lastArtUrl[2000] = "";
char pendingArt[2000] = "";
uint32_t artRetryAt = 0;
char lastTitle[100] = "";
char lastStation[100] = "";
char lastMediaUri[320] = "";
char lastMediaCount[8] = "";
char lastListHead[72] = "";
char lastErr[48] = "";
int spotifySn = 0;

void* ps(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) Serial.printf("[sonos] out of PSRAM allocating %u\n", (unsigned)n);
    return p;
}

void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mu); }

void setStatus(const char* s) {
    lock();
    strlcpy(published.status, s ? s : "", sizeof(published.status));
    unlock();
    statusUntil = millis() + 4000;
}

void maybeClearStatus() {
    if (millis() < statusUntil) return;
    lock();
    published.status[0] = 0;
    unlock();
}

void xmlUnescape(char* s) {
    if (!s) return;
    char* r = s;
    char* d = s;
    while (*r) {
        if (*r != '&') {
            *d++ = *r++;
            continue;
        }
        if (!strncmp(r, "&lt;", 4)) {
            *d++ = '<';
            r += 4;
        } else if (!strncmp(r, "&gt;", 4)) {
            *d++ = '>';
            r += 4;
        } else if (!strncmp(r, "&amp;", 5)) {
            *d++ = '&';
            r += 5;
        } else if (!strncmp(r, "&quot;", 6)) {
            *d++ = '"';
            r += 6;
        } else if (!strncmp(r, "&apos;", 6)) {
            *d++ = '\'';
            r += 6;
        } else if (r[1] == '#') {
            r += 2;
            int v = 0;
            if (*r == 'x' || *r == 'X') {
                r++;
                while (isxdigit((unsigned char)*r)) {
                    char c = *r++;
                    v *= 16;
                    if (c <= '9') v += c - '0';
                    else v += (c & ~0x20) - 'A' + 10;
                }
            } else {
                while (isdigit((unsigned char)*r)) v = v * 10 + (*r++ - '0');
            }
            if (*r == ';') r++;
            if (v < 0x80) *d++ = (char)v;
            else if (v < 0x800) {
                *d++ = (char)(0xC0 | (v >> 6));
                *d++ = (char)(0x80 | (v & 0x3F));
            } else {
                *d++ = (char)(0xE0 | (v >> 12));
                *d++ = (char)(0x80 | ((v >> 6) & 0x3F));
                *d++ = (char)(0x80 | (v & 0x3F));
            }
        } else {
            *d++ = *r++;
        }
    }
    *d = 0;
}

void trim(char* s) {
    if (!s) return;
    char* a = s;
    while (*a == ' ' || *a == '\n' || *a == '\r' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
}

// `tag` matches the element name at a '<', and the following character ends the name.
bool isTagAt(const char* p, const char* tag) {
    size_t n = strlen(tag);
    if (strncmp(p, tag, n) != 0) return false;
    char c = p[n];
    return c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n';
}

bool findTag(const char* xml, const char* tag, const char** content, const char** contentEnd) {
    if (!xml || !tag) return false;
    const char* p = xml;
    while ((p = strchr(p, '<'))) {
        if (p[1] == '/') {
            p++;
            continue;
        }
        if (isTagAt(p + 1, tag)) {
            const char* gt = strchr(p, '>');
            if (!gt) return false;
            if (gt > p && gt[-1] == '/') {
                *content = gt;
                *contentEnd = gt;
                return true;
            }
            char close[80];
            snprintf(close, sizeof(close), "</%s>", tag);
            const char* end = strstr(gt + 1, close);
            if (!end) return false;
            *content = gt + 1;
            *contentEnd = end;
            return true;
        }
        p++;
    }
    return false;
}

void copyTag(const char* xml, const char* tag, char* dst, size_t cap) {
    dst[0] = 0;
    const char* a = nullptr;
    const char* b = nullptr;
    if (!findTag(xml, tag, &a, &b) || cap == 0) return;
    size_t n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(dst, a, n);
    dst[n] = 0;
    xmlUnescape(dst);
    trim(dst);
}

bool attr(const char* tag, const char* name, char* out, size_t cap) {
    out[0] = 0;
    const char* end = strchr(tag, '>');
    if (!end || cap == 0) return false;
    char key[48];
    snprintf(key, sizeof(key), "%s=\"", name);
    size_t klen = strlen(key);
    for (const char* p = tag; p + klen < end; p++) {
        if (memcmp(p, key, klen) != 0) continue;
        p += klen;
        size_t i = 0;
        while (p < end && *p != '"' && i + 1 < cap) out[i++] = *p++;
        out[i] = 0;
        xmlUnescape(out);
        return true;
    }
    return false;
}

void parseHost(const char* url, char* ip, size_t cap) {
    ip[0] = 0;
    if (!url) return;
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != ':' && p[i] != '/' && i + 1 < cap) {
        ip[i] = p[i];
        i++;
    }
    ip[i] = 0;
}

void el(const char* name, const char* value) {
    w.s("<");
    w.s(name);
    w.s(">");
    w.xml(value ? value : "");
    w.s("</");
    w.s(name);
    w.s(">");
}

void elInt(const char* name, int value) {
    char t[16];
    snprintf(t, sizeof(t), "%d", value);
    el(name, t);
}

class RespSink : public Stream {
   public:
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t n = 0;
    size_t write(const uint8_t* data, size_t size) override {
        size_t room = n < cap ? cap - n : 0;
        size_t take = size < room ? size : room;
        if (take) memcpy(buf + n, data, take);
        n += take;
        return size;
    }
    size_t write(uint8_t c) override { return write(&c, 1); }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

template <typename F>
bool invoke(const char* ip, const char* path, const char* service, const char* action, F fill) {
    if (!soapBuf || !resp || !ip || !strchr(ip, '.')) return false;
    w.reset(soapBuf, SOAP_CAP);
    w.s("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    w.s("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:");
    w.s(action);
    w.s(" xmlns:u=\"");
    w.s(service);
    w.s("\">");
    fill();
    w.s("</u:");
    w.s(action);
    w.s("></s:Body></s:Envelope>");
    if (w.overflow) {
        snprintf(lastErr, sizeof(lastErr), "%s overflow", action);
        Serial.printf("[sonos] %s envelope overflow\n", action);
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400%s", ip, path);
    char hdr[180];
    snprintf(hdr, sizeof(hdr), "\"%s#%s\"", service, action);

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(4000);
    if (!http.begin(client, url)) {
        snprintf(lastErr, sizeof(lastErr), "%s no connect", action);
        return false;
    }
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPACTION", hdr);
    int code = http.POST((uint8_t*)soapBuf, w.n);
    resp[0] = 0;
    if (code > 0) {
        // writeToStream removes HTTP chunked framing. Reading the raw socket
        // left chunk sizes (such as "\r\n556\r\n") inside long favorite URIs.
        RespSink sink;
        sink.buf = (uint8_t*)resp;
        sink.cap = RESP_CAP - 1;
        http.writeToStream(&sink);
        resp[sink.n < sink.cap ? sink.n : sink.cap] = 0;
    }
    http.end();
    if (code != 200 || strstr(resp, "Fault>")) {
        char err[24] = "";
        const char* a = nullptr;
        const char* b = nullptr;
        if (findTag(resp, "errorCode", &a, &b)) {
            size_t n = (size_t)(b - a);
            if (n > sizeof(err) - 1) n = sizeof(err) - 1;
            memcpy(err, a, n);
            err[n] = 0;
        }
        const char* e = strstr(resp, "errorCode");
        if (e) {
            const char* gt = strchr(e, '>');
            if (gt && !err[0]) {
                gt++;
                size_t n = 0;
                while (gt[n] && gt[n] != '<' && n + 1 < sizeof(err)) n++;
                memcpy(err, gt, n);
                err[n] = 0;
                trim(err);
            }
        }
        snprintf(lastErr, sizeof(lastErr), "%s %d%s%s", action, code, err[0] ? " " : "", err);
        Serial.printf("[sonos] %s HTTP %d%s%s\n", action, code, err[0] ? " error " : "", err);
        return false;
    }
    return true;
}

bool avt(const char* ip, const char* action) {
    return invoke(ip, AVT_PATH, AVT, action, [&] {
        el("InstanceID", "0");
        if (!strcmp(action, "Play")) el("Speed", "1");
    });
}

void minimalDidl(const char* uri, const char* title) {
    Xml d;
    d.reset(didlBuf, DIDL_CAP);
    d.s("<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">");
    d.s("<item id=\"0\" parentID=\"-1\" restricted=\"1\"><dc:title>");
    d.xml(title && title[0] ? title : "Favorite");
    d.s("</dc:title><upnp:class>object.item.audioItem</upnp:class><res protocolInfo=\"*:*:*:*\">");
    d.xml(uri ? uri : "");
    d.s("</res></item></DIDL-Lite>");
    if (d.overflow) didlBuf[0] = 0;
}

bool bareUri(const char* uri) {
    return uri && (!strncmp(uri, "x-rincon:", 9) || !strncmp(uri, "x-rincon-queue:", 15) ||
                   !strncmp(uri, "x-rincon-playlist:", 18) || !strncmp(uri, "x-rincon-cpcontainer:", 21));
}

bool setUri(const char* ip, const char* uri, const char* meta) {
    const char* use = meta && meta[0] ? meta : "";
    if (!use[0] && !bareUri(uri)) {
        minimalDidl(uri, "");
        use = didlBuf;
    }
    bool ok = invoke(ip, AVT_PATH, AVT, "SetAVTransportURI", [&] {
        el("InstanceID", "0");
        el("CurrentURI", uri ? uri : "");
        el("CurrentURIMetaData", use);
    });
    if (!ok && use[0] && !bareUri(uri)) {
        ok = invoke(ip, AVT_PATH, AVT, "SetAVTransportURI", [&] {
            el("InstanceID", "0");
            el("CurrentURI", uri ? uri : "");
            el("CurrentURIMetaData", "");
        });
    }
    return ok;
}

bool addQueue(const char* ip, const char* uri, const char* meta) {
    const char* use = meta && meta[0] ? meta : "";
    if (!use[0]) {
        minimalDidl(uri, "");
        use = didlBuf;
    }
    return invoke(ip, AVT_PATH, AVT, "AddURIToQueue", [&] {
        el("InstanceID", "0");
        el("EnqueuedURI", uri ? uri : "");
        el("EnqueuedURIMetaData", use);
        el("DesiredFirstTrackNumberEnqueued", "0");
        el("EnqueueAsNext", "0");
    });
}

Room* coord() {
    for (int i = 0; i < roomCount; i++)
        if (strcmp(rooms[i].uuid, active) == 0) return &rooms[i];
    return nullptr;
}

// Play, pause and the queue belong to the group coordinator. A room that is
// already following another room does not have its own transport.
Room* transportRoom() {
    Room* s = coord();
    if (!s || s->coordinator || !s->group[0]) return s;
    for (int i = 0; i < roomCount; i++)
        if (!strcmp(rooms[i].uuid, s->group)) return &rooms[i];
    return s;
}

void publishRooms() {
    int groupOf[MAX_ROOMS];
    int heads[MAX_ROOMS];
    int nGroups = 0;
    for (int i = 0; i < MAX_ROOMS; i++) groupOf[i] = -1;
    for (int i = 0; i < roomCount; i++) {
        if (groupOf[i] >= 0 || nGroups >= MAX_ROOMS) continue;
        int head = i;
        if (rooms[i].group[0]) {
            for (int j = 0; j < roomCount; j++) {
                if (rooms[j].coordinator && !strcmp(rooms[j].uuid, rooms[i].group)) {
                    head = j;
                    break;
                }
            }
        }
        int g = nGroups++;
        heads[g] = head;
        const char* id = rooms[head].uuid;
        for (int j = 0; j < roomCount; j++) {
            if (j == head || (id[0] && rooms[j].group[0] && !strcmp(rooms[j].group, id)))
                groupOf[j] = g;
        }
    }

    bool gPlay[MAX_ROOMS] = {};
    int gCount[MAX_ROOMS] = {};
    for (int i = 0; i < roomCount; i++) {
        int g = groupOf[i];
        if (g < 0) continue;
        if (!strcmp(rooms[i].uuid, rooms[heads[g]].uuid) && i != heads[g]) continue;
        gCount[g]++;
        if (rooms[i].playing) gPlay[g] = true;
    }

    int gorder[MAX_ROOMS];
    int nOrder = 0;
    bool taken[MAX_ROOMS] = {};
    for (int pass = 0; pass < 2; pass++) {
        bool wantPlaying = pass == 0;
        for (int g = 0; g < nGroups; g++) {
            if (taken[g] || gPlay[g] != wantPlaying) continue;
            taken[g] = true;
            gorder[nOrder++] = g;
        }
    }

    int order[MAX_ROOMS];
    int nOut = 0;
    for (int gi = 0; gi < nOrder; gi++) {
        int g = gorder[gi];
        int head = heads[g];
        if (nOut < MAX_ROOMS) order[nOut++] = head;
        for (int i = 0; i < roomCount && nOut < MAX_ROOMS; i++) {
            if (i == head || groupOf[i] != g) continue;
            if (!strcmp(rooms[i].uuid, rooms[head].uuid)) continue;
            order[nOut++] = i;
        }
    }

    lock();
    published.household = haveHouse;
    published.roomCount = nOut;
    published.groupName[0] = 0;
    for (int out = 0; out < MAX_ROOMS; out++) {
        if (out >= nOut) {
            memset(&published.rooms[out], 0, sizeof(published.rooms[out]));
            continue;
        }
        int i = order[out];
        int g = groupOf[i];
        RoomView& dst = published.rooms[out];
        strlcpy(dst.name, rooms[i].name, sizeof(dst.name));
        strlcpy(dst.uuid, rooms[i].uuid, sizeof(dst.uuid));
        strlcpy(dst.group, rooms[i].group, sizeof(dst.group));
        dst.inGroup = g >= 0 && gCount[g] > 1;
        dst.leader = rooms[i].coordinator;
        dst.playing = rooms[i].playing;
        dst.coordinator = strcmp(rooms[i].uuid, active) == 0;
        if (dst.coordinator)
            strlcpy(published.groupName, rooms[i].name, sizeof(published.groupName));
    }
    unlock();
}

bool parseGroups(char* xml) {
    Room scratch[MAX_ROOMS];
    int n = 0;
    char* p = xml;
    while (p && *p && n < MAX_ROOMS) {
        char* g = strstr(p, "<ZoneGroup ");
        if (!g) break;
        char coordId[40];
        if (!attr(g, "Coordinator", coordId, sizeof(coordId))) {
            p = g + 11;
            continue;
        }
        char* end = strstr(g, "</ZoneGroup>");
        if (!end) break;
        char* m = g;
        while (m < end && n < MAX_ROOMS) {
            char* mem = strstr(m, "<ZoneGroupMember ");
            if (!mem || mem >= end) break;
            Room& r = scratch[n];
            memset(&r, 0, sizeof(r));
            attr(mem, "UUID", r.uuid, sizeof(r.uuid));
            attr(mem, "ZoneName", r.name, sizeof(r.name));
            char loc[160];
            attr(mem, "Location", loc, sizeof(loc));
            parseHost(loc, r.ip, sizeof(r.ip));
            strlcpy(r.group, coordId, sizeof(r.group));
            r.coordinator = strcmp(r.uuid, coordId) == 0;
            if (r.uuid[0] && r.ip[0]) n++;
            char* gt = strchr(mem, '>');
            m = gt ? gt + 1 : mem + 18;
        }
        p = end + 12;
    }
    if (!n) return false;
    memcpy(rooms, scratch, sizeof(Room) * n);
    roomCount = n;
    return true;
}

bool getZone(const char* ip) {
    if (!invoke(ip, ZGT_PATH, ZGT, "GetZoneGroupState", [] {})) return false;
    const char* a = nullptr;
    const char* b = nullptr;
    if (!findTag(resp, "ZoneGroupState", &a, &b)) return false;
    size_t n = (size_t)(b - a);
    if (n >= RESP_CAP) n = RESP_CAP - 1;
    memmove(resp, a, n);
    resp[n] = 0;
    xmlUnescape(resp);
    if (strstr(resp, "&lt;")) xmlUnescape(resp);
    return parseGroups(resp);
}

bool transportState(const char* ip, char* state, size_t cap) {
    state[0] = 0;
    if (!avt(ip, "GetTransportInfo")) return false;
    copyTag(resp, "CurrentTransportState", state, cap);
    return state[0] != 0;
}

bool stoppedState(const char* state) {
    return state && (!strcmp(state, "STOPPED") || !strcmp(state, "NO_MEDIA_PRESENT"));
}

bool transportPlaying(const char* ip, bool* playing) {
    char state[32];
    if (!transportState(ip, state, sizeof(state))) return false;
    *playing = !strcmp(state, "PLAYING") || !strcmp(state, "TRANSITIONING");
    return true;
}

void chooseActive(bool keep) {
    if (keep && active[0]) {
        for (int i = 0; i < roomCount; i++)
            if (!strcmp(rooms[i].uuid, active)) return;
    }
    for (int i = 0; i < roomCount; i++) {
        if (!rooms[i].coordinator) continue;
        bool again = false;
        for (int j = 0; j < i; j++)
            if (rooms[j].coordinator && !strcmp(rooms[j].uuid, rooms[i].uuid)) again = true;
        if (again) continue;
        bool on = false;
        if (transportPlaying(rooms[i].ip, &on) && on) {
            strlcpy(active, rooms[i].uuid, sizeof(active));
            return;
        }
    }
    for (int i = 0; i < roomCount; i++) {
        if (rooms[i].coordinator) {
            strlcpy(active, rooms[i].uuid, sizeof(active));
            return;
        }
    }
    if (roomCount) strlcpy(active, rooms[0].uuid, sizeof(active));
}

void saveIp(const char* ip) {
    Preferences prefs;
    prefs.begin("sonos", false);
    prefs.putString("ip", ip);
    prefs.end();
}

void loadIp(char* ip, size_t cap) {
    Preferences prefs;
    prefs.begin("sonos", false);
    String s = prefs.getString("ip", "");
    prefs.end();
    strlcpy(ip, s.c_str(), cap);
}

bool refreshFrom(const char* ip) {
    if (!getZone(ip)) return false;
    chooseActive(active[0] != 0);
    haveHouse = true;
    publishRooms();
    Room* c = coord();
    saveIp(c && c->ip[0] ? c->ip : ip);
    Serial.printf("[sonos] %d rooms, controlling '%s'\n", roomCount, published.groupName);
    topoFails = 0;
    playFails = 0;
    return true;
}

bool ssdp(char* ip, size_t cap) {
    WiFiUDP udp;
    if (!udp.begin(1900) && !udp.begin(0)) {
        Serial.println("[sonos] SSDP socket failed");
        return false;
    }
    static const char msg[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
        "\r\n";
    uint32_t start = millis();
    uint32_t sent = 0;
    char pkt[768];
    while (millis() - start < 2500) {
        if (millis() - sent > 700) {
            udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
            udp.write((const uint8_t*)msg, sizeof(msg) - 1);
            udp.endPacket();
            sent = millis();
        }
        int n = udp.parsePacket();
        if (n <= 0) {
            delay(20);
            continue;
        }
        int r = udp.read((uint8_t*)pkt, sizeof(pkt) - 1);
        if (r < 0) r = 0;
        pkt[r] = 0;
        const char* line = pkt;
        while (*line) {
            if (strncasecmp(line, "LOCATION:", 9) == 0) {
                const char* u = line + 9;
                while (*u == ' ' || *u == '\t') u++;
                char url[160];
                size_t i = 0;
                while (u[i] && u[i] != '\r' && u[i] != '\n' && u[i] != ' ' && i + 1 < sizeof(url)) {
                    url[i] = u[i];
                    i++;
                }
                url[i] = 0;
                parseHost(url, ip, cap);
                if (ip[0]) {
                    udp.stop();
                    Serial.printf("[sonos] SSDP %s\n", ip);
                    return true;
                }
            }
            const char* nl = strchr(line, '\n');
            if (!nl) break;
            line = nl + 1;
        }
    }
    udp.stop();
    return false;
}

bool discover() {
    char ip[16] = "";
    loadIp(ip, sizeof(ip));
    if (ip[0] && refreshFrom(ip)) return true;
    active[0] = 0;
    if (ssdp(ip, sizeof(ip)) && refreshFrom(ip)) return true;
    Serial.println("[sonos] no speakers yet");
    return false;
}

void urlEncode(const char* in, char* out, size_t cap) {
    static const char* hex = "0123456789ABCDEF";
    size_t o = 0;
    if (!in) in = "";
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap; p++) {
        unsigned char c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                     c == '_' || c == '.' || c == '~';
        if (plain) {
            out[o++] = (char)c;
        } else if (o + 3 < cap) {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        } else {
            break;
        }
    }
    out[o] = 0;
}

// Prefer the speaker's own /getaa image. An https artwork URL is fetched by the
// speaker, which is the one that can speak TLS, and returned to us as plain HTTP.
void absoluteArt(const char* uri, const char* ip, char* out, size_t cap) {
    out[0] = 0;
    if (!uri || !uri[0] || !ip || !ip[0] || cap < 32) return;
    if (uri[0] == '/') {
        snprintf(out, cap, "http://%s:1400%s", ip, uri);
        return;
    }
    if (!strncmp(uri, "http://", 7)) {
        strlcpy(out, uri, cap);
        return;
    }
    int n = snprintf(out, cap, "http://%s:1400/getaa?s=1&u=", ip);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return;
    }
    urlEncode(uri, out + n, cap - (size_t)n);
}

int artRank(const char* uri) {
    if (!uri || !uri[0]) return 0;
    if (uri[0] == '/' || strstr(uri, "/getaa")) return 3;
    if (!strncmp(uri, "http://", 7)) return 2;
    if (!strncmp(uri, "https://", 8)) return 1;
    return 0;
}

void pickArt(const char* xml, char* dst, size_t cap) {
    dst[0] = 0;
    int best = 0;
    const char* p = xml;
    while (p && *p && best < 3) {
        const char* tag = strstr(p, "<upnp:albumArtURI");
        const char* name = "upnp:albumArtURI";
        const char* plain = strstr(p, "<albumArtURI");
        if (!tag || (plain && plain < tag)) {
            tag = plain;
            name = "albumArtURI";
        }
        if (!tag) break;
        const char* a = nullptr;
        const char* b = nullptr;
        if (!findTag(tag, name, &a, &b)) {
            p = tag + 1;
            continue;
        }
        char uri[1400];
        size_t n = (size_t)(b - a);
        if (n >= sizeof(uri)) n = sizeof(uri) - 1;
        memcpy(uri, a, n);
        uri[n] = 0;
        xmlUnescape(uri);
        trim(uri);
        int rank = artRank(uri);
        if (rank > best) {
            strlcpy(dst, uri, cap);
            best = rank;
        }
        p = b;
    }
}

void publishArt(const uint8_t* jpeg, size_t n) {
    if (!art[0] || !art[1]) return;
    int back = 1 - artSlot;
    if (n > ART_CAP) n = 0;
    if (n && jpeg != art[back]) memcpy(art[back], jpeg, n);
    artLen[back] = n;
    lock();
    artSlot = back;
    published.artGen++;
    unlock();
}

class ArtStream : public Stream {
   public:
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t n = 0;
    size_t write(const uint8_t* data, size_t size) override {
        size_t room = n < cap ? cap - n : 0;
        size_t take = size < room ? size : room;
        if (take) memcpy(buf + n, data, take);
        n += take;
        return size;
    }
    size_t write(uint8_t c) override { return write(&c, 1); }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

void considerArt(const char* uri, const char* ip) {
    absoluteArt(uri, ip, fetchUrl, sizeof(fetchUrl));
    const char* url = fetchUrl;
    if (!url[0]) {
        if (lastArtUrl[0]) {
            lastArtUrl[0] = 0;
            pendingArt[0] = 0;
            publishArt(nullptr, 0);
        }
        return;
    }
    if (!strcmp(url, lastArtUrl)) return;
    if (!strcmp(url, pendingArt) && millis() < artRetryAt) return;
    strlcpy(pendingArt, url, sizeof(pendingArt));
    if (!art[0] || !art[1]) return;

    int back = 1 - artSlot;
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(4000);
    if (!http.begin(client, url)) {
        artRetryAt = millis() + 15000;
        return;
    }
    int code = http.GET();
    ArtStream out;
    out.buf = art[back];
    out.cap = ART_CAP;
    int written = code == 200 ? http.writeToStream(&out) : -1;
    size_t got = out.n;
    bool jpeg = got >= 2 && art[back][0] == 0xFF && art[back][1] == 0xD8;
    bool png = got >= 8 && art[back][0] == 0x89 && art[back][1] == 'P' && art[back][2] == 'N' && art[back][3] == 'G';
    bool ok = code == 200 && written > 0 && (jpeg || png);
    http.end();
    if (!ok) {
        artRetryAt = millis() + 15000;
        unsigned m0 = got ? art[back][0] : 0;
        unsigned m1 = got > 1 ? art[back][1] : 0;
        Serial.printf("[sonos] art HTTP %d wrote %d kept %u magic %02x %02x\n", code, written, (unsigned)got, m0, m1);
        return;
    }
    artLen[back] = got;
    lock();
    artSlot = back;
    published.artGen++;
    unlock();
    strlcpy(lastArtUrl, url, sizeof(lastArtUrl));
    pendingArt[0] = 0;
    Serial.printf("[sonos] art %u bytes\n", (unsigned)got);
}

bool hasWord(const char* s, const char* word) {
    if (!s || !word || !word[0]) return false;
    size_t n = strlen(word);
    for (const char* p = s; *p; p++)
        if (!strncasecmp(p, word, n)) return true;
    return false;
}

void classifySource(const char* uri, char* dst, size_t cap) {
    dst[0] = 0;
    if (!uri || !uri[0] || cap == 0) return;
    const char* name = "Sonos";
    if (hasWord(uri, "spotify")) name = "Spotify";
    else if (hasWord(uri, "dzrs.trk") || hasWord(uri, "x-sonosapi-radio:sonos")) name = "Sonos Radio";
    else if (hasWord(uri, "deezer")) name = "Deezer";
    else if (hasWord(uri, "soundcloud")) name = "SoundCloud";
    else if (hasWord(uri, "tunein") || hasWord(uri, "x-sonosapi-stream") || hasWord(uri, "x-sonosapi-radio") ||
             hasWord(uri, "x-rincon-mp3radio"))
        name = "Radio";
    else if (hasWord(uri, "x-sonos-htastream") || hasWord(uri, "spdif"))
        name = "TV";
    else if (hasWord(uri, "x-rincon-stream"))
        name = "Line-in";
    else if (hasWord(uri, "airplay"))
        name = "AirPlay";
    else if (hasWord(uri, "x-file-cifs") || hasWord(uri, "x-rincon-playlist"))
        name = "Library";
    strlcpy(dst, name, cap);
}

void copyObjectId(const char* from, char* id, size_t cap) {
    size_t i = 0;
    if (!from) from = "";
    while (from[i] && from[i] != '<' && from[i] != '"' && from[i] != ' ' && from[i] != '&' && from[i] != '?' &&
           i + 1 < cap) {
        id[i] = from[i];
        i++;
    }
    id[i] = 0;
}

const char* findCi(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return nullptr;
    size_t n = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (!strncasecmp(p, needle, n)) return p;
    return nullptr;
}

bool hexPrefix(const char* p) {
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)p[i])) return false;
    return true;
}

const char* skipSpotifySep(const char* p) {
    if (!strncasecmp(p, "%253a", 5)) return p + 5;
    if (!strncasecmp(p, "%3a", 3)) return p + 3;
    if (*p == ':' || *p == '/') return p + 1;
    return p;
}

bool readSpotifyKey(const char* p, char* id, size_t cap) {
    size_t i = 0;
    while (p[i] && i + 1 < cap && isalnum((unsigned char)p[i])) {
        id[i] = p[i];
        i++;
    }
    id[i] = 0;
    return i >= 4;
}

// Spotify on Sonos plays an album or a playlist directly. The local queue is
// often empty, and the list id is only inside the track URI or its metadata.
bool spotifyListId(const char* hay, char* out, size_t cap) {
    if (!hay || !out || cap < 32) return false;
    const char* kinds[] = {"artistRadio", "songRadio", "trackRadio", "playlistRadio", "albumRadio", "playlist", "album",
                            "collection"};
    const char* prefixes[] = {"100c206c", "100c206c", "100c206c", "100c206c", "100c206c", "1006206c", "1004206c",
                               "1006206c"};
    for (int k = 0; k < 8; k++) {
        const char* p = hay;
        size_t kindLen = strlen(kinds[k]);
        while ((p = findCi(p, "spotify"))) {
            const char* q = p + 7;
            for (int hop = 0; hop < 4; hop++) {
                const char* n = skipSpotifySep(q);
                if (n == q) break;
                q = n;
                if (!strncasecmp(q, "spotify", 7)) q += 7;
                else break;
            }
            if (!strncasecmp(q, kinds[k], kindLen)) {
                const char* idAt = skipSpotifySep(q + kindLen);
                char sid[48];
                if (readSpotifyKey(idAt, sid, sizeof(sid))) {
                    if (p >= hay + 8 && hexPrefix(p - 8)) {
                        copyObjectId(p - 8, out, cap);
                        if (hasWord(out, kinds[k]) && !hasWord(out, "track")) return true;
                    }
                    snprintf(out, cap, "%sspotify%%3a%s%%3a%s", prefixes[k], kinds[k], sid);
                    return true;
                }
            }
            p += 7;
        }
    }
    return false;
}

void considerContext(const char* meta, const char* uri, char* id, size_t idCap, char* name, size_t nameCap) {
    char found[240];
    found[0] = 0;
    if (meta) spotifyListId(meta, found, sizeof(found));
    if (uri && !hasWord(found, "playlist")) {
        char fromUri[240];
        fromUri[0] = 0;
        if (spotifyListId(uri, fromUri, sizeof(fromUri)) && (hasWord(fromUri, "playlist") || !found[0]))
            strlcpy(found, fromUri, sizeof(found));
    }
    if (found[0] && (!id[0] || (hasWord(found, "playlist") && !hasWord(id, "playlist"))))
        strlcpy(id, found, idCap);
    if (!id[0]) {
        const char* via = nullptr;
        if (uri && !strncmp(uri, "x-rincon-cpcontainer:", 21)) via = uri;
        else if (meta) via = strstr(meta, "x-rincon-cpcontainer:");
        if (via) copyObjectId(via + 21, id, idCap);
    }
    if (!id[0] && meta) {
        const char* parent = strstr(meta, "parentID=\"");
        if (parent) {
            char tmp[240];
            copyObjectId(parent + 10, tmp, sizeof(tmp));
            if (tmp[0] && strcmp(tmp, "Q:0") && strcmp(tmp, "-1") &&
                (hasWord(tmp, "playlist") || hasWord(tmp, "album") || hasWord(tmp, "collection")) &&
                !hasWord(tmp, "track"))
                strlcpy(id, tmp, idCap);
        }
    }
    if (name && nameCap && !name[0] && meta && strstr(meta, "object.container"))
        copyTag(meta, "dc:title", name, nameCap);
}

bool rawLabel(const char* s) {
    if (!s || !s[0]) return false;
    if (strchr(s, '?') || strstr(s, "://") || strstr(s, ".mp3") || strstr(s, ".aac") || strstr(s, ".m3u8") ||
        strstr(s, "aw_0_"))
        return true;
    int marks = 0;
    for (const char* p = s; *p; p++)
        if (*p == '%') marks++;
    return marks >= 2;
}

void copyStream(const char* xml, char* dst, size_t cap) {
    dst[0] = 0;
    if (!xml || cap == 0) return;
    const char* sc = strstr(xml, "streamContent>");
    if (!sc) return;
    sc += 14;
    const char* end = strchr(sc, '<');
    if (!end || end <= sc) return;
    size_t n = (size_t)(end - sc);
    if (n >= cap) n = cap - 1;
    memcpy(dst, sc, n);
    dst[n] = 0;
    xmlUnescape(dst);
    trim(dst);
}

bool radioUri(const char* uri) {
    return uri && (hasWord(uri, "x-sonosapi-stream") || hasWord(uri, "x-rincon-mp3radio") || hasWord(uri, "tunein") ||
                   hasWord(uri, "x-sonosapi-radio:sonos"));
}

bool getPosition(const char* ip) {
    if (!avt(ip, "GetPositionInfo")) return false;
    char rel[16], dur[16], title[100], artist[100], album[100], trackUri[400], source[16];
    char trackNo[8];
    copyTag(resp, "Track", trackNo, sizeof(trackNo));
    copyTag(resp, "RelTime", rel, sizeof(rel));
    copyTag(resp, "TrackDuration", dur, sizeof(dur));
    copyTag(resp, "TrackURI", trackUri, sizeof(trackUri));
    title[0] = artist[0] = album[0] = 0;
    pickedArt[0] = 0;
    const char* a = nullptr;
    const char* b = nullptr;
    if (findTag(resp, "TrackMetaData", &a, &b) && trackBuf) {
        size_t n = (size_t)(b - a);
        if (n >= TRACK_CAP) n = TRACK_CAP - 1;
        memcpy(trackBuf, a, n);
        trackBuf[n] = 0;
        xmlUnescape(trackBuf);
        if (strstr(trackBuf, "&lt;")) xmlUnescape(trackBuf);
        copyTag(trackBuf, "dc:title", title, sizeof(title));
        copyTag(trackBuf, "dc:creator", artist, sizeof(artist));
        copyTag(trackBuf, "upnp:album", album, sizeof(album));
        char program[100];
        copyStream(trackBuf, program, sizeof(program));
        if (rawLabel(title)) {
            if (lastStation[0] && !rawLabel(lastStation)) strlcpy(title, lastStation, sizeof(title));
            else if (program[0] && !rawLabel(program)) strlcpy(title, program, sizeof(title));
        }
        if (!artist[0] && program[0] && !rawLabel(program) && strcmp(program, title) != 0)
            strlcpy(artist, program, sizeof(artist));
        pickArt(trackBuf, pickedArt, sizeof(pickedArt));
    }
    classifySource(trackUri[0] ? trackUri : pickedArt, source, sizeof(source));
    if (directTitle[0] && strstr(trackUri, ":8080/yt/")) {
        strlcpy(title, directTitle, sizeof(title));
        strlcpy(artist, directArtist, sizeof(artist));
        album[0] = 0;
        strlcpy(source, "YouTube", sizeof(source));
        strlcpy(pickedArt, directArt, sizeof(pickedArt));
    }
    if (strcmp(title, lastTitle) != 0) {
        Serial.printf("[sonos] now '%s' / '%s' art %u\n", title, artist, (unsigned)strlen(pickedArt));
        if (trackUri[0]) Serial.printf("[sonos] uri %.220s\n", trackUri);
        if (trackBuf) {
            const char* parent = strstr(trackBuf, "parentID=\"");
            if (parent) {
                char id[140];
                copyObjectId(parent + 10, id, sizeof(id));
                Serial.printf("[sonos] parent %s\n", id);
            }
        }
        if (trackUri[0]) Serial.printf("[sonos] source %s\n", source[0] ? source : "Sonos");
        strlcpy(lastTitle, title, sizeof(lastTitle));
    }
    if (trackUri[0] && strstr(trackUri, "spotify")) {
        const char* sn = strstr(trackUri, "sn=");
        int n = sn ? atoi(sn + 3) : 0;
        if (n > 0) spotifySn = n;
    }
    const char* shownSource = title[0] ? source : "";
    if (strcmp(shownSource, published.source) != 0 && (shownSource[0] || published.source[0])) {
        lock();
        published.listGen++;
        unlock();
    }
    lock();
    strlcpy(published.title, title, sizeof(published.title));
    strlcpy(published.artist, artist, sizeof(published.artist));
    strlcpy(published.album, album, sizeof(published.album));
    strlcpy(published.source, title[0] ? source : "", sizeof(published.source));
    published.track = title[0] ? atoi(trackNo) : 0;
    strlcpy(published.rel, rel, sizeof(published.rel));
    strlcpy(published.dur, dur, sizeof(published.dur));
    unlock();
    considerArt(pickedArt, ip);
    return true;
}

bool readVolume(const char* ip, int* vol) {
    bool ok = invoke(ip, RC_PATH, RC, "GetVolume", [] {
        el("InstanceID", "0");
        el("Channel", "Master");
    });
    if (!ok) return false;
    char num[8];
    copyTag(resp, "CurrentVolume", num, sizeof(num));
    if (!num[0]) return false;
    int v = atoi(num);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    *vol = v;
    return true;
}

bool writeVolume(const char* ip, int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return invoke(ip, RC_PATH, RC, "SetVolume", [&] {
        el("InstanceID", "0");
        el("Channel", "Master");
        elInt("DesiredVolume", vol);
    });
}

void applyPlaying(const char* groupUuid, bool on) {
    if (!groupUuid || !groupUuid[0]) return;
    for (int i = 0; i < roomCount; i++)
        if (!strcmp(rooms[i].group, groupUuid)) rooms[i].playing = on;
}

void refreshWhosPlaying() {
    for (int i = 0; i < roomCount; i++) {
        if (!rooms[i].coordinator) continue;
        bool seen = false;
        for (int j = 0; j < i; j++)
            if (rooms[j].coordinator && !strcmp(rooms[j].uuid, rooms[i].uuid)) seen = true;
        if (seen) continue;
        bool on = false;
        if (!transportPlaying(rooms[i].ip, &on)) continue;
        applyPlaying(rooms[i].uuid, on);
    }
    publishRooms();
}

void clearFavs();
void publishFavs();

void clearNowPlaying() {
    bool had = lastTitle[0] || published.title[0] || published.source[0] || published.queueName[0];
    lastTitle[0] = 0;
    lastStation[0] = 0;
    lastMediaUri[0] = 0;
    lastMediaCount[0] = 0;
    lastArtUrl[0] = 0;
    pendingArt[0] = 0;
    publishArt(nullptr, 0);
    lock();
    published.title[0] = 0;
    published.artist[0] = 0;
    published.album[0] = 0;
    published.rel[0] = 0;
    published.dur[0] = 0;
    published.playing = false;
    published.source[0] = 0;
    published.queueName[0] = 0;
    published.track = 0;
    if (had) published.listGen++;
    unlock();
}

bool refreshPlayback() {
    Room* c = transportRoom();
    Room* volRoom = coord();
    if (!c) return false;
    char state[32];
    bool gotState = transportState(c->ip, state, sizeof(state));
    if (gotState && stoppedState(state)) {
        if (published.title[0] || published.source[0] || lastTitle[0]) {
            Serial.printf("[sonos] %s stopped\n", c->name);
            clearNowPlaying();
            if (listIsQueue) {
                clearFavs();
                publishFavs();
            }
        }
        applyPlaying(c->coordinator ? c->uuid : c->group, false);
        publishRooms();
        int vol = -1;
        bool gotVol = volRoom && readVolume(volRoom->ip, &vol);
        lock();
        published.playing = false;
        if (gotVol && millis() >= volumeHoldUntil) published.volume = vol;
        unlock();
        return true;
    }
    if (!getPosition(c->ip)) return false;
    static uint32_t mediaAt = 0;
    if (millis() - mediaAt > 2000 && avt(c->ip, "GetMediaInfo")) {
        mediaAt = millis();
        char media[320];
        char tracks[8];
        copyTag(resp, "CurrentURI", media, sizeof(media));
        copyTag(resp, "NrTracks", tracks, sizeof(tracks));
        bool uriChanged = media[0] && strcmp(media, lastMediaUri) != 0;
        bool countChanged = tracks[0] && strcmp(tracks, lastMediaCount) != 0;
        if (uriChanged || countChanged) {
            bool known = lastMediaUri[0] != 0;
            if (media[0]) strlcpy(lastMediaUri, media, sizeof(lastMediaUri));
            strlcpy(lastMediaCount, tracks, sizeof(lastMediaCount));
            Serial.printf("[sonos] playing from %.140s tracks %s\n", lastMediaUri, tracks[0] ? tracks : "?");
            if (known) {
                lock();
                published.listGen++;
                unlock();
            }
        }
        if (radioUri(media) && trackBuf) {
            const char* ma = nullptr;
            const char* mb = nullptr;
            char station[100];
            station[0] = 0;
            if (findTag(resp, "CurrentURIMetaData", &ma, &mb) && mb > ma) {
                size_t n = (size_t)(mb - ma);
                if (n >= TRACK_CAP) n = TRACK_CAP - 1;
                memcpy(trackBuf, ma, n);
                trackBuf[n] = 0;
                xmlUnescape(trackBuf);
                if (strstr(trackBuf, "&lt;")) xmlUnescape(trackBuf);
                copyTag(trackBuf, "dc:title", station, sizeof(station));
            }
            if (station[0] && !rawLabel(station)) {
                strlcpy(lastStation, station, sizeof(lastStation));
                if (rawLabel(published.title)) {
                    lock();
                    strlcpy(published.title, station, sizeof(published.title));
                    unlock();
                    strlcpy(lastTitle, station, sizeof(lastTitle));
                }
            }
        } else if (media[0]) {
            lastStation[0] = 0;
        }
    }
    bool playing = published.playing;
    if (gotState) playing = !strcmp(state, "PLAYING") || !strcmp(state, "TRANSITIONING");
    if (gotState) {
        applyPlaying(c->coordinator ? c->uuid : c->group, playing);
        publishRooms();
    }
    int vol = -1;
    bool gotVol = volRoom && readVolume(volRoom->ip, &vol);
    lock();
    if (gotState) published.playing = playing;
    if (gotVol && millis() >= volumeHoldUntil) published.volume = vol;
    unlock();
    return true;
}

void clearFavs() {
    for (int i = 0; i < favCount; i++) {
        if (favs[i].meta) heap_caps_free(favs[i].meta);
        favs[i].meta = nullptr;
    }
    favCount = 0;
}

bool localNameIs(const char* name, const char* local) {
    const char* colon = nullptr;
    const char* q = name;
    while (*q && *q != '>' && *q != ' ' && *q != '/' && *q != '\t' && *q != '\r' && *q != '\n') {
        if (*q == ':') colon = q;
        q++;
    }
    return isTagAt(colon ? colon + 1 : name, local);
}

// Closing tag of the element that starts at `open` ('<'), counting nested copies of the same name.
char* elementEnd(char* open, const char* local) {
    int depth = 0;
    char* p = open;
    while (p && *p) {
        char* lt = strchr(p, '<');
        if (!lt) return nullptr;
        if (lt[1] == '!' || lt[1] == '?') {
            p = lt + 2;
            continue;
        }
        if (lt[1] == '/') {
            if (localNameIs(lt + 2, local)) {
                depth--;
                if (depth == 0) return lt;
            }
        } else if (localNameIs(lt + 1, local)) {
            char* gt = strchr(lt, '>');
            if (!(gt && gt > lt && gt[-1] == '/')) depth++;
        }
        p = lt + 1;
    }
    return nullptr;
}

// Content of a direct child of `item`. Ignores the same tag nested inside metadata.
bool directContent(const char* item, const char* local, const char** begin, const char** end) {
    int depth = 0;
    const char* p = item;
    while (p && *p) {
        const char* lt = strchr(p, '<');
        if (!lt) return false;
        if (lt[1] == '!' || lt[1] == '?') {
            p = lt + 2;
            continue;
        }
        if (lt[1] == '/') {
            if (depth > 0) depth--;
            p = lt + 2;
            continue;
        }
        const char* gt = strchr(lt, '>');
        if (!gt) return false;
        bool selfClose = gt > lt && gt[-1] == '/';
        if (!selfClose) depth++;
        if (depth == 2 && localNameIs(lt + 1, local) && !selfClose) {
            const char* content = gt + 1;
            int inner = 1;
            const char* q = content;
            while (q && *q) {
                const char* n = strchr(q, '<');
                if (!n) return false;
                if (n[1] == '/') {
                    if (localNameIs(n + 2, local)) {
                        inner--;
                        if (inner == 0) {
                            *begin = content;
                            *end = n;
                            return true;
                        }
                    }
                } else if (n[1] != '!' && n[1] != '?' && localNameIs(n + 1, local)) {
                    const char* g2 = strchr(n, '>');
                    if (!(g2 && g2 > n && g2[-1] == '/')) inner++;
                }
                q = n + 1;
            }
            return false;
        }
        p = gt + 1;
    }
    return false;
}

void takeFavorite(char* item, bool elementIsContainer) {
    if (favCount >= MAX_FAVS) return;
    Fav& f = favs[favCount];
    memset(&f, 0, sizeof(f));
    copyTag(item, "dc:title", f.title, sizeof(f.title));
    const char* rs = nullptr;
    const char* re = nullptr;
    if (!directContent(item, "res", &rs, &re)) findTag(item, "res", &rs, &re);
    if (rs && re && re > rs) {
        size_t n = (size_t)(re - rs);
        if (n >= sizeof(f.uri)) n = sizeof(f.uri) - 1;
        memcpy(f.uri, rs, n);
        f.uri[n] = 0;
        xmlUnescape(f.uri);
        trim(f.uri);
    }
    if (!f.uri[0]) {
        if (!listIsQueue) return;
        strlcpy(f.uri, "queue", sizeof(f.uri));
    }
    if (!f.title[0]) strlcpy(f.title, "Favorite", sizeof(f.title));
    char cls[80];
    copyTag(item, "upnp:class", cls, sizeof(cls));
    f.container = elementIsContainer || (cls[0] && strstr(cls, "object.container")) ||
                  !strncmp(f.uri, "x-rincon-cpcontainer:", 21) || !strncmp(f.uri, "x-rincon-playlist:", 18);
    const char* a = nullptr;
    const char* b = nullptr;
    if (directContent(item, "resMD", &a, &b) || findTag(item, "r:resMD", &a, &b)) {
        size_t n = (size_t)(b - a);
        if (n > META_CAP) n = META_CAP;
        char* m = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (m) {
            memcpy(m, a, n);
            m[n] = 0;
            xmlUnescape(m);
            if (strstr(m, "&lt;")) xmlUnescape(m);
            trim(m);
            f.meta = m;
        }
    }
    favCount++;
}

void publishFavs() {
    lock();
    published.loadingFavs = false;
    published.queue = listIsQueue;
    published.favCount = favCount;
    for (int i = 0; i < MAX_FAVS; i++) {
        if (i < favCount) strlcpy(published.favs[i], favs[i].title, sizeof(published.favs[i]));
        else published.favs[i][0] = 0;
    }
    unlock();
}

void ingestCurrentResult() {
    const char* a = nullptr;
    const char* b = nullptr;
    if (!findTag(resp, "Result", &a, &b)) return;
    size_t n = (size_t)(b - a);
    if (n >= RESP_CAP) n = RESP_CAP - 1;
    memmove(resp, a, n);
    resp[n] = 0;
    xmlUnescape(resp);
    if (strstr(resp, "&lt;")) xmlUnescape(resp);

    char* p = resp;
    while (favCount < MAX_FAVS && p && *p) {
        char* item = nullptr;
        bool isC = false;
        for (char* s = p; (s = strchr(s, '<')); s++) {
            const char* name = s + 1;
            const char* colon = nullptr;
            const char* q = name;
            while (*q && *q != '>' && *q != ' ' && *q != '/' && *q != '\t') {
                if (*q == ':') colon = q;
                q++;
            }
            const char* tag = colon ? colon + 1 : name;
            if (isTagAt(tag, "item")) {
                item = s;
                break;
            }
            if (isTagAt(tag, "container")) {
                item = s;
                isC = true;
                break;
            }
        }
        if (!item) break;
        char* close = elementEnd(item, isC ? "container" : "item");
        if (!close || *close != '<') break;
        char* gt = strchr(close, '>');
        if (!gt) break;
        char saved = *close;
        *close = 0;
        takeFavorite(item, isC);
        *close = saved;
        p = gt + 1;
    }
    if (listIsQueue && favCount == 0 && n > 40)
        Serial.printf("[sonos] queue browse returned %u bytes and no tracks\n", (unsigned)n);
}

void loadList(const char* objectId, bool queue, bool announce = true) {
    Room* c = queue ? transportRoom() : coord();
    browseId = objectId;
    listIsQueue = queue;
    lock();
    published.loadingFavs = true;
    published.queue = queue;
    published.favCount = 0;
    unlock();
    clearFavs();
    if (!c) {
        publishFavs();
        setStatus("No room selected");
        return;
    }
    bool ok = invoke(c->ip, CD_PATH, CD, "Browse", [] {
        el("ObjectID", browseId);
        el("BrowseFlag", "BrowseDirectChildren");
        el("Filter", "*");
        el("StartingIndex", "0");
        el("RequestedCount", "40");
        el("SortCriteria", "");
    });
    if (!ok) {
        publishFavs();
        if (announce) setStatus(queue ? "Could not load the queue" : "Could not load favorites");
        return;
    }
    const char* resultAt = nullptr;
    const char* resultEnd = nullptr;
    if (!findTag(resp, "Result", &resultAt, &resultEnd)) {
        publishFavs();
        if (announce) setStatus(queue ? "No queue returned" : "No favorites returned");
        return;
    }
    ingestCurrentResult();
    Serial.printf("[sonos] %d %s\n", favCount, listIsQueue ? "queue tracks" : "favorites");
    if (!listIsQueue)
        for (int i = 0; i < favCount; i++)
            Serial.printf("[fav %d] '%s' %s meta %u uri %.150s\n", i, favs[i].title, favs[i].container ? "C" : "T",
                          favs[i].meta ? (unsigned)strlen(favs[i].meta) : 0, favs[i].uri);
    publishFavs();
}

bool favsContain(const char* title) {
    if (!title || !title[0]) return false;
    for (int i = 0; i < favCount; i++)
        if (!strcmp(favs[i].title, title)) return true;
    return false;
}

void loadQueue() {
    Room* c = transportRoom();
    char uri[320];
    char trackUri[400];
    char nowTitle[100];
    char objectId[240];
    char playlist[72];
    uri[0] = trackUri[0] = nowTitle[0] = objectId[0] = playlist[0] = 0;
    char state[32];
    if (c && transportState(c->ip, state, sizeof(state)) && stoppedState(state)) {
        Serial.printf("[sonos] %s has stopped\n", c->name);
        listIsQueue = true;
        clearNowPlaying();
        clearFavs();
        publishFavs();
        return;
    }
    if (c && avt(c->ip, "GetMediaInfo")) {
        copyTag(resp, "CurrentURI", uri, sizeof(uri));
        char tracks[8];
        copyTag(resp, "NrTracks", tracks, sizeof(tracks));
        Serial.printf("[sonos] media tracks %s uri %.180s\n", tracks[0] ? tracks : "?", uri);
        if (uri[0]) strlcpy(lastMediaUri, uri, sizeof(lastMediaUri));
        if (tracks[0]) strlcpy(lastMediaCount, tracks, sizeof(lastMediaCount));
        const char* a = nullptr;
        const char* b = nullptr;
        if (findTag(resp, "CurrentURIMetaData", &a, &b) && trackBuf) {
            size_t n = (size_t)(b - a);
            if (n >= TRACK_CAP) n = TRACK_CAP - 1;
            memcpy(trackBuf, a, n);
            trackBuf[n] = 0;
            xmlUnescape(trackBuf);
            if (strstr(trackBuf, "&lt;")) xmlUnescape(trackBuf);
            considerContext(trackBuf, uri, objectId, sizeof(objectId), playlist, sizeof(playlist));
        } else {
            considerContext(nullptr, uri, objectId, sizeof(objectId), playlist, sizeof(playlist));
        }
        if (avt(c->ip, "GetPositionInfo")) {
            copyTag(resp, "TrackURI", trackUri, sizeof(trackUri));
            const char* a = nullptr;
            const char* b = nullptr;
            if (findTag(resp, "TrackMetaData", &a, &b) && trackBuf) {
                size_t n = (size_t)(b - a);
                if (n >= TRACK_CAP) n = TRACK_CAP - 1;
                memcpy(trackBuf, a, n);
                trackBuf[n] = 0;
                xmlUnescape(trackBuf);
                if (strstr(trackBuf, "&lt;")) xmlUnescape(trackBuf);
                copyTag(trackBuf, "dc:title", nowTitle, sizeof(nowTitle));
                considerContext(trackBuf, trackUri, objectId, sizeof(objectId), playlist, sizeof(playlist));
            } else {
                considerContext(nullptr, trackUri, objectId, sizeof(objectId), playlist, sizeof(playlist));
            }
        }
    }
    bool fromQueue = !strncmp(uri, "x-rincon-queue:", 15);
    bool radio = hasWord(uri, "x-sonosapi-radio") || hasWord(uri, "artistRadio") || hasWord(uri, "x-sonosapi-stream") ||
                 hasWord(uri, "x-rincon-mp3radio");
    bool spotifyRadio = hasWord(uri, "artistRadio") || hasWord(uri, "songRadio") || hasWord(uri, "trackRadio") ||
                        hasWord(uri, "playlistRadio") || hasWord(uri, "albumRadio") || hasWord(trackUri, "artistRadio") ||
                        hasWord(trackUri, "songRadio") || hasWord(trackUri, "trackRadio") ||
                        (hasWord(uri, "spotify") && hasWord(uri, "x-sonosapi-radio"));
    if (radio && !spotifyRadio) {
        objectId[0] = 0;
        if (hasWord(uri, "x-sonosapi-radio:sonos") || hasWord(trackUri, "dzrs.trk"))
            strlcpy(playlist, "Sonos Radio", sizeof(playlist));
        else if (!playlist[0]) strlcpy(playlist, "Radio", sizeof(playlist));
    } else if (spotifyRadio) {
        objectId[0] = 0;
        if (!spotifyListId(uri, objectId, sizeof(objectId))) spotifyListId(trackUri, objectId, sizeof(objectId));
        strlcpy(playlist, "Spotify radio", sizeof(playlist));
    } else if (!playlist[0] && objectId[0] && hasWord(objectId, "spotify")) {
        strlcpy(playlist, hasWord(objectId, "album") ? "Album" : "Spotify playlist", sizeof(playlist));
    }
    if (objectId[0]) Serial.printf("[sonos] context %.140s\n", objectId);
    lock();
    strlcpy(published.queueName, playlist, sizeof(published.queueName));
    unlock();
    localQueue = true;
    activeListId[0] = 0;
    if (radio && !spotifyRadio && !fromQueue) {
        listIsQueue = true;
        clearFavs();
        publishFavs();
        Serial.printf("[sonos] %s is a station, not the saved queue\n", playlist);
        return;
    }
    if (spotifyRadio) {
        listIsQueue = true;
        localQueue = true;
        activeListId[0] = 0;
        if (objectId[0]) {
            Serial.printf("[sonos] browse radio %.120s\n", objectId);
            loadList(objectId, true, false);
        } else {
            clearFavs();
            publishFavs();
        }
        if (favCount > 0 && lastListHead[0] && !strcmp(favs[0].title, lastListHead)) {
            clearFavs();
            publishFavs();
        }
        if (favCount == 0) {
            loadList("Q:0", true, false);
            if (favCount > 0 && lastListHead[0] && !strcmp(favs[0].title, lastListHead)) {
                Serial.printf("[sonos] radio ignored leftover '%s'\n", lastListHead);
                clearFavs();
                publishFavs();
            }
        }
        if (favCount > 0) strlcpy(lastListHead, favs[0].title, sizeof(lastListHead));
        else Serial.printf("[sonos] no radio tracks for %.160s\n", uri[0] ? uri : "(no uri)");
        return;
    }
    loadList("Q:0", true);
    if (!fromQueue && nowTitle[0] && favCount > 0 && !favsContain(nowTitle)) {
        Serial.printf("[sonos] hiding leftover queue, now '%s'\n", nowTitle);
        clearFavs();
        publishFavs();
    }
    if (favCount == 0 && objectId[0] && strcmp(objectId, "Q:0") != 0) {
        Serial.printf("[sonos] browse playlist %.120s\n", objectId);
        localQueue = false;
        strlcpy(activeListId, objectId, sizeof(activeListId));
        lock();
        strlcpy(published.queueName, playlist, sizeof(published.queueName));
        unlock();
        loadList(objectId, true);
    }
    if (favCount > 0) strlcpy(lastListHead, favs[0].title, sizeof(lastListHead));
    else Serial.printf("[sonos] no track list for %.160s\n", uri[0] ? uri : "(no uri)");
}

void copyToken(const char* from, char* out, size_t cap) {
    size_t i = 0;
    if (!from) from = "";
    while (from[i] && from[i] != '<' && from[i] != '"' && from[i] != '?' && from[i] != '&' && from[i] != ' ' &&
           i + 1 < cap) {
        out[i] = from[i];
        i++;
    }
    out[i] = 0;
}

void looseTag(const char* xml, const char* local, char* dst, size_t cap) {
    dst[0] = 0;
    const char* p = xml ? strstr(xml, local) : nullptr;
    if (!p) return;
    const char* gt = strchr(p, '>');
    if (!gt) return;
    gt++;
    size_t n = 0;
    while (gt[n] && gt[n] != '<' && n + 1 < cap) n++;
    memcpy(dst, gt, n);
    dst[n] = 0;
    trim(dst);
}

// Sonos will not change Spotify audio unless the description names the new
// track or playlist and the account (SA_RINCON...). The saved blob often does not.
bool buildServiceDidl(const char* playUri, const char* title, const char* stored, bool asTrack) {
    char itemId[200];
    char parent[140];
    char desc[180];
    char trackId[80];
    itemId[0] = desc[0] = trackId[0] = 0;
    strlcpy(parent, "-1", sizeof(parent));
    const char* pAt = stored ? strstr(stored, "parentID=\"") : nullptr;
    if (pAt) copyToken(pAt + 10, parent, sizeof(parent));
    if (!parent[0] || !strncmp(parent, "FV:", 3)) strlcpy(parent, "-1", sizeof(parent));
    const char* dAt = stored ? strstr(stored, "SA_RINCON") : nullptr;
    if (dAt) copyToken(dAt, desc, sizeof(desc));
    if (asTrack) {
        const char* t = playUri ? findCi(playUri, "track") : nullptr;
        if (t) copyToken(skipSpotifySep(t + 5), trackId, sizeof(trackId));
        if (trackId[0]) snprintf(itemId, sizeof(itemId), "10032020spotify%%3atrack%%3a%s", trackId);
    } else if (playUri && !strncmp(playUri, "x-rincon-cpcontainer:", 21)) {
        copyToken(playUri + 21, itemId, sizeof(itemId));
    }
    const char* cls = "object.item.audioItem.musicTrack";
    if (!asTrack) cls = hasWord(itemId, "album") ? "object.container.album.musicAlbum" : "object.container.playlistContainer";
    Xml d;
    d.reset(didlBuf, DIDL_CAP);
    d.s("<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">");
    d.s("<item id=\"");
    d.xml(itemId[0] ? itemId : "0");
    d.s("\" parentID=\"");
    d.xml(parent[0] ? parent : "-1");
    d.s("\" restricted=\"true\"><dc:title>");
    d.xml(title && title[0] ? title : "Favorite");
    d.s("</dc:title><upnp:class>");
    d.s(cls);
    d.s("</upnp:class>");
    if (desc[0]) {
        d.s("<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">");
        d.xml(desc);
        d.s("</desc>");
    }
    if (asTrack && playUri && playUri[0]) {
        d.s("<res protocolInfo=\"sonos.com-spotify:*:audio/x-spotify:*\">");
        d.xml(playUri);
        d.s("</res>");
    }
    d.s("</item></DIDL-Lite>");
    if (d.overflow) didlBuf[0] = 0;
    Serial.printf("[sonos] didl id %.90s desc %.60s\n", itemId, desc[0] ? desc : "(none)");
    return didlBuf[0] != 0;
}

// Append uri, then optionally jump to it and start it. Trims older entries only
// after the new one is actually playing, so the running stream is not cut first.
bool queueAndPlay(Room* c, const char* uri, const char* meta, bool playNow) {
    if (!c || !uri || !uri[0]) return false;
    char pos[8];
    strlcpy(pos, "1", sizeof(pos));
    char queueUri[96];
    snprintf(queueUri, sizeof(queueUri), "x-rincon-queue:%s#0", c->uuid);
    auto seekPos = [&]() {
        bool sought = invoke(c->ip, AVT_PATH, AVT, "Seek", [&] {
            el("InstanceID", "0");
            el("Unit", "TRACK_NR");
            el("Target", pos);
        });
        Serial.printf("[sonos] seek %s %d\n", pos, sought);
        return sought;
    };
    bool onQueue = false;
    if (avt(c->ip, "GetMediaInfo")) {
        char cur[160];
        copyTag(resp, "CurrentURI", cur, sizeof(cur));
        onQueue = !strncmp(cur, "x-rincon-queue:", 15);
    }
    bool added = addQueue(c->ip, uri, meta);
    Serial.printf("[sonos] queue add %d onQueue %d uri %.180s\n", added, onQueue, uri);
    if (!added) return false;
    looseTag(resp, "FirstTrackNumberEnqueued", pos, sizeof(pos));
    if (!pos[0]) strlcpy(pos, "1", sizeof(pos));
    int firstNew = atoi(pos);
    if (firstNew < 1) firstNew = 1;
    if (!playNow) {
        setStatus("Added to the queue");
        return true;
    }
    bool opened = onQueue ? true : setUri(c->ip, queueUri, "");
    Serial.printf("[sonos] first track %d, on queue %d\n", firstNew, opened);
    if (opened) seekPos();
    bool started = false;
    if (opened) {
        for (int attempt = 0; attempt < 3 && !started; attempt++) {
            if (attempt) delay(200);
            started = avt(c->ip, "Play");
        }
    }
    if (!started) return false;
    for (int k = 0; k < 6; k++) {
        delay(1800);
        char st[32] = "";
        transportState(c->ip, st, sizeof(st));
        bool playingNow = !strcmp(st, "PLAYING");
        bool busy = !strcmp(st, "TRANSITIONING");
        if (!playingNow && !busy && k < 5) {
            if (k >= 2) setUri(c->ip, queueUri, "");
            seekPos();
            bool again = avt(c->ip, "Play");
            Serial.printf("[sonos] %s after play (step %d), playing again %d\n", st[0] ? st : "?", k, again);
        }
        if (playingNow && firstNew > 1) {
            char cnt[8];
            snprintf(cnt, sizeof(cnt), "%d", firstNew - 1);
            bool trimmed = invoke(c->ip, AVT_PATH, AVT, "RemoveTrackRangeFromQueue", [&] {
                el("InstanceID", "0");
                el("UpdateID", "0");
                el("StartingIndex", "1");
                el("NumberOfTracks", cnt);
            });
            Serial.printf("[sonos] removed %s older tracks %d\n", cnt, trimmed);
            break;
        }
        if (playingNow || busy) break;
    }
    lock();
    published.playing = true;
    unlock();
    lastPoll = 0;
    return true;
}

void playFavorite(int index) {
    if (index < 0 || index >= favCount) return;
    Room* c = transportRoom();
    if (!c) {
        setStatus("No room selected");
        return;
    }
    Fav& f = favs[index];
    Serial.printf("[sonos] play '%s' uri %.160s\n", f.title, f.uri);
    if (listIsQueue) {
        char num[8];
        snprintf(num, sizeof(num), "%d", index + 1);
        bool ok = true;
        if (!localQueue && activeListId[0]) {
            char playUri[280];
            snprintf(playUri, sizeof(playUri), "x-rincon-cpcontainer:%s", activeListId);
            ok = setUri(c->ip, playUri, "");
        }
        ok = ok && invoke(c->ip, AVT_PATH, AVT, "Seek", [&] {
                    el("InstanceID", "0");
                    el("Unit", "TRACK_NR");
                    el("Target", num);
                }) &&
            avt(c->ip, "Play");
        if (!ok) setStatus("Could not play that track");
        else {
            lock();
            published.playing = true;
            unlock();
        }
        lastPoll = 0;
        return;
    }
    char raw[512];
    strlcpy(raw, f.uri, sizeof(raw));
    bool container = !strncmp(raw, "x-rincon-cpcontainer:", 21) || !strncmp(raw, "x-rincon-playlist:", 18);
    char uri[512];
    strlcpy(uri, raw, sizeof(uri));
    if (container) {
        char* q = strchr(uri, '?');
        if (q) *q = 0;
    }
    bool spotify = hasWord(raw, "spotify");
    bool oneSong = !container && !radioUri(raw) &&
                   (hasWord(raw, "x-sonos-spotify:") || hasWord(raw, "x-sonos-http:") || hasWord(raw, "x-sonosapi-hls"));
    const char* stored = (f.meta && strstr(f.meta, "DIDL-Lite")) ? f.meta : "";
    const char* meta = stored;
    if (spotify && oneSong && buildServiceDidl(raw, f.title, f.meta, true)) meta = didlBuf;
    Serial.printf("[sonos] favorite '%s' meta %u uri %.180s\n", f.title, meta[0] ? (unsigned)strlen(meta) : 0, raw);
    bool opened = false;
    // The speaker rejects a playlist address as a transport URI (714) in this
    // household, but takes it in the queue. Songs go through the queue too.
    bool viaQueue = oneSong || container;
    if (container) meta = stored;
    if (viaQueue) {
        if (!queueAndPlay(c, raw, meta, true)) {
            char msg[72];
            snprintf(msg, sizeof(msg), "%s", lastErr[0] ? lastErr : "Could not start that favorite");
            setStatus(msg);
        }
        lastPoll = 0;
        return;
    }
    opened = setUri(c->ip, uri, meta);
    bool started = false;
    if (opened) {
        for (int attempt = 0; attempt < 3 && !started; attempt++) {
            if (attempt) delay(200);
            started = avt(c->ip, "Play");
        }
    }
    Serial.printf("[sonos] opened %d started %d\n", opened, started);
    if (started) {
        // Spotify sometimes accepts Play and then drops back to STOPPED. Check, and play again.
        for (int k = 0; k < 6; k++) {
            delay(1800);
            char st[32] = "";
            transportState(c->ip, st, sizeof(st));
            bool playingNow = !strcmp(st, "PLAYING");
            bool busy = !strcmp(st, "TRANSITIONING");
            // A station sits in TRANSITIONING while it buffers.
            bool going = playingNow || busy;
            if (!going && k < 5) {
                bool again = avt(c->ip, "Play");
                Serial.printf("[sonos] %s after play (step %d), playing again %d\n", st[0] ? st : "?", k, again);
            }
            if (avt(c->ip, "GetPositionInfo")) {
                char tr[8], du[16], re[16], tu[260], tt[80];
                copyTag(resp, "Track", tr, sizeof(tr));
                copyTag(resp, "TrackDuration", du, sizeof(du));
                copyTag(resp, "RelTime", re, sizeof(re));
                copyTag(resp, "TrackURI", tu, sizeof(tu));
                tt[0] = 0;
                const char* ta = nullptr;
                const char* tb = nullptr;
                if (findTag(resp, "TrackMetaData", &ta, &tb)) {
                    char* tmp = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (tmp) {
                        size_t n = (size_t)(tb - ta);
                        if (n > 4095) n = 4095;
                        memcpy(tmp, ta, n);
                        tmp[n] = 0;
                        xmlUnescape(tmp);
                        if (strstr(tmp, "&lt;")) xmlUnescape(tmp);
                        copyTag(tmp, "dc:title", tt, sizeof(tt));
                        heap_caps_free(tmp);
                    }
                }
                Serial.printf("[verify %d] %s track %s dur %s rel %s title '%s' uri %.200s\n", k, st, tr, du, re, tt, tu);
            }
            if (going) break;
        }
    }
    if (!started) {
        char msg[72];
        snprintf(msg, sizeof(msg), "%s", lastErr[0] ? lastErr : "Could not start that favorite");
        setStatus(msg);
    }
    else {
        if (radioUri(uri) && f.title[0] && !rawLabel(f.title))
            strlcpy(lastStation, f.title, sizeof(lastStation));
        lock();
        published.playing = true;
        if (lastStation[0] && radioUri(uri)) strlcpy(published.title, lastStation, sizeof(published.title));
        unlock();
        if (lastStation[0] && radioUri(uri)) strlcpy(lastTitle, lastStation, sizeof(lastTitle));
    }
    lastPoll = 0;
}

Room* findRoom(const char* uuid) {
    if (!uuid || !uuid[0]) return nullptr;
    for (int i = 0; i < roomCount; i++)
        if (!strcmp(rooms[i].uuid, uuid)) return &rooms[i];
    return nullptr;
}

int groupPeers(const Room* target) {
    if (!target || !target->group[0]) return 0;
    int n = 0;
    for (int i = 0; i < roomCount; i++) {
        if (!strcmp(rooms[i].uuid, target->uuid)) continue;
        if (!strcmp(rooms[i].group, target->group)) n++;
    }
    return n;
}

// Pulls this room out of its zone. The rooms it leaves keep playing together.
bool standalone(Room* target) {
    char delegated[40] = "";
    if (target->coordinator) {
        for (int i = 0; i < roomCount; i++) {
            if (strcmp(rooms[i].uuid, target->uuid) && !strcmp(rooms[i].group, target->group)) {
                strlcpy(delegated, rooms[i].uuid, sizeof(delegated));
                break;
            }
        }
    }
    auto call = [&](const char* delegate) {
        return invoke(target->ip, AVT_PATH, AVT, "BecomeCoordinatorOfStandaloneGroup", [&] {
            el("InstanceID", "0");
            el("DelegatedGroupCoordinatorID", delegate);
            el("NewGroupID", "");
        });
    };
    if (call(delegated)) return true;
    if (delegated[0] && call("")) return true;
    return invoke(target->ip, AVT_PATH, AVT, "BecomeCoordinatorOfStandaloneGroup", [&] {
        el("InstanceID", "0");
    });
}

void joinRoom(const char* uuid) {
    Room* target = findRoom(uuid);
    Room* current = coord();
    if (!target || !current || !target->ip[0]) return;
    const char* master = current->group[0] ? current->group : current->uuid;
    if (!master[0] || !strcmp(target->uuid, master) || !strcmp(target->group, master)) return;
    if (groupPeers(target) > 0 && !standalone(target)) {
        setStatus("Could not move that room");
        return;
    }
    char uri[56];
    snprintf(uri, sizeof(uri), "x-rincon:%s", master);
    if (!setUri(target->ip, uri, "")) {
        setStatus("Could not add that room");
        return;
    }
    char msg[72];
    snprintf(msg, sizeof(msg), "Added %s", target->name);
    setStatus(msg);
    lastTopo = 0;
    lastPoll = 0;
    Serial.printf("[sonos] added '%s'\n", target->name);
}

void leaveRoom(const char* uuid) {
    Room* target = findRoom(uuid);
    if (!target || !target->ip[0] || groupPeers(target) == 0) return;
    if (!standalone(target)) {
        setStatus("Could not remove that room");
        return;
    }
    char msg[72];
    snprintf(msg, sizeof(msg), "Removed %s", target->name);
    setStatus(msg);
    lastTopo = 0;
    lastPoll = 0;
    Serial.printf("[sonos] removed '%s'\n", target->name);
}

void selectRoom(const char* uuid) {
    if (!uuid || !uuid[0]) return;
    Room* r = nullptr;
    for (int i = 0; i < roomCount; i++)
        if (!strcmp(rooms[i].uuid, uuid)) r = &rooms[i];
    if (!r || !strcmp(r->uuid, active)) return;
    strlcpy(active, r->uuid, sizeof(active));
    clearNowPlaying();
    clearFavs();
    publishFavs();
    publishRooms();
    lastPoll = 0;
    Serial.printf("[sonos] selected '%s'\n", r->name);
}

void handle(const Command& cmd) {
    Room* c = transportRoom();
    Room* volRoom = coord();
    switch (cmd.type) {
        case Cmd::Play:
            if (!c) return;
            lock();
            published.playing = true;
            unlock();
            if (!avt(c->ip, "Play")) setStatus("Play failed");
            lastPoll = 0;
            break;
        case Cmd::Pause:
            if (!c) return;
            lock();
            published.playing = false;
            unlock();
            if (!avt(c->ip, "Pause")) setStatus("Pause failed");
            lastPoll = 0;
            break;
        case Cmd::Next:
            if (c && !avt(c->ip, "Next")) setStatus("Next failed");
            lastPoll = 0;
            break;
        case Cmd::Prev:
            if (c && !avt(c->ip, "Previous")) setStatus("Previous failed");
            lastPoll = 0;
            break;
        case Cmd::Volume: {
            int vol = cmd.value;
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            heldVolume = vol;
            volumeHoldUntil = millis() + 2500;
            lock();
            published.volume = vol;
            unlock();
            if (!volRoom || !writeVolume(volRoom->ip, vol)) setStatus("Volume failed");
            break;
        }
        case Cmd::ToggleRoom:
            selectRoom(cmd.text);
            break;
        case Cmd::LoadFavorites:
            loadList("FV:2", false);
            break;
        case Cmd::LoadQueue:
            loadQueue();
            break;
        case Cmd::PlayFavorite:
            playFavorite(cmd.value);
            break;
        case Cmd::JoinRoom:
            joinRoom(cmd.text);
            break;
        case Cmd::LeaveRoom:
            leaveRoom(cmd.text);
            break;
    }
}

void drain() {
    Command cmd;
    while (q && xQueueReceive(q, &cmd, 0) == pdTRUE) handle(cmd);
}

}  // namespace

bool enqueue(const char* uri, const char* meta, bool playNow) {
    Room* c = transportRoom();
    if (!c) {
        setStatus("No room selected");
        return false;
    }
    // Artist radio is a station. It starts on its own, the way TuneIn does.
    if (uri && (strstr(uri, "x-sonosapi-radio") || strstr(uri, "artistRadio"))) {
        if (!playNow) {
            setStatus("A station plays on its own");
            return true;
        }
        bool opened = setUri(c->ip, uri, meta ? meta : "");
        bool started = false;
        if (opened) {
            for (int attempt = 0; attempt < 3 && !started; attempt++) {
                if (attempt) delay(200);
                started = avt(c->ip, "Play");
            }
        }
        Serial.printf("[sonos] radio opened %d started %d\n", opened, started);
        if (!started) {
            setStatus(lastErr[0] ? lastErr : "Could not start that station");
            return false;
        }
        lock();
        published.playing = true;
        unlock();
        lastPoll = 0;
        return true;
    }
    return queueAndPlay(c, uri, meta, playNow);
}

bool playDirect(const char* uri, const char* title) {
    Room* c = transportRoom();
    if (!c) {
        setStatus("No room selected");
        return false;
    }
    if (!uri || !uri[0]) return false;
    Serial.printf("[sonos] direct %u bytes\n", (unsigned)strlen(uri));
    char* meta = (char*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!meta) {
        setStatus("Out of memory");
        return false;
    }
    Xml d;
    d.reset(meta, 16 * 1024);
    d.s("<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
        "<item id=\"yt\" parentID=\"-1\" restricted=\"true\"><dc:title>");
    d.xml(title && title[0] ? title : "YouTube");
    d.s("</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/mp4:*\">");
    d.xml(uri);
    d.s("</res></item></DIDL-Lite>");
    const char* use = d.overflow ? "" : meta;
    if (d.overflow) Serial.println("[sonos] direct metadata overflow");
    bool opened = setUri(c->ip, uri, use);
    bool started = false;
    if (opened) {
        for (int attempt = 0; attempt < 3 && !started; attempt++) {
            if (attempt) delay(200);
            started = avt(c->ip, "Play");
        }
    }
    heap_caps_free(meta);
    Serial.printf("[sonos] direct opened %d started %d\n", opened, started);
    if (!started) {
        setStatus(lastErr[0] ? lastErr : "Speaker refused the stream");
        return false;
    }
    lock();
    published.playing = true;
    unlock();
    lastPoll = 0;
    return true;
}

void setDirectInfo(const char* title, const char* artist, const char* artUrl) {
    strlcpy(directTitle, title ? title : "", sizeof(directTitle));
    strlcpy(directArtist, artist ? artist : "", sizeof(directArtist));
    strlcpy(directArt, artUrl ? artUrl : "", sizeof(directArt));
}

bool seekRel(const char* hms) {
    Room* c = transportRoom();
    if (!c || !hms || !hms[0]) return false;
    bool ok = invoke(c->ip, AVT_PATH, AVT, "Seek", [&] {
        el("InstanceID", "0");
        el("Unit", "REL_TIME");
        el("Target", hms);
    });
    Serial.printf("[sonos] seek %s %d\n", hms, ok);
    return ok;
}

int spotifyAccount() { return spotifySn; }

void begin() {
    memset(&published, 0, sizeof(published));
    published.volume = -1;
    mu = xSemaphoreCreateMutex();
    q = xQueueCreate(8, sizeof(Command));
    soapBuf = (char*)ps(SOAP_CAP);
    resp = (char*)ps(RESP_CAP);
    trackBuf = (char*)ps(TRACK_CAP);
    didlBuf = (char*)ps(DIDL_CAP);
    art[0] = (uint8_t*)ps(ART_CAP);
    art[1] = (uint8_t*)ps(ART_CAP);
    published.ready = mu && q && soapBuf && resp && trackBuf && didlBuf;
    if (!art[0] || !art[1]) Serial.println("[sonos] album art disabled");
    Serial.printf("[sonos] ready %d  psram free %u\n", published.ready ? 1 : 0, (unsigned)ESP.getFreePsram());
}

void loop() {
    if (!published.ready) return;
    drain();
    uint32_t now = millis();
    if (!haveHouse) {
        if (lastDiscover != 0 && now - lastDiscover < 4000) return;
        lastDiscover = now;
        if (!discover()) return;
        lastTopo = now;
        lastPoll = 0;
    }
    if (now - lastTopo >= 8000) {
        lastTopo = now;
        Room* c = coord();
        if (!c || !getZone(c->ip)) {
            if (++topoFails >= 3) {
                haveHouse = false;
                lock();
                published.household = false;
                unlock();
                Serial.println("[sonos] lost the household");
                return;
            }
        } else {
            topoFails = 0;
            chooseActive(true);
            refreshWhosPlaying();
        }
    }
    if (haveHouse && now - lastSweep >= 3000) {
        lastSweep = now;
        refreshWhosPlaying();
    }
    if (now - lastPoll >= 1000) {
        lastPoll = now;
        if (!refreshPlayback()) {
            if (++playFails >= 3) {
                haveHouse = false;
                lock();
                published.household = false;
                unlock();
                Serial.println("[sonos] speaker stopped answering");
            }
        } else {
            playFails = 0;
            maybeClearStatus();
        }
    }
}

void command(Cmd cmd, int value, const char* text) {
    if (!q) return;
    Command c;
    c.type = cmd;
    c.value = value;
    strlcpy(c.text, text ? text : "", sizeof(c.text));
    if (xQueueSend(q, &c, 0) != pdTRUE) Serial.println("[sonos] command queue full");
}

void copyView(View& out) {
    if (!mu) {
        memset(&out, 0, sizeof(out));
        out.volume = -1;
        return;
    }
    lock();
    out = published;
    unlock();
}

size_t copyArt(uint8_t* dst, size_t cap, int* gen) {
    if (!mu || !art[0]) {
        if (gen) *gen = 0;
        return 0;
    }
    lock();
    size_t n = artLen[artSlot];
    if (n > cap) n = 0;
    if (n && dst) memcpy(dst, art[artSlot], n);
    if (gen) *gen = published.artGen;
    unlock();
    return n;
}

}  // namespace sonos
