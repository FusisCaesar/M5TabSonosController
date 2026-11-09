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
        if (gotTitle[0]) strlcpy(title, gotTitle, sizeof(title));
        const char* gotAuthor = doc["videoDetails"]["author"] | "";
        if (gotAuthor[0]) strlcpy(author, gotAuthor, sizeof(author));
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

void addItem(const char* id, const char* title, const char* channel, int seconds) {
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
    strlcpy(it.id, id, sizeof(it.id));
    strlcpy(it.title, title && title[0] ? title : id, sizeof(it.title));
    strlcpy(it.channel, channel ? channel : "", sizeof(it.channel));
    it.seconds = seconds;
    unlock();
}

void takeVideo(JsonObject v) {
    const char* id = v["videoId"] | "";
    const char* title = textOf(v["title"]);
    const char* channel = textOf(v["longBylineText"]);
    if (!channel[0]) channel = textOf(v["ownerText"]);
    if (!channel[0]) channel = textOf(v["shortBylineText"]);
    const char* length = textOf(v["lengthText"]);
    addItem(id, title, channel, parseClock(length));
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
        grab(hit, window, "\"videoId\":\"", id, sizeof(id));
        if (!grab(hit, window, "\"title\":{\"runs\":[{\"text\":\"", title, sizeof(title)))
            grab(hit, window, "\"title\":{\"simpleText\":\"", title, sizeof(title));
        if (!grab(hit, window, "\"longBylineText\":{\"runs\":[{\"text\":\"", channel, sizeof(channel)))
            grab(hit, window, "\"ownerText\":{\"runs\":[{\"text\":\"", channel, sizeof(channel));
        if (!grab(hit, window, "\"lengthText\":{\"runs\":[{\"text\":\"", length, sizeof(length)))
            grab(hit, window, "\"lengthText\":{\"simpleText\":\"", length, sizeof(length));
        if (videoIdOk(id) && title[0]) addItem(id, title, channel, parseClock(length));
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
        Serial.printf("[yt] fetch to %u of %u, speaker at %u\n", (unsigned)fillPos, (unsigned)totalSize,
                      (unsigned)headPos);
    }
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
    if (strncmp(request, "HEAD", 4) == 0) {
        client.print("HTTP/1.0 200 OK\r\nContent-Type: audio/mp4\r\nConnection: close\r\n\r\n");
        client.stop();
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
    headPos = pos;
    uint32_t progress = millis();
    while (client.connected() && relayGen == myGen && pos < total) {
        size_t fill = fillPos;
        size_t validStart = fill > RING_CAP - FETCH_CHUNK ? fill - (RING_CAP - FETCH_CHUNK) : 0;
        if (pos < validStart || pos > fill + RING_CAP / 2) {
            // Outside what is buffered: ask the fetcher to restart there.
            uint32_t epoch = fetchEpoch;
            seekReq = (int32_t)pos;
            uint32_t w0 = millis();
            while (relayGen == myGen && fetchEpoch == epoch && millis() - w0 < 15000) vTaskDelay(pdMS_TO_TICKS(5));
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
        headPos = pos;
        progress = millis();
    }
    client.stop();
    Serial.printf("[yt] relay client closed at %u of %u (from %u, buffered to %u)\n", (unsigned)pos,
                  (unsigned)total, (unsigned)first, (unsigned)fillPos);
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
        serveRelay(client);
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
    unlock();
    Serial.printf("[yt] search rows %d more %d\n", n, continuation[0] != 0);
}

// ---- Starred videos -------------------------------------------------------------------------
struct Thumb {
    char id[16];
    uint8_t* data;
    size_t len;
    uint32_t retryAt;
};
Thumb thumbs[MAX_STARS];
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

void loadStars() {
    Preferences prefs;
    if (!prefs.begin("ytstar", true)) return;
    int n = prefs.getInt("n", 0);
    if (n > MAX_STARS) n = MAX_STARS;
    int got = 0;
    for (int i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof(key), "s%d", i);
        Item it;
        if (prefs.getBytes(key, &it, sizeof(it)) != sizeof(it)) continue;
        it.id[sizeof(it.id) - 1] = 0;
        it.title[sizeof(it.title) - 1] = 0;
        it.channel[sizeof(it.channel) - 1] = 0;
        if (!videoIdOk(it.id)) continue;
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
void serviceStars() {
    if (starsDirty) saveStars();
    char ids[MAX_STARS][16];
    int n;
    lock();
    n = published.starCount;
    for (int i = 0; i < n; i++) strlcpy(ids[i], published.stars[i].id, sizeof(ids[i]));
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
    static uint32_t lastFetch = 0;
    if (millis() - lastFetch < 300) return;
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
        lastFetch = millis();
        if (!fetchThumb(*slot)) slot->retryAt = millis() + 20000;
        return;
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
    if (!ring || !fetchUrl) Serial.println("[yt] no memory for the relay buffer");
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
