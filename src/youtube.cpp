#include "youtube.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>

#include <WiFi.h>

#include "net.h"
#include "sonos.h"

namespace yt {
namespace {

constexpr size_t BODY_CAP = 512 * 1024;
constexpr uint32_t SEEK_AFTER_MS = 5UL * 60UL * 1000UL;
constexpr char API_KEY[] = "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w";

struct Client {
    const char* tag;
    const char* name;
    const char* version;
    const char* ua;
    const char* extra;
};

const Client kClients[] = {
    {"android_vr", "ANDROID_VR", "1.65.10",
     "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
     "\"androidSdkVersion\":32,\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\""},
    {"ios", "IOS", "21.26.4",
     "com.google.ios.youtube/21.26.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
     "\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\""},
    {"android", "ANDROID", "21.26.364",
     "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip",
     "\"androidSdkVersion\":30"},
    {"mweb", "MWEB", "2.20260708.05.00",
     "Mozilla/5.0 (iPad; CPU OS 16_7_10 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1,gzip(gfe)",
     ""},
    {"tv", "TVHTML5", "7.20260707.07.00",
     "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)",
     ""},
};

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) override { heap_caps_free(ptr); }
    void* reallocate(void* ptr, size_t size) override {
        return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

SemaphoreHandle_t mu;
char* httpBuf = nullptr;
PsramAllocator alloc;

View published;
char wantId[16] = "";
bool wantResolve = false;
char wantQuery[64] = "";
bool wantSearch = false;
bool wantMore = false;
int wantPlay = -1;
bool wantSeek = false;

char savedName[24] = "";
char savedVersion[32] = "";
char savedUa[180] = "";
char continuation[640] = "";
uint32_t seekAt = 0;
uint32_t checkAt = 0;
char* playedUrl = nullptr;
char* fallbackUrl = nullptr;
char* resolvedUrl = nullptr;  // address that answered the ranged probe, after redirects
char fallbackTitle[80] = "";
bool triedRadio = false;
bool triedRelay = false;
volatile bool relayGo = false;
volatile uint32_t relayGen = 0;  // bumped on every new selection; older relay streams stop

void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mu); }

void setStatus(const char* text) {
    lock();
    strlcpy(published.status, text ? text : "", sizeof(published.status));
    unlock();
    if (text && text[0]) Serial.printf("[yt] %s\n", text);
}

bool videoIdOk(const char* id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n < 6 || n > 12) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    }
    return true;
}

// The UI font covers U+0020..U+017F. Swap common punctuation for plain characters and drop what the
// font cannot draw (emoji, CJK), so titles do not show empty boxes.
void cleanText(char* s) {
    if (!s) return;
    const unsigned char* in = (const unsigned char*)s;
    char* out = s;
    bool lastSpace = true;
    while (*in) {
        uint32_t cp;
        int len;
        if (*in < 0x80) {
            cp = *in;
            len = 1;
        } else if ((*in & 0xE0) == 0xC0) {
            cp = *in & 0x1F;
            len = 2;
        } else if ((*in & 0xF0) == 0xE0) {
            cp = *in & 0x0F;
            len = 3;
        } else if ((*in & 0xF8) == 0xF0) {
            cp = *in & 0x07;
            len = 4;
        } else {
            in++;
            continue;
        }
        int used = 1;
        while (used < len && (in[used] & 0xC0) == 0x80) cp = (cp << 6) | (in[used++] & 0x3F);
        if (used < len) {
            in += used;
            continue;
        }
        const unsigned char* start = in;
        in += len;
        const char* repl = nullptr;
        if (cp == 0x2018 || cp == 0x2019) repl = "'";
        else if (cp == 0x201C || cp == 0x201D) repl = "\"";
        else if (cp == 0x2013 || cp == 0x2014) repl = "-";
        else if (cp == 0x2026) repl = "...";
        if (repl) {
            for (const char* r = repl; *r; r++) *out++ = *r;
            lastSpace = false;
        } else if (cp >= 0x20 && cp <= 0x17F && cp != 0x7F) {
            if (cp == ' ') {
                if (lastSpace) continue;
                lastSpace = true;
            } else {
                lastSpace = false;
            }
            for (int i = 0; i < len; i++) *out++ = (char)start[i];
        }
    }
    while (out > s && out[-1] == ' ') out--;
    *out = 0;
}

bool ensureBuf() {
    if (httpBuf) return true;
    httpBuf = (char*)heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!httpBuf) {
        setStatus("Out of memory");
        return false;
    }
    return true;
}

class Body : public Stream {
   public:
    size_t n = 0;
    bool overflow = false;
    size_t write(const uint8_t* data, size_t len) override {
        size_t room = n + 1 < BODY_CAP ? BODY_CAP - 1 - n : 0;
        size_t take = len < room ? len : room;
        if (take) memcpy(httpBuf + n, data, take);
        if (take < len) overflow = true;
        n += take;
        if (httpBuf) httpBuf[n] = 0;
        return len;
    }
    size_t write(uint8_t b) override { return write(&b, 1); }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

int postJson(const char* url, const char* payload, const char* ua, Body& out) {
    WiFiClientSecure client;
    client.useBuiltinCACertBundle();
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setTimeout(12000);
    http.setReuse(false);
    if (!http.begin(client, url)) return -1;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", ua && ua[0] ? ua : "Mozilla/5.0");
    int code = http.POST((uint8_t*)payload, strlen(payload));
    out.n = 0;
    out.overflow = false;
    if (code > 0) http.writeToStream(&out);
    http.end();
    return code;
}

// One ranged GET without following redirects. A 3xx puts the new address in `location`.
int rangedGetOnce(const char* url, char* location, size_t locationCap) {
    WiFiClientSecure client;
    client.useBuiltinCACertBundle();
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setTimeout(12000);
    http.setReuse(false);
    if (!http.begin(client, url)) return -1;
    http.addHeader("Range", "bytes=0-1023");
    const char* wanted[] = {"Location"};
    http.collectHeaders(wanted, 1);
    int code = http.GET();
    if (code >= 300 && code < 400 && location) {
        String loc = http.header("Location");
        strlcpy(location, loc.c_str(), locationCap);
    }
    uint8_t magic[8] = {};
    int got = 0;
    if (code == 200 || code == 206) {
        NetworkClient* stream = http.getStreamPtr();
        uint32_t start = millis();
        while (got < 8 && stream && millis() - start < 2000) {
            int n = stream->read(magic + got, 8 - got);
            if (n <= 0) break;
            got += n;
        }
    }
    Serial.printf("[yt] range %d type %s magic %02x%02x%02x%02x%02x%02x%02x%02x\n", code,
                  http.header("Content-Type").c_str(), magic[0], magic[1], magic[2], magic[3], magic[4], magic[5],
                  magic[6], magic[7]);
    http.end();
    return code;
}

// Follows up to three redirects. `finalUrl` (6500 bytes) receives the address that answered, so the
// relay can use it directly instead of paying for a redirect on every chunk.
int rangedGet(const char* url, char* finalUrl) {
    strlcpy(finalUrl, url, 6500);
    char* next = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) return -1;
    int code = -1;
    for (int hop = 0; hop < 4; hop++) {
        next[0] = 0;
        code = rangedGetOnce(finalUrl, next, 6500);
        if (code >= 300 && code < 400 && next[0]) {
            Serial.printf("[yt] redirect to %.60s...\n", next);
            strlcpy(finalUrl, next, 6500);
            continue;
        }
        break;
    }
    heap_caps_free(next);
    return code;
}

int scoreFormat(const char* mime, int itag, bool hasUrl) {
    if (!hasUrl || !mime) return 0;
    if (itag == 140) return 110;
    if (strstr(mime, "audio/mp4")) return 100;
    if (strstr(mime, "audio/")) return 80;
    if (itag == 18) return 60;
    if (strstr(mime, "video/mp4")) return 40;
    return 10;
}

const char* textOf(JsonObject o) {
    const char* simple = o["simpleText"] | "";
    if (simple[0]) return simple;
    const char* run = o["runs"][0]["text"] | "";
    return run;
}

void jsonEscape(const char* in, char* out, size_t cap) {
    size_t n = 0;
    if (!in) in = "";
    for (; *in && n + 2 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20) continue;
        if (*in == '"' || *in == '\\') out[n++] = '\\';
        out[n++] = *in;
    }
    out[n] = 0;
}

int parseClock(const char* text) {
    if (!text || !text[0]) return 0;
    int parts[3] = {0, 0, 0};
    int n = 0;
    int cur = 0;
    for (const char* p = text; *p && n < 3; p++) {
        if (*p >= '0' && *p <= '9') cur = cur * 10 + (*p - '0');
        else if (*p == ':') {
            parts[n++] = cur;
            cur = 0;
        } else break;
    }
    parts[n++] = cur;
    if (n == 1) return parts[0];
    if (n == 2) return parts[0] * 60 + parts[1];
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

void considerFormats(JsonArray arr, const char* tag, char* bestUrl, size_t bestCap, int& bestScore, int& bestItag) {
    for (JsonObject f : arr) {
        int itag = f["itag"] | 0;
        const char* mime = f["mimeType"] | "";
        const char* url = f["url"] | "";
        const char* cipher = f["signatureCipher"] | "";
        if (!cipher[0]) cipher = f["signature"] | "";
        Serial.printf("[yt] %s itag %d mime %.48s url %u cipher %u\n", tag, itag, mime,
                      (unsigned)strlen(url), (unsigned)strlen(cipher));
        int score = scoreFormat(mime, itag, url[0] != 0);
        if (score > bestScore && strlen(url) + 1 < bestCap) {
            bestScore = score;
            bestItag = itag;
            strlcpy(bestUrl, url, bestCap);
        }
    }
}

bool startRelay(const char* title);

void resolveId(const char* id) {
    if (!ensureBuf()) return;
    setStatus("Opening YouTube");
    char* best = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* payload = (char*)heap_caps_malloc(900, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!best || !payload) {
        heap_caps_free(best);
        heap_caps_free(payload);
        setStatus("Out of memory");
        return;
    }
    best[0] = 0;
    triedRelay = false;
    relayGo = false;
    relayGen = relayGen + 1;  // drop any stream still being relayed for the previous pick
    checkAt = 0;
    seekAt = 0;
    char title[80] = "YouTube";
    char author[48] = "";
    bool played = false;
    bool sawHttp = false;
    bool handed = false;

    auto tryClient = [&](const char* tag, const char* name, const char* version, const char* ua,
                         const char* extra) {
        if (played) return;
        snprintf(payload, 900,
                 "{\"context\":{\"client\":{\"clientName\":\"%s\",\"clientVersion\":\"%s\","
                 "\"hl\":\"en\",\"gl\":\"US\"%s%s}},\"videoId\":\"%s\","
                 "\"contentCheckOk\":true,\"racyCheckOk\":true}",
                 name, version, (extra && extra[0]) ? "," : "", extra ? extra : "", id);
        char url[160];
        snprintf(url, sizeof(url),
                 "https://www.youtube.com/youtubei/v1/player?prettyPrint=false&key=%s", API_KEY);
        Body out;
        int code = postJson(url, payload, ua, out);
        Serial.printf("[yt] %s player %d bytes %u overflow %d\n", tag, code, (unsigned)out.n,
                      out.overflow);
        if (code == 200) sawHttp = true;
        if (code != 200 || out.n < 20) return;
        JsonDocument doc(&alloc);
        JsonDocument filter(&alloc);
        filter["playabilityStatus"]["status"] = true;
        filter["playabilityStatus"]["reason"] = true;
        filter["videoDetails"]["title"] = true;
        filter["videoDetails"]["author"] = true;
        filter["videoDetails"]["lengthSeconds"] = true;
        filter["streamingData"]["hlsManifestUrl"] = true;
        filter["streamingData"]["adaptiveFormats"][0]["itag"] = true;
        filter["streamingData"]["adaptiveFormats"][0]["mimeType"] = true;
        filter["streamingData"]["adaptiveFormats"][0]["url"] = true;
        filter["streamingData"]["adaptiveFormats"][0]["signatureCipher"] = true;
        filter["streamingData"]["formats"][0]["itag"] = true;
        filter["streamingData"]["formats"][0]["mimeType"] = true;
        filter["streamingData"]["formats"][0]["url"] = true;
        filter["streamingData"]["formats"][0]["signatureCipher"] = true;
        DeserializationError err =
            deserializeJson(doc, httpBuf, out.n, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(30));
        if (err) {
            Serial.printf("[yt] %s json %s\n", tag, err.c_str());
            return;
        }
        const char* status = doc["playabilityStatus"]["status"] | "";
        const char* reason = doc["playabilityStatus"]["reason"] | "";
        Serial.printf("[yt] %s playable %s %s\n", tag, status, reason);
        const char* gotTitle = doc["videoDetails"]["title"] | "";
        if (gotTitle[0]) {
            strlcpy(title, gotTitle, sizeof(title));
            cleanText(title);
            if (!title[0]) strlcpy(title, "YouTube", sizeof(title));
        }
        const char* gotAuthor = doc["videoDetails"]["author"] | "";
        if (gotAuthor[0]) {
            strlcpy(author, gotAuthor, sizeof(author));
            cleanText(author);
        }
        const char* hls = doc["streamingData"]["hlsManifestUrl"] | "";
        Serial.printf("[yt] %s hls %u\n", tag, (unsigned)strlen(hls));
        int score = 0;
        int itag = 0;
        best[0] = 0;
        considerFormats(doc["streamingData"]["adaptiveFormats"].as<JsonArray>(), tag, best, 6500, score, itag);
        considerFormats(doc["streamingData"]["formats"].as<JsonArray>(), tag, best, 6500, score, itag);
        char* aacUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char* progUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!aacUrl || !progUrl) {
            heap_caps_free(aacUrl);
            heap_caps_free(progUrl);
            return;
        }
        aacUrl[0] = progUrl[0] = 0;
        auto take = [&](JsonArray arr) {
            for (JsonObject f : arr) {
                int idTag = f["itag"] | 0;
                const char* url = f["url"] | "";
                if (!url[0] || strlen(url) > 6490) continue;
                if (idTag == 140) strlcpy(aacUrl, url, 6500);
                if (idTag == 18) strlcpy(progUrl, url, 6500);
            }
        };
        take(doc["streamingData"]["adaptiveFormats"].as<JsonArray>());
        take(doc["streamingData"]["formats"].as<JsonArray>());
        // Sonos plays the plain MP4 of itag 18 but drops DASH itag 140 (fragmented MP4).
        const char* primary = progUrl[0] ? progUrl : (aacUrl[0] ? aacUrl : best);
        int primaryItag = progUrl[0] ? 18 : (aacUrl[0] ? 140 : itag);
        const char* second = "";  // if itag 18 is refused, let the next client try rather than settle for 140
        if (false && (!primary || !primary[0]) && hls[0] && strlen(hls) < 6499) {  // the relay cannot serve an HLS playlist
            primary = hls;
            primaryItag = 0;
            second = "";
        }
        bool started = false;
        const char* used = nullptr;
        if (primary && primary[0]) {
            Serial.printf("[yt] %s chosen itag %d len %u\n", tag, primaryItag, (unsigned)strlen(primary));
            if (!resolvedUrl) resolvedUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int range = resolvedUrl ? rangedGet(primary, resolvedUrl) : -1;
            if (range == 200 || range == 206) {
                handed = true;
                used = resolvedUrl;
                started = true;
            } else {
                Serial.printf("[yt] %s range failed\n", tag);
            }
        }
        if (!started && second[0]) {
            Serial.printf("[yt] %s trying itag 18 len %u\n", tag, (unsigned)strlen(second));
            if (!resolvedUrl) resolvedUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int range = resolvedUrl ? rangedGet(second, resolvedUrl) : -1;
            if (range == 200 || range == 206) {
                handed = true;
                used = resolvedUrl;
                started = true;
                second = "";
            }
        }
        if (started && used) {
            if (!playedUrl) playedUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (playedUrl) strlcpy(playedUrl, used, 6500);
            triedRadio = false;
            triedRelay = false;
            strlcpy(fallbackTitle, title, sizeof(fallbackTitle));
            char thumb[96];
            snprintf(thumb, sizeof(thumb), "http://i.ytimg.com/vi/%s/mqdefault.jpg", id);
            sonos::setDirectInfo(title, author, thumb);
            lock();
            strlcpy(published.playingId, id, sizeof(published.playingId));
            strlcpy(published.playingTitle, title, sizeof(published.playingTitle));
            strlcpy(published.playingChannel, author, sizeof(published.playingChannel));
            unlock();
            started = startRelay(title);
        }
        if (started && second[0]) {
            if (!fallbackUrl) fallbackUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (fallbackUrl) strlcpy(fallbackUrl, second, 6500);
            strlcpy(fallbackTitle, title, sizeof(fallbackTitle));
            checkAt = millis() + 6000;
            seekAt = 0;
        } else if (started) {
            if (fallbackUrl) fallbackUrl[0] = 0;
            strlcpy(fallbackTitle, title, sizeof(fallbackTitle));
            checkAt = millis() + 6000;
            seekAt = 0;
        }
        heap_caps_free(aacUrl);
        heap_caps_free(progUrl);
        if (started) played = true;
    };

    if (savedName[0] && savedVersion[0]) {
        tryClient("saved", savedName, savedVersion, savedUa[0] ? savedUa : "Mozilla/5.0", "");
    }
    for (const Client& c : kClients) tryClient(c.tag, c.name, c.version, c.ua, c.extra);

    heap_caps_free(best);
    heap_caps_free(payload);
    if (!played) {
        if (!sawHttp) setStatus("YouTube busy");
        else if (handed) setStatus("Speaker refused the stream");
        else setStatus("No audio allowed for this video");
        return;
    }
    setStatus("Playing");
}

// "Streamed 2 hours ago" -> "2 hrs". "4 days ago" -> "4 days". Empty when it does not match.
void shortenAge(const char* in, char* out, size_t cap) {
    out[0] = 0;
    if (!in || !in[0] || cap < 8) return;
    const char* p = in;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return;
    int n = 0;
    while (isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        if (n > 9999) n = 9999;
        p++;
    }
    while (*p && !isalpha((unsigned char)*p)) p++;
    const char* unit = nullptr;
    if (strncmp(p, "second", 6) == 0) unit = "sec";
    else if (strncmp(p, "minute", 6) == 0) unit = "min";
    else if (strncmp(p, "hour", 4) == 0) unit = n == 1 ? "hr" : "hrs";
    else if (strncmp(p, "day", 3) == 0) unit = n == 1 ? "day" : "days";
    else if (strncmp(p, "week", 4) == 0) unit = n == 1 ? "wk" : "wks";
    else if (strncmp(p, "month", 5) == 0) unit = "mo";
    else if (strncmp(p, "year", 4) == 0) unit = n == 1 ? "yr" : "yrs";
    if (unit) snprintf(out, cap, "%d %s", n, unit);
}

// "1,234,567 views" -> "1.2M". "1.2M views" stays "1.2M". Empty when there is no count.
void shortenViews(const char* in, char* out, size_t cap) {
    out[0] = 0;
    if (!in || !in[0] || cap < 4) return;
    while (*in == ' ') in++;
    if (!strncmp(in, "No views", 8)) {
        strlcpy(out, "0", cap);
        return;
    }
    char token[24];
    size_t n = 0;
    for (const char* p = in; *p && *p != ' ' && n + 1 < sizeof(token); p++) token[n++] = *p;
    token[n] = 0;
    if (!token[0]) return;
    bool digits = true;
    long long v = 0;
    for (const char* p = token; *p; p++) {
        if (*p == ',') continue;
        if (!isdigit((unsigned char)*p)) {
            digits = false;
            break;
        }
        v = v * 10 + (*p - '0');
    }
    if (!digits || v < 1000) {
        strlcpy(out, token, cap);
        return;
    }
    long long div = 1000;
    char suf = 'K';
    if (v >= 1000000000LL) {
        div = 1000000000LL;
        suf = 'B';
    } else if (v >= 1000000LL) {
        div = 1000000LL;
        suf = 'M';
    }
    long long whole = v / div;
    int tenth = (int)((v % div) / (div / 10));
    if (tenth) snprintf(out, cap, "%d.%d%c", (int)whole, tenth, suf);
    else snprintf(out, cap, "%d%c", (int)whole, suf);
}

void addItem(const char* id, const char* title, const char* channel, int seconds, const char* age, const char* views) {
    if (!videoIdOk(id)) return;
    lock();
    if (published.count >= MAX_ITEMS) {
        unlock();
        return;
    }
    for (int i = 0; i < published.count; i++) {
        if (strcmp(published.items[i].id, id) == 0) {
            unlock();
            return;
        }
    }
    Item& it = published.items[published.count++];
    memset(&it, 0, sizeof(it));
    strlcpy(it.id, id, sizeof(it.id));
    strlcpy(it.title, title && title[0] ? title : id, sizeof(it.title));
    strlcpy(it.channel, channel ? channel : "", sizeof(it.channel));
    cleanText(it.title);
    cleanText(it.channel);
    if (!it.title[0]) strlcpy(it.title, id, sizeof(it.title));
    it.seconds = seconds;
    shortenAge(age, it.age, sizeof(it.age));
    shortenViews(views, it.views, sizeof(it.views));
    unlock();
}

void takeVideo(JsonObject v) {
    const char* id = v["videoId"] | "";
    const char* title = textOf(v["title"]);
    const char* channel = textOf(v["longBylineText"]);
    if (!channel[0]) channel = textOf(v["ownerText"]);
    if (!channel[0]) channel = textOf(v["shortBylineText"]);
    const char* length = textOf(v["lengthText"]);
    const char* age = textOf(v["publishedTimeText"]);
    const char* views = textOf(v["shortViewCountText"]);
    if (!views[0]) views = textOf(v["viewCountText"]);
    addItem(id, title, channel, parseClock(length), age, views);
}

void takeContents(JsonArray contents) {
    for (JsonObject block : contents) {
        JsonArray items = block["itemSectionRenderer"]["contents"].as<JsonArray>();
        if (items.isNull()) items = block["continuationItems"].as<JsonArray>();
        if (items.isNull()) {
            const char* token = block["continuationItemRenderer"]["continuationEndpoint"]["continuationCommand"]["token"] | "";
            if (token[0]) {
                strlcpy(continuation, token, sizeof(continuation));
                lock();
                published.more = true;
                unlock();
            }
            continue;
        }
        for (JsonObject it : items) {
            JsonObject v = it["compactVideoRenderer"];
            if (v.isNull()) v = it["videoRenderer"];
            if (!v.isNull()) {
                takeVideo(v);
                continue;
            }
            const char* token = it["continuationItemRenderer"]["continuationEndpoint"]["continuationCommand"]["token"] | "";
            if (token[0]) {
                strlcpy(continuation, token, sizeof(continuation));
                lock();
                published.more = true;
                unlock();
            }
        }
    }
}

bool grab(const char* start, const char* end, const char* key, char* out, size_t cap) {
    size_t k = strlen(key);
    for (const char* p = start; p + k < end; p++) {
        if (memcmp(p, key, k) != 0) continue;
        p += k;
        size_t n = 0;
        while (p < end && *p != '"' && n + 1 < cap) {
            if (*p == '\\' && p + 1 < end) {
                p++;
                if (*p == 'n') {
                    out[n++] = ' ';
                    p++;
                    continue;
                }
            }
            out[n++] = *p++;
        }
        out[n] = 0;
        return out[0] != 0;
    }
    return false;
}

void scanSearch(size_t n) {
    const char* begin = httpBuf;
    const char* end = httpBuf + n;
    const char* videoKey = strstr(begin, "\"videoId\"");
    Serial.printf("[yt] videoId offset %d compact %d lockup %d\n", videoKey ? (int)(videoKey - begin) : -1,
                  strstr(begin, "compactVideoRenderer") ? 1 : 0, strstr(begin, "lockupViewModel") ? 1 : 0);
    const char* p = begin;
    while (p < end) {
        const char* match = nullptr;
        for (const char* q = p; q + 24 < end; q++) {
            if (memcmp(q, "\"compactVideoRenderer\"", 23) == 0 || memcmp(q, "\"videoRenderer\"", 15) == 0 ||
                memcmp(q, "\"videoId\":\"", 11) == 0) {
                match = q;
                break;
            }
        }
        if (!match) break;
        const char* hit = match - 900;
        if (hit < begin) hit = begin;
        const char* window = match + 2500;
        if (window > end) window = end;
        char id[16] = "";
        char title[80] = "";
        char channel[48] = "";
        char length[20] = "";
        char age[40] = "";
        char views[32] = "";
        const char* limit = end;
        for (const char* q = match + 24; q + 24 < end && q < match + 12000; q++) {
            if (memcmp(q, "\"compactVideoRenderer\"", 23) == 0 || memcmp(q, "\"videoRenderer\"", 15) == 0) {
                limit = q;
                break;
            }
        }
        if (limit > match + 12000) limit = match + 12000;
        grab(hit, window, "\"videoId\":\"", id, sizeof(id));
        if (!grab(hit, window, "\"title\":{\"runs\":[{\"text\":\"", title, sizeof(title)))
            grab(hit, window, "\"title\":{\"simpleText\":\"", title, sizeof(title));
        if (!grab(hit, window, "\"longBylineText\":{\"runs\":[{\"text\":\"", channel, sizeof(channel)))
            grab(hit, window, "\"ownerText\":{\"runs\":[{\"text\":\"", channel, sizeof(channel));
        if (!grab(hit, window, "\"lengthText\":{\"runs\":[{\"text\":\"", length, sizeof(length)))
            grab(hit, window, "\"lengthText\":{\"simpleText\":\"", length, sizeof(length));
        if (!grab(match, limit, "\"publishedTimeText\":{\"simpleText\":\"", age, sizeof(age)))
            grab(match, limit, "\"publishedTimeText\":{\"runs\":[{\"text\":\"", age, sizeof(age));
        if (!grab(match, limit, "\"shortViewCountText\":{\"simpleText\":\"", views, sizeof(views)))
            if (!grab(match, limit, "\"viewCountText\":{\"simpleText\":\"", views, sizeof(views)))
                if (!grab(match, limit, "\"shortViewCountText\":{\"runs\":[{\"text\":\"", views, sizeof(views)))
                    grab(match, limit, "\"viewCountText\":{\"runs\":[{\"text\":\"", views, sizeof(views));
        if (videoIdOk(id) && title[0]) addItem(id, title, channel, parseClock(length), age, views);
        p = match + 11;
    }
    const char* found = strstr(begin, "continuationCommand");
    if (found) {
        char token[640];
        if (grab(found, end, "\"token\":\"", token, sizeof(token)) && strlen(token) > 20) {
            strlcpy(continuation, token, sizeof(continuation));
            lock();
            published.more = true;
            unlock();
        }
    }
}

// The speaker reads the relay in short bursts and reconnects often. A separate task keeps filling a
// PSRAM ring ahead of it, so a slow moment on Wi-Fi or a search running at the same time does not
// leave the speaker without audio.
constexpr size_t RING_CAP = 6u * 1024 * 1024;
constexpr size_t FETCH_CHUNK = 256 * 1024;
constexpr size_t LOOKAHEAD = 4u * 1024 * 1024;
constexpr size_t KEEP_BEHIND = 512 * 1024;
uint8_t* ring = nullptr;
char* fetchUrl = nullptr;
// The speaker re-reads the MP4 index (the start of the file) while it plays. The ring has long moved on
// by then, so the first bytes are kept for good and can be served at once.
constexpr size_t HEAD_CAP = 6u * 1024 * 1024;
uint8_t* headCache = nullptr;
volatile size_t headFilled = 0;  // bytes of the file start held in headCache
volatile size_t fillPos = 0;    // next absolute byte the fetcher will write
volatile size_t headPos = 0;    // where the speaker is reading
volatile size_t totalSize = 0;  // 0 until the first answer says how big the file is
volatile bool fetchEof = false;
volatile bool fetchFailed = false;
volatile int32_t seekReq = -1;
volatile uint32_t fetchGen = 0;
volatile uint32_t fetchEpoch = 0;

void fetchTask(void*) {
    uint8_t buf[4096];
    int failures = 0;
    for (;;) {
        if (!ring || !fetchUrl || !relayGo || !playedUrl || !playedUrl[0]) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const uint32_t gen = relayGen;
        if (fetchGen != gen) {
            strlcpy(fetchUrl, playedUrl, 6500);
            fillPos = 0;
            headPos = 0;
            totalSize = 0;
            fetchEof = false;
            fetchFailed = false;
            headFilled = 0;
            seekReq = -1;
            failures = 0;
            fetchEpoch = fetchEpoch + 1;
            fetchGen = gen;
        }
        if (seekReq >= 0) {
            size_t s = (size_t)seekReq;
            seekReq = -1;
            fillPos = s;
            headPos = s;
            fetchEof = false;
            fetchEpoch = fetchEpoch + 1;
        }
        const size_t total = totalSize;
        if (fetchFailed || fetchEof || (total && fillPos >= total)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const size_t startAt = fillPos;
        if (startAt >= headPos + LOOKAHEAD || startAt + FETCH_CHUNK > headPos + RING_CAP - KEEP_BEHIND) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        size_t endByte = startAt + FETCH_CHUNK - 1;
        if (total && endByte >= total) endByte = total - 1;
        char spec[48];
        snprintf(spec, sizeof(spec), "bytes=%u-%u", (unsigned)startAt, (unsigned)endByte);

        WiFiClientSecure up;
        up.useBuiltinCACertBundle();
        up.setHandshakeTimeout(8);
        HTTPClient http;
        http.setTimeout(15000);
        http.setReuse(false);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        size_t gotChunk = 0;
        int code = -1;
        if (http.begin(up, fetchUrl)) {
            http.addHeader("Range", spec);
            const char* wanted[] = {"Content-Range"};
            http.collectHeaders(wanted, 1);
            code = http.GET();
            if (code == 200 || code == 206) {
                if (!totalSize) {
                    String cr = http.header("Content-Range");
                    const char* slash = strchr(cr.c_str(), '/');
                    if (slash && slash[1] && slash[1] != '*') totalSize = strtoul(slash + 1, nullptr, 10);
                }
                NetworkClient* stream = http.getStreamPtr();
                uint32_t lastData = millis();
                while (stream && relayGen == gen && seekReq < 0 && gotChunk < FETCH_CHUNK) {
                    int got = stream->read(buf, sizeof(buf));
                    if (got > 0) {
                        size_t at = fillPos;
                        size_t off = at % RING_CAP;
                        size_t first = RING_CAP - off;
                        if ((size_t)got <= first) {
                            memcpy(ring + off, buf, got);
                        } else {
                            memcpy(ring + off, buf, first);
                            memcpy(ring, buf + first, got - first);
                        }
                        if (headCache && at == headFilled && at < HEAD_CAP) {
                            size_t c = HEAD_CAP - at;
                            if (c > (size_t)got) c = (size_t)got;
                            memcpy(headCache + at, buf, c);
                            headFilled = at + c;
                        }
                        fillPos = at + got;
                        gotChunk += (size_t)got;
                        lastData = millis();
                        continue;
                    }
                    if (!stream->connected()) break;
                    if (millis() - lastData > 10000) break;
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }
        http.end();
        if (relayGen != gen || fetchGen != gen) continue;
        if (seekReq >= 0) continue;
        if (gotChunk == 0) {
            Serial.printf("[yt] fetch %s failed (%d)\n", spec, code);
            if (++failures >= 3) {
                fetchFailed = true;
                Serial.println("[yt] fetch gave up");
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        failures = 0;
        if (!totalSize && gotChunk < FETCH_CHUNK) {
            totalSize = fillPos;
            fetchEof = true;
        }
        Serial.printf("[yt] fetch to %u of %u, speaker at %u, heap %u (largest %u), psram %u\n", (unsigned)fillPos,
                      (unsigned)totalSize, (unsigned)headPos, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreePsram());
    }
}

volatile int ringClients = 0;    // connections currently reading from the ring
volatile int activeClients = 0;  // connection tasks alive

// Copy one closed range from YouTube straight to a client, without touching the ring.
bool proxyChunk(WiFiClient& client, uint32_t myGen, size_t& pos, size_t total) {
    uint8_t buf[4096];
    size_t endByte = pos + FETCH_CHUNK - 1;
    if (endByte >= total) endByte = total - 1;
    char spec[48];
    snprintf(spec, sizeof(spec), "bytes=%u-%u", (unsigned)pos, (unsigned)endByte);
    WiFiClientSecure up;
    up.useBuiltinCACertBundle();
    up.setHandshakeTimeout(8);
    HTTPClient http;
    http.setTimeout(15000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    size_t got = 0;
    int code = -1;
    if (fetchUrl && http.begin(up, fetchUrl)) {
        http.addHeader("Range", spec);
        code = http.GET();
        if (code == 206 || (code == 200 && pos == 0)) {
            NetworkClient* stream = http.getStreamPtr();
            uint32_t lastData = millis();
            while (stream && relayGen == myGen && client.connected() && pos <= endByte) {
                int n = stream->read(buf, sizeof(buf));
                if (n > 0) {
                    int off = 0;
                    uint32_t stall = millis();
                    while (off < n && client.connected() && relayGen == myGen && millis() - stall < 25000) {
                        size_t w = client.write(buf + off, n - off);
                        if (w == 0) {
                            vTaskDelay(pdMS_TO_TICKS(5));
                            continue;
                        }
                        off += (int)w;
                        stall = millis();
                    }
                    if (off < n) break;
                    pos += n;
                    got += n;
                    lastData = millis();
                    continue;
                }
                if (!stream->connected()) break;
                if (millis() - lastData > 10000) break;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }
    http.end();
    if (got == 0) Serial.printf("[yt] side read %s failed (%d)\n", spec, code);
    return got > 0;
}

// ---- MP4 audio to ADTS -------------------------------------------------------------------------
// The speaker cannot play the MP4 file for long: it loads the sample table in pieces and gives up after
// about 32768 audio samples (roughly twelve minutes). So the relay does not hand over the MP4. It reads
// the audio track's sample table from the start of the file (moov, kept in headCache), picks the audio
// out of the interleaved file as it streams by, and sends it as a plain ADTS AAC stream, like a radio
// station. That has no index and no length, so there is nothing for the speaker to run out of.
constexpr bool ADTS_MODE = true;

struct AacInfo {
    uint32_t gen = 0;
    int state = 0;  // 0 not parsed yet, 1 ready, -1 not usable
    const uint8_t* stsc = nullptr;
    uint32_t stscCount = 0;
    const uint8_t* stco = nullptr;
    uint32_t stcoCount = 0;
    const uint8_t* stsz = nullptr;
    uint32_t stszCount = 0;
    uint32_t stszConst = 0;
    uint8_t b2 = 0;      // ADTS byte 2 without the channel high bit
    uint8_t chan = 0;
};
AacInfo aac;
volatile int aacLock = 0;
volatile uint32_t aacNext = 0;   // next audio sample to send
volatile uint32_t aacOwner = 0;  // the connection that streams right now (a newer one takes over)

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Finds the next box of `type` between start and end. Returns its body range.
bool findBox(const uint8_t* b, size_t start, size_t end, const char* type, size_t* bodyStart, size_t* bodyEnd) {
    while (start + 8 <= end) {
        uint32_t size = be32(b + start);
        size_t hdr = 8;
        if (size == 1) {
            if (start + 16 > end || be32(b + start + 8) != 0) return false;
            size = be32(b + start + 12);
            hdr = 16;
        } else if (size == 0) {
            size = (uint32_t)(end - start);
        }
        if (size < hdr || start + size > end) return false;
        if (memcmp(b + start + 4, type, 4) == 0) {
            *bodyStart = start + hdr;
            *bodyEnd = start + size;
            return true;
        }
        start += size;
    }
    return false;
}

// 1 ready, 0 needs more of the file, -1 cannot be used.
int parseAac(AacInfo& a) {
    const uint8_t* b = headCache;
    const size_t filled = headFilled;
    size_t p = 0, moovS = 0, moovE = 0;
    while (p + 8 <= filled) {
        uint32_t size = be32(b + p);
        if (size < 8) return -1;
        if (memcmp(b + p + 4, "moov", 4) == 0) {
            if (p + size > filled) return filled >= HEAD_CAP ? -1 : 0;
            moovS = p + 8;
            moovE = p + size;
            break;
        }
        if (memcmp(b + p + 4, "mdat", 4) == 0) return -1;  // index at the end: not handled
        p += size;
    }
    if (!moovE) return filled >= HEAD_CAP ? -1 : 0;
    size_t t = moovS, ts = 0, te = 0, ms = 0, me = 0;
    bool found = false;
    while (findBox(b, t, moovE, "trak", &ts, &te)) {
        size_t hs, he;
        if (findBox(b, ts, te, "mdia", &ms, &me) && findBox(b, ms, me, "hdlr", &hs, &he) && he - hs >= 12 &&
            memcmp(b + hs + 8, "soun", 4) == 0) {
            found = true;
            break;
        }
        t = te;
    }
    if (!found) return -1;
    size_t is, ie, ss, se, a1, a2, e1, e2;
    if (!findBox(b, ms, me, "minf", &is, &ie) || !findBox(b, is, ie, "stbl", &ss, &se)) return -1;
    size_t stsdS, stsdE, stscS, stscE, stcoS, stcoE, stszS, stszE;
    if (!findBox(b, ss, se, "stsd", &stsdS, &stsdE) || !findBox(b, ss, se, "stsc", &stscS, &stscE) ||
        !findBox(b, ss, se, "stco", &stcoS, &stcoE) || !findBox(b, ss, se, "stsz", &stszS, &stszE))
        return -1;
    // stsd: version/flags, count, then the mp4a entry (box header, 28 bytes of fields, child boxes).
    if (stsdE - stsdS < 8 + 8 + 28 || memcmp(b + stsdS + 12, "mp4a", 4) != 0) return -1;
    size_t entryEnd = stsdS + 8 + be32(b + stsdS + 8);
    if (entryEnd > stsdE) entryEnd = stsdE;
    if (!findBox(b, stsdS + 8 + 8 + 28, entryEnd, "esds", &e1, &e2)) return -1;
    (void)a1;
    (void)a2;
    const uint8_t* d = b + e1 + 4;
    const uint8_t* dEnd = b + e2;
    auto readLen = [&](const uint8_t*& q) {
        uint32_t l = 0;
        for (int i = 0; i < 4 && q < dEnd; i++) {
            uint8_t c = *q++;
            l = (l << 7) | (c & 0x7F);
            if (!(c & 0x80)) break;
        }
        return l;
    };
    if (d >= dEnd || *d++ != 0x03) return -1;
    readLen(d);
    d += 2;
    if (d >= dEnd) return -1;
    uint8_t flags = *d++;
    if (flags & 0x80) d += 2;
    if (flags & 0x40) {
        if (d >= dEnd) return -1;
        d += 1 + *d;
    }
    if (flags & 0x20) d += 2;
    if (d >= dEnd || *d++ != 0x04) return -1;
    readLen(d);
    d += 13;
    if (d >= dEnd || *d++ != 0x05) return -1;
    readLen(d);
    if (d + 2 > dEnd) return -1;
    uint8_t aot = d[0] >> 3;
    uint8_t freq = ((d[0] & 7) << 1) | (d[1] >> 7);
    uint8_t chan = (d[1] >> 3) & 0x0F;
    if (aot < 1 || aot > 4 || freq >= 13 || chan < 1 || chan > 7) {
        Serial.printf("[yt] aac: audio type %u freq %u chan %u not handled\n", aot, freq, chan);
        return -1;
    }
    a.b2 = (uint8_t)(((aot - 1) << 6) | (freq << 2) | (chan >> 2));
    a.chan = chan;
    if (stscE - stscS < 8 || stcoE - stcoS < 8 || stszE - stszS < 12) return -1;
    a.stscCount = be32(b + stscS + 4);
    a.stsc = b + stscS + 8;
    a.stcoCount = be32(b + stcoS + 4);
    a.stco = b + stcoS + 8;
    a.stszConst = be32(b + stszS + 4);
    a.stszCount = be32(b + stszS + 8);
    a.stsz = b + stszS + 12;
    if (stscS + 8 + (size_t)a.stscCount * 12 > stscE || stcoS + 8 + (size_t)a.stcoCount * 4 > stcoE ||
        (!a.stszConst && stszS + 12 + (size_t)a.stszCount * 4 > stszE) || !a.stscCount || !a.stcoCount)
        return -1;
    Serial.printf("[yt] aac: type %u freq %u chan %u, %u chunks, %u samples, moov %u bytes\n", aot, freq, chan,
                  (unsigned)a.stcoCount, (unsigned)a.stszCount, (unsigned)(moovE - moovS));
    return 1;
}

int aacReady(uint32_t myGen) {
    while (__sync_lock_test_and_set(&aacLock, 1)) vTaskDelay(pdMS_TO_TICKS(2));
    if (aac.gen != myGen) {
        aac = AacInfo();
        aac.gen = myGen;
        aacNext = 0;
    }
    if (aac.state == 0 && fetchGen == myGen && headCache) {
        int r = parseAac(aac);
        if (r != 0) aac.state = r;
    }
    int s = aac.state;
    __sync_lock_release(&aacLock);
    return s;
}

// Makes sure file bytes [start, end) are in the ring. The ring is only moved by the one streaming client.
bool ringHas(size_t start, size_t end, uint32_t myGen, uint32_t mine) {
    uint32_t waitFrom = millis();
    for (;;) {
        if (relayGen != myGen || aacOwner != mine) return false;
        size_t fill = fillPos;
        size_t validStart = fill > RING_CAP - FETCH_CHUNK ? fill - (RING_CAP - FETCH_CHUNK) : 0;
        if (start < validStart || start > fill + 4 * FETCH_CHUNK) {
            uint32_t epoch = fetchEpoch;
            seekReq = (int32_t)start;
            uint32_t w0 = millis();
            while (relayGen == myGen && fetchEpoch == epoch && millis() - w0 < 15000) vTaskDelay(pdMS_TO_TICKS(5));
            waitFrom = millis();
            continue;
        }
        if (end <= fill) return true;
        if (fetchEof || fetchFailed || millis() - waitFrom > 25000) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

struct Out {
    WiFiClient* c;
    uint8_t buf[4096];
    size_t n = 0;
    bool bad = false;
    void flush(uint32_t myGen) {
        size_t off = 0;
        uint32_t stall = millis();
        while (off < n && c->connected() && relayGen == myGen && millis() - stall < 30000) {
            size_t w = c->write(buf + off, n - off);
            if (w == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            off += w;
            stall = millis();
        }
        if (off < n) bad = true;
        n = 0;
    }
    void add(const uint8_t* p, size_t len, uint32_t myGen) {
        while (len && !bad) {
            size_t room = sizeof(buf) - n;
            size_t k = len < room ? len : room;
            memcpy(buf + n, p, k);
            n += k;
            p += k;
            len -= k;
            if (n == sizeof(buf)) flush(myGen);
        }
    }
};

void serveAac(WiFiClient& client, uint32_t myGen) {
    const uint32_t mine = __sync_add_and_fetch(&aacOwner, 1);
    uint32_t t0 = millis();
    int ready;
    while ((ready = aacReady(myGen)) == 0 && relayGen == myGen && millis() - t0 < 40000) vTaskDelay(pdMS_TO_TICKS(20));
    if (ready != 1 || relayGen != myGen) {
        Serial.println("[yt] aac: audio index not usable");
        client.print("HTTP/1.0 502 Bad Gateway\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }
    client.print("HTTP/1.0 200 OK\r\nContent-Type: audio/aac\r\nicy-name: YouTube\r\nCache-Control: no-cache\r\n"
                 "Connection: close\r\n\r\n");
    const AacInfo& a = aac;
    uint32_t s = aacNext;
    // Find the chunk that holds sample s.
    uint32_t c = 0, fs = 0, run = 0, spc = 0;
    for (;; c++) {
        if (c >= a.stcoCount) break;
        while (run + 1 < a.stscCount && be32(a.stsc + 12 * (run + 1)) <= c + 1) run++;
        spc = be32(a.stsc + 12 * run + 4);
        if (fs + spc > s) break;
        fs += spc;
    }
    Serial.printf("[yt] aac: streaming from sample %u (chunk %u)\n", (unsigned)s, (unsigned)c);
    Out* out = new Out();
    out->c = &client;
    uint32_t sent = 0;
    while (c < a.stcoCount && client.connected() && relayGen == myGen && aacOwner == mine && !out->bad) {
        while (run + 1 < a.stscCount && be32(a.stsc + 12 * (run + 1)) <= c + 1) run++;
        spc = be32(a.stsc + 12 * run + 4);
        size_t off = be32(a.stco + 4 * c);
        for (uint32_t i = fs; i < fs + spc && i < a.stszCount; i++) {
            size_t sz = a.stszConst ? a.stszConst : be32(a.stsz + 4 * i);
            if (i < s) {
                off += sz;
                continue;
            }
            if (!ringHas(off, off + sz, myGen, mine)) {
                out->bad = true;
                break;
            }
            headPos = off;
            uint32_t len = (uint32_t)sz + 7;
            uint8_t h[7] = {0xFF,
                            0xF1,
                            a.b2,
                            (uint8_t)(((a.chan & 3) << 6) | (len >> 11)),
                            (uint8_t)((len >> 3) & 0xFF),
                            (uint8_t)(((len & 7) << 5) | 0x1F),
                            0xFC};
            out->add(h, 7, myGen);
            size_t ro = off % RING_CAP;
            size_t first = RING_CAP - ro;
            if (sz <= first) {
                out->add(ring + ro, sz, myGen);
            } else {
                out->add(ring + ro, first, myGen);
                out->add(ring, sz - first, myGen);
            }
            off += sz;
            s = i + 1;
            aacNext = s;
            sent++;
            if (out->bad) break;
        }
        fs += spc;
        c++;
    }
    if (!out->bad) out->flush(myGen);
    delete out;
    client.stop();
    Serial.printf("[yt] aac: client closed after %u samples, next %u of %u\n", (unsigned)sent, (unsigned)aacNext,
                  (unsigned)aac.stszCount);
}

void serveRelay(WiFiClient& client) {
    client.setTimeout(3);
    char line[180];
    char range[80] = "";
    char request[100] = "";
    bool firstLine = true;
    bool clientRange = false;
    int n = 0;
    uint32_t last = millis();
    while (client.connected() && millis() - last < 2000) {
        if (!client.available()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        char c = client.read();
        last = millis();
        if (c != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = c;
            continue;
        }
        line[n] = 0;
        if (n == 0 || (n == 1 && line[0] == '\r')) break;
        if (firstLine) {
            strlcpy(request, line, sizeof(request));
            firstLine = false;
        }
        if (strncasecmp(line, "Range:", 6) == 0) {
            const char* r = line + 6;
            while (*r == ' ') r++;
            strlcpy(range, r, sizeof(range));
            char* cr = strchr(range, '\r');
            if (cr) *cr = 0;
            clientRange = range[0] != 0;
        }
        n = 0;
    }
    const uint32_t myGen = relayGen;
    unsigned reqGen = 0;
    const char* slashYt = strstr(request, "/yt/");
    if (slashYt) reqGen = strtoul(slashYt + 4, nullptr, 10);
    if (!relayGo || !playedUrl || !playedUrl[0] || reqGen != myGen) {
        Serial.printf("[yt] relay refused '%s' (current %u)\n", request, (unsigned)myGen);
        client.print("HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }
    Serial.printf("[yt] relay request '%s' range '%s'\n", request, range);
    if (strncmp(request, "HEAD", 4) == 0) {
        client.printf("HTTP/1.0 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                      ADTS_MODE ? "audio/aac" : "audio/mp4");
        client.stop();
        return;
    }
    if (ADTS_MODE) {
        serveAac(client, myGen);
        return;
    }
    // Wait until the fetcher has taken this stream and learned the file size.
    uint32_t waitStart = millis();
    while (relayGen == myGen && (fetchGen != myGen || (!totalSize && !fetchEof && !fetchFailed)) &&
           millis() - waitStart < 25000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (relayGen != myGen || fetchGen != myGen || fetchFailed || !totalSize) {
        Serial.println("[yt] relay no data");
        client.print("HTTP/1.0 502 Bad Gateway\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }
    const size_t total = totalSize;
    size_t pos = 0;
    if (clientRange) {
        const char* eq = strchr(range, '=');
        if (eq) pos = strtoul(eq + 1, nullptr, 10);
    }
    if (pos >= total) {
        client.print("HTTP/1.1 416 Range Not Satisfiable\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }
    if (pos > 0) {
        client.printf("HTTP/1.1 206 Partial Content\r\nContent-Type: audio/mp4\r\n"
                      "Accept-Ranges: bytes\r\nConnection: close\r\n"
                      "Content-Range: bytes %u-%u/%u\r\nContent-Length: %u\r\n\r\n",
                      (unsigned)pos, (unsigned)(total - 1), (unsigned)total, (unsigned)(total - pos));
    } else {
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: audio/mp4\r\nAccept-Ranges: bytes\r\n"
                      "Connection: close\r\nContent-Length: %u\r\n\r\n",
                      (unsigned)total);
    }
    const size_t first = pos;
    uint32_t progress = millis();
    bool inRing = false;  // reading from the ring (at most one client may move the ring)
    bool proxied = false;
    while (client.connected() && relayGen == myGen && pos < total) {
        size_t fill = fillPos;
        size_t validStart = fill > RING_CAP - FETCH_CHUNK ? fill - (RING_CAP - FETCH_CHUNK) : 0;
        bool avail = pos >= validStart && pos <= fill + 4 * FETCH_CHUNK;
        if (!inRing && !avail && pos < headFilled) {
            // An index read near the start of the file: answer from the kept copy, the ring stays put.
            if (!proxied) {
                proxied = true;
                Serial.printf("[yt] relay head read from %u\n", (unsigned)pos);
            }
            size_t n = headFilled - pos;
            if (n > 8192) n = 8192;
            size_t w = client.write(headCache + pos, n);
            if (w == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            pos += w;
            progress = millis();
            continue;
        }
        if (!inRing) {
            if (avail) {
                inRing = true;
                __sync_fetch_and_add(&ringClients, 1);
                if (pos > headPos) headPos = pos;
            } else if (ringClients == 0) {
                // Nobody else is reading the ring: restart the fetcher at this position.
                inRing = true;
                __sync_fetch_and_add(&ringClients, 1);
                uint32_t epoch = fetchEpoch;
                seekReq = (int32_t)pos;
                uint32_t w0 = millis();
                while (relayGen == myGen && fetchEpoch == epoch && millis() - w0 < 15000) vTaskDelay(pdMS_TO_TICKS(5));
                progress = millis();
                continue;
            }
        } else if (!avail) {
            if (ringClients > 1) {
                // Another client owns the ring now. Leave it and read straight from YouTube.
                inRing = false;
                __sync_fetch_and_sub(&ringClients, 1);
            } else {
                uint32_t epoch = fetchEpoch;
                seekReq = (int32_t)pos;
                uint32_t w0 = millis();
                while (relayGen == myGen && fetchEpoch == epoch && millis() - w0 < 15000) vTaskDelay(pdMS_TO_TICKS(5));
                progress = millis();
                continue;
            }
        }
        if (!inRing) {
            // The speaker opened a second connection somewhere the ring does not cover (it re-reads the
            // MP4 index while playing). Serve it with its own ranged requests so the ring keeps going.
            if (!proxied) {
                proxied = true;
                Serial.printf("[yt] relay side read from %u\n", (unsigned)pos);
            }
            if (!proxyChunk(client, myGen, pos, total)) break;
            progress = millis();
            continue;
        }
        if (pos >= fill) {
            if (fetchEof || fetchFailed || millis() - progress > 25000) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t off = pos % RING_CAP;
        size_t n = fill - pos;
        if (n > RING_CAP - off) n = RING_CAP - off;
        if (n > 8192) n = 8192;
        size_t w = client.write(ring + off, n);
        if (w == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        pos += w;
        if (ringClients <= 1 || pos > headPos) headPos = pos;
        progress = millis();
    }
    if (inRing) __sync_fetch_and_sub(&ringClients, 1);
    client.stop();
    Serial.printf("[yt] relay client closed at %u of %u (from %u, buffered to %u)\n", (unsigned)pos,
                  (unsigned)total, (unsigned)first, (unsigned)fillPos);
}

void clientTask(void* arg) {
    WiFiClient* c = (WiFiClient*)arg;
    serveRelay(*c);
    delete c;
    __sync_fetch_and_sub(&activeClients, 1);
    vTaskDelete(nullptr);
}

void relayTask(void*) {
    WiFiServer server(8080);
    bool begun = false;
    for (;;) {
        if (!begun) {
            if (WiFi.status() != WL_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            server.begin();
            begun = true;
            Serial.println("[yt] relay listening");
        }
        WiFiClient client = server.accept();
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // The speaker may hold one connection for the music and open more for the MP4 index, and it
        // gives up on all of them if one is left unanswered. So every connection gets its own task.
        if (activeClients >= 4) {
            client.print("HTTP/1.0 503 Busy\r\nConnection: close\r\n\r\n");
            client.stop();
            continue;
        }
        WiFiClient* copy = new WiFiClient(client);
        __sync_fetch_and_add(&activeClients, 1);
        if (xTaskCreatePinnedToCore(clientTask, "ytc", 24 * 1024, copy, 1, nullptr, 0) != pdPASS) {
            __sync_fetch_and_sub(&activeClients, 1);
            copy->stop();
            delete copy;
            Serial.println("[yt] relay could not start a client task");
        }
    }
}

bool startRelay(const char* title) {
    if (!playedUrl || !playedUrl[0] || !net::localIp()[0]) return false;
    relayGo = true;
    relayGen = relayGen + 1;
    char local[96];
    snprintf(local, sizeof(local), "http://%s:8080/yt/%u", net::localIp(), (unsigned)relayGen);
    Serial.printf("[yt] relay %s\n", local);
    return sonos::playDirect(local, title && title[0] ? title : "YouTube");
}

void finishSearch() {
    lock();
    published.loading = false;
    unlock();
}

void runSearch(bool more) {
    if (!ensureBuf()) {
        finishSearch();
        return;
    }
    if (more && !continuation[0]) {
        finishSearch();
        return;
    }
    setStatus(more ? "Loading more" : "Searching");
    char* payload = (char*)heap_caps_malloc(1600, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
        setStatus("Out of memory");
        finishSearch();
        return;
    }
    const char* name = "ANDROID";
    const char* version = "21.26.364";
    const char* ua = kClients[2].ua;
    const char* extra = kClients[2].extra;
    char escaped[700];
    if (more) {
        jsonEscape(continuation, escaped, sizeof(escaped));
        snprintf(payload, 1600,
                 "{\"context\":{\"client\":{\"clientName\":\"%s\",\"clientVersion\":\"%s\","
                 "\"hl\":\"en\",\"gl\":\"US\",%s}},\"continuation\":\"%s\"}",
                 name, version, extra, escaped);
    } else {
        continuation[0] = 0;
        lock();
        published.count = 0;
        published.more = false;
        char query[64];
        strlcpy(query, published.query, sizeof(query));
        unlock();
        jsonEscape(query, escaped, sizeof(escaped));
        snprintf(payload, 1600,
                 "{\"context\":{\"client\":{\"clientName\":\"%s\",\"clientVersion\":\"%s\","
                 "\"hl\":\"en\",\"gl\":\"US\",%s}},\"query\":\"%s\"}",
                 name, version, extra, escaped);
    }
    char url[160];
    snprintf(url, sizeof(url), "https://www.youtube.com/youtubei/v1/search?prettyPrint=false&key=%s", API_KEY);
    Body out;
    int code = postJson(url, payload, ua, out);
    heap_caps_free(payload);
    Serial.printf("[yt] search %d bytes %u overflow %d\n", code, (unsigned)out.n, out.overflow);
    if (code != 200 || out.n < 20) {
        setStatus("YouTube busy");
        finishSearch();
        return;
    }
    lock();
    published.more = false;
    unlock();
    continuation[0] = 0;
    scanSearch(out.n);
    if (published.count == 0) Serial.printf("[yt] search head %.160s\n", httpBuf);
    lock();
    int n = published.count;
    published.loading = false;
    if (n == 0) strlcpy(published.status, "Nothing found", sizeof(published.status));
    else published.status[0] = 0;
    char age0[16] = "";
    char views0[16] = "";
    if (n > 0) {
        strlcpy(age0, published.items[0].age, sizeof(age0));
        strlcpy(views0, published.items[0].views, sizeof(views0));
    }
    unlock();
    Serial.printf("[yt] search rows %d more %d age '%s' views '%s'\n", n, continuation[0] != 0, age0, views0);
}

// ---- Starred videos -------------------------------------------------------------------------
struct Thumb {
    char id[16];
    uint8_t* data;
    size_t len;
    uint32_t retryAt;
};
constexpr int MAX_THUMBS = MAX_STARS + MAX_ITEMS;
Thumb thumbs[MAX_THUMBS];
bool starsDirty = false;

class ThumbSink : public Stream {
   public:
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t n = 0;
    size_t write(const uint8_t* data, size_t len) override {
        size_t room = n < cap ? cap - n : 0;
        size_t take = len < room ? len : room;
        if (take) memcpy(buf + n, data, take);
        n += take;
        return len;
    }
    size_t write(uint8_t b) override { return write(&b, 1); }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

static_assert(offsetof(Item, seconds) + sizeof(int) == 148, "legacy starred record size");

void loadStars() {
    Preferences prefs;
    if (!prefs.begin("ytstar", true)) return;
    int n = prefs.getInt("n", 0);
    if (n > MAX_STARS) n = MAX_STARS;
    int got = 0;
    for (int i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof(key), "s%d", i);
        Item it{};
        // Stars saved before age and views were added are the bytes up through seconds.
        constexpr size_t kLegacyItem = 148;
        size_t bytes = prefs.getBytes(key, &it, sizeof(it));
        if (bytes != sizeof(it) && bytes != kLegacyItem) continue;
        it.id[sizeof(it.id) - 1] = 0;
        it.title[sizeof(it.title) - 1] = 0;
        it.channel[sizeof(it.channel) - 1] = 0;
        if (!videoIdOk(it.id)) continue;
        cleanText(it.title);
        cleanText(it.channel);
        published.stars[got++] = it;
    }
    published.starCount = got;
    prefs.end();
    if (got) Serial.printf("[yt] %d starred\n", got);
}

void saveStars() {
    Item copy[MAX_STARS];
    int n;
    lock();
    n = published.starCount;
    memcpy(copy, published.stars, sizeof(Item) * n);
    starsDirty = false;
    unlock();
    Preferences prefs;
    if (!prefs.begin("ytstar", false)) return;
    prefs.putInt("n", n);
    for (int i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof(key), "s%d", i);
        prefs.putBytes(key, &copy[i], sizeof(Item));
    }
    prefs.end();
    Serial.printf("[yt] saved %d starred\n", n);
}

bool fetchThumb(Thumb& t) {
    constexpr size_t TMP_CAP = 96 * 1024;
    uint8_t* tmp = (uint8_t*)heap_caps_malloc(TMP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return false;
    char url[96];
    snprintf(url, sizeof(url), "http://i.ytimg.com/vi/%s/mqdefault.jpg", t.id);
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    bool ok = false;
    if (http.begin(client, url)) {
        int code = http.GET();
        ThumbSink sink;
        sink.buf = tmp;
        sink.cap = TMP_CAP;
        if (code == 200) http.writeToStream(&sink);
        http.end();
        if (code == 200 && sink.n > 2 && tmp[0] == 0xFF && tmp[1] == 0xD8) {
            uint8_t* keep = (uint8_t*)heap_caps_malloc(sink.n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (keep) {
                memcpy(keep, tmp, sink.n);
                lock();
                heap_caps_free(t.data);
                t.data = keep;
                t.len = sink.n;
                unlock();
                ok = true;
                Serial.printf("[yt] thumbnail %s %u bytes\n", t.id, (unsigned)sink.n);
            }
        } else {
            Serial.printf("[yt] thumb %s HTTP %d (%u bytes)\n", t.id, code, (unsigned)sink.n);
        }
    }
    heap_caps_free(tmp);
    return ok;
}

// Network task, when nothing else is running: save changes and download missing thumbnails.
// Starred videos come first, then the videos on the current search page.
void serviceStars() {
    if (starsDirty) saveStars();
    char ids[MAX_THUMBS][16];
    int n = 0;
    lock();
    for (int i = 0; i < published.starCount && n < MAX_THUMBS; i++)
        strlcpy(ids[n++], published.stars[i].id, sizeof(ids[0]));
    for (int i = 0; i < published.count && n < MAX_THUMBS; i++) {
        bool seen = false;
        for (int j = 0; j < n; j++)
            if (!strcmp(ids[j], published.items[i].id)) seen = true;
        if (seen || !published.items[i].id[0]) continue;
        strlcpy(ids[n++], published.items[i].id, sizeof(ids[0]));
    }
    unlock();
    for (Thumb& t : thumbs) {
        if (!t.id[0]) continue;
        bool keep = false;
        for (int i = 0; i < n; i++)
            if (!strcmp(ids[i], t.id)) keep = true;
        if (keep) continue;
        lock();
        heap_caps_free(t.data);
        t.data = nullptr;
        t.len = 0;
        t.id[0] = 0;
        unlock();
    }
    int fetched = 0;
    for (int i = 0; i < n; i++) {
        Thumb* slot = nullptr;
        for (Thumb& t : thumbs)
            if (!strcmp(t.id, ids[i])) slot = &t;
        if (!slot) {
            for (Thumb& t : thumbs) {
                if (t.id[0]) continue;
                lock();
                strlcpy(t.id, ids[i], sizeof(t.id));
                t.data = nullptr;
                t.len = 0;
                t.retryAt = 0;
                unlock();
                slot = &t;
                break;
            }
        }
        if (!slot || slot->len || (int32_t)(millis() - slot->retryAt) < 0) continue;
        if (!fetchThumb(*slot)) slot->retryAt = millis() + 20000;
        if (++fetched >= 3) return;
    }
}

void loadClient() {
    Preferences prefs;
    if (!prefs.begin("yt", true)) return;
    strlcpy(savedName, prefs.getString("name", "").c_str(), sizeof(savedName));
    strlcpy(savedVersion, prefs.getString("ver", "").c_str(), sizeof(savedVersion));
    strlcpy(savedUa, prefs.getString("ua", "").c_str(), sizeof(savedUa));
    prefs.end();
    if (savedName[0]) Serial.printf("[yt] saved client %s %s\n", savedName, savedVersion);
}

}  // namespace

void begin() {
    mu = xSemaphoreCreateMutex();
    memset(&published, 0, sizeof(published));
    strlcpy(published.heading, "YouTube", sizeof(published.heading));
    loadClient();
    loadStars();
    ring = (uint8_t*)heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fetchUrl = (char*)heap_caps_malloc(6500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    headCache = (uint8_t*)heap_caps_malloc(HEAD_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring || !fetchUrl) Serial.println("[yt] no memory for the relay buffer");
    if (!headCache) Serial.println("[yt] no memory for the head cache");
    xTaskCreatePinnedToCore(fetchTask, "ytfetch", 24 * 1024, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(relayTask, "ytrelay", 12 * 1024, nullptr, 1, nullptr, 0);
}

void loop(bool online) {
    if (!online) return;
    if (checkAt && (int32_t)(millis() - checkAt) >= 0) {
        checkAt = 0;
        sonos::View speaker;
        sonos::copyView(speaker);
        Serial.printf("[yt] after start playing %d dur %s title %.60s\n", speaker.playing, speaker.dur, speaker.title);
        if (!speaker.playing) setStatus("Speaker refused the stream");
    }
    if (seekAt && (int32_t)(millis() - seekAt) >= 0) {
        seekAt = 0;
        Serial.println("[yt] scheduled seek");
        sonos::seekRel("00:05:00");
    }
    char id[16];
    char query[64];
    int play = -1;
    bool resolve = false, search = false, more = false, seek = false;
    lock();
    if (wantResolve) {
        resolve = true;
        strlcpy(id, wantId, sizeof(id));
        wantResolve = false;
    } else if (wantSearch) {
        search = true;
        strlcpy(query, wantQuery, sizeof(query));
        strlcpy(published.query, query, sizeof(published.query));
        wantSearch = false;
        published.loading = true;
    } else if (wantMore) {
        more = true;
        wantMore = false;
        published.loading = true;
    } else if (wantPlay >= 0) {
        play = wantPlay;
        wantPlay = -1;
    } else if (wantSeek) {
        seek = true;
        wantSeek = false;
    }
    unlock();
    if (resolve) {
        if (!videoIdOk(id)) setStatus("Bad video id");
        else resolveId(id);
    } else if (search) {
        runSearch(false);
    } else if (more) {
        runSearch(true);
    } else if (play >= 0) {
        lock();
        bool ok = play >= 0 && play < published.count;
        if (ok) strlcpy(id, published.items[play].id, sizeof(id));
        unlock();
        if (ok) resolveId(id);
    } else if (seek) {
        sonos::seekRel("00:05:00");
    }
    if (!(resolve || search || more || play >= 0 || seek)) serviceStars();
}

void requestResolve(const char* videoId) {
    if (!videoIdOk(videoId)) return;
    lock();
    strlcpy(wantId, videoId, sizeof(wantId));
    wantResolve = true;
    unlock();
}

void requestSearch(const char* query) {
    if (!query || !query[0]) return;
    lock();
    strlcpy(wantQuery, query, sizeof(wantQuery));
    wantSearch = true;
    published.loading = true;
    published.status[0] = 0;
    unlock();
}

void requestMore() {
    lock();
    wantMore = true;
    unlock();
}

void requestPlay(int index) {
    lock();
    wantPlay = index;
    unlock();
}

void requestSeek() {
    lock();
    wantSeek = true;
    unlock();
}

void setClient(const char* name, const char* version, const char* ua) {
    if (!name || !version || !name[0] || !version[0]) return;
    strlcpy(savedName, name, sizeof(savedName));
    strlcpy(savedVersion, version, sizeof(savedVersion));
    strlcpy(savedUa, ua ? ua : "", sizeof(savedUa));
    Preferences prefs;
    if (!prefs.begin("yt", false)) return;
    prefs.putString("name", savedName);
    prefs.putString("ver", savedVersion);
    prefs.putString("ua", savedUa);
    prefs.end();
    Serial.printf("[yt] client set %s %s\n", savedName, savedVersion);
}

void copyView(View& out) {
    lock();
    out = published;
    for (int i = 0; i < out.starCount; i++) {
        out.starThumb[i] = false;
        for (const Thumb& t : thumbs)
            if (t.len && !strcmp(t.id, out.stars[i].id)) out.starThumb[i] = true;
    }
    unlock();
}

void toggleStar(const char* id, const char* title, const char* channel) {
    if (!videoIdOk(id)) return;
    bool full = false;
    lock();
    int count = published.starCount;
    int at = -1;
    for (int i = 0; i < count; i++)
        if (!strcmp(published.stars[i].id, id)) at = i;
    if (at >= 0) {
        memmove(&published.stars[at], &published.stars[at + 1], sizeof(Item) * (count - at - 1));
        published.starCount = count - 1;
        starsDirty = true;
    } else if (count >= MAX_STARS) {
        full = true;
    } else {
        memmove(&published.stars[1], &published.stars[0], sizeof(Item) * count);
        Item& it = published.stars[0];
        memset(&it, 0, sizeof(it));
        strlcpy(it.id, id, sizeof(it.id));
        strlcpy(it.title, title && title[0] ? title : "YouTube", sizeof(it.title));
        strlcpy(it.channel, channel ? channel : "", sizeof(it.channel));
        published.starCount = count + 1;
        starsDirty = true;
    }
    unlock();
    if (full) setStatus("Starred list is full");
}

void starPlaying() {
    char id[16], title[80], channel[48];
    lock();
    strlcpy(id, published.playingId, sizeof(id));
    strlcpy(title, published.playingTitle, sizeof(title));
    strlcpy(channel, published.playingChannel, sizeof(channel));
    unlock();
    toggleStar(id, title, channel);
}

void playStar(int index) {
    lock();
    if (index >= 0 && index < published.starCount) {
        strlcpy(wantId, published.stars[index].id, sizeof(wantId));
        wantResolve = true;
    }
    unlock();
}

size_t copyThumb(const char* id, uint8_t* dst, size_t cap) {
    size_t n = 0;
    lock();
    for (const Thumb& t : thumbs) {
        if (t.len && t.len <= cap && !strcmp(t.id, id)) {
            memcpy(dst, t.data, t.len);
            n = t.len;
            break;
        }
    }
    unlock();
    return n;
}

}  // namespace yt
