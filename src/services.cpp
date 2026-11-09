#include "services.h"

#include "sonos.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace svc {
namespace {

constexpr size_t HTTP_CAP = 48 * 1024;

SemaphoreHandle_t mu = nullptr;
char* httpBuf = nullptr;
View published;
char clientId[48] = "";
char refresh[400] = "";
char secret[80] = "";
char access[400] = "";
uint32_t accessUntil = 0;

bool wantProbe = false;
bool wantSave = false;
bool wantClear = false;
bool wantList = false;
char wantQuery[64] = "";
int wantOffset = 0;
int wantPlay = -1;
bool wantAppend = false;
char saveId[48] = "";
char saveRefresh[400] = "";
char saveSecret[80] = "";

void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mu); }

void setStatus(const char* text) {
    lock();
    strlcpy(published.status, text ? text : "", sizeof(published.status));
    published.loading = false;
    unlock();
}

void urlEncode(const char* in, char* out, size_t cap) {
    static const char* hex = "0123456789ABCDEF";
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[j++] = (char)c;
        else if (j + 4 < cap) {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = 0;
}

void xmlEscape(const char* in, char* out, size_t cap) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        const char* rep = nullptr;
        if (in[i] == '&') rep = "&amp;";
        else if (in[i] == '<') rep = "&lt;";
        else if (in[i] == '>') rep = "&gt;";
        else if (in[i] == '"') rep = "&quot;";
        if (!rep) {
            out[j++] = in[i];
            continue;
        }
        size_t n = strlen(rep);
        if (j + n >= cap) break;
        memcpy(out + j, rep, n);
        j += n;
    }
    out[j] = 0;
}

void b64(const char* in, char* out, size_t cap) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in);
    size_t j = 0;
    for (size_t i = 0; i < n && j + 4 < cap; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[j++] = t[(v >> 18) & 63];
        out[j++] = t[(v >> 12) & 63];
        out[j++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[j] = 0;
}

class Body : public Stream {
   public:
    char* buf = nullptr;
    size_t cap = 0;
    size_t n = 0;
    size_t write(const uint8_t* data, size_t size) override {
        size_t room = n + 1 < cap ? cap - 1 - n : 0;
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

int https(const char* url, const char* method, const char* contentType, const char* payload, const char* bearer) {
    if (!httpBuf) return -1;
    httpBuf[0] = 0;
    WiFiClientSecure client;
    client.useBuiltinCACertBundle();
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) return -1;
    if (contentType && contentType[0]) http.addHeader("Content-Type", contentType);
    if (bearer && bearer[0]) http.addHeader("Authorization", bearer);
    int code = -1;
    if (!strcmp(method, "POST")) code = http.POST((uint8_t*)(payload ? payload : ""), payload ? strlen(payload) : 0);
    else code = http.GET();
    if (code > 0) {
        Body body;
        body.buf = httpBuf;
        body.cap = HTTP_CAP;
        http.writeToStream(&body);
        httpBuf[body.n < HTTP_CAP ? body.n : HTTP_CAP - 1] = 0;
    }
    http.end();
    return code;
}

void loadCredentials() {
    Preferences prefs;
    if (!prefs.begin("svc", true)) return;
    prefs.getString("sp_id", clientId, sizeof(clientId));
    prefs.getString("sp_rt", refresh, sizeof(refresh));
    prefs.getString("sp_sec", secret, sizeof(secret));
    prefs.end();
}

void saveCredentials() {
    Preferences prefs;
    if (!prefs.begin("svc", false)) return;
    prefs.putString("sp_id", clientId);
    prefs.putString("sp_rt", refresh);
    prefs.putString("sp_sec", secret);
    prefs.end();
}

bool refreshAccess() {
    if (!clientId[0] || !refresh[0]) return false;
    char encR[800];
    char encId[96];
    urlEncode(refresh, encR, sizeof(encR));
    urlEncode(clientId, encId, sizeof(encId));
    char form[1000];
    snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s", encR, encId);
    char auth[180] = "";
    if (secret[0]) {
        char raw[140];
        char coded[200];
        snprintf(raw, sizeof(raw), "%s:%s", clientId, secret);
        b64(raw, coded, sizeof(coded));
        snprintf(auth, sizeof(auth), "Basic %s", coded);
    }
    int code = https("https://accounts.spotify.com/api/token", "POST", "application/x-www-form-urlencoded", form, auth);
    Serial.printf("[svc] token HTTP %d\n", code);
    if (code != 200) {
        setStatus(code == 400 ? "Spotify sign-in expired" : "Spotify sign-in failed");
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf)) return false;
    const char* token = doc["access_token"] | "";
    int seconds = doc["expires_in"] | 3600;
    const char* next = doc["refresh_token"] | "";
    if (!token[0]) return false;
    strlcpy(access, token, sizeof(access));
    if (next[0]) {
        strlcpy(refresh, next, sizeof(refresh));
        saveCredentials();
    }
    if (seconds < 120) seconds = 120;
    accessUntil = millis() + (uint32_t)(seconds - 60) * 1000;
    return true;
}

bool ensureAccess() {
    if (access[0] && (int32_t)(millis() - accessUntil) < 0) return true;
    return refreshAccess();
}

void addItem(Kind kind, const char* id, const char* title, const char* subtitle) {
    if (!id || !id[0] || published.count >= MAX_ITEMS) return;
    Item& it = published.items[published.count++];
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    strlcpy(it.id, id, sizeof(it.id));
    strlcpy(it.title, title && title[0] ? title : "Untitled", sizeof(it.title));
    strlcpy(it.subtitle, subtitle ? subtitle : "", sizeof(it.subtitle));
}

const char* firstArtist(JsonObject item) {
    JsonArray artists = item["artists"].as<JsonArray>();
    if (!artists.isNull() && artists.size() > 0) return artists[0]["name"] | "";
    return "";
}

bool spotifyGet(const char* url) {
    if (!ensureAccess()) return false;
    char auth[460];
    snprintf(auth, sizeof(auth), "Bearer %s", access);
    int code = https(url, "GET", "", "", auth);
    Serial.printf("[svc] GET %d %s\n", code, url);
    if (code == 401) {
        access[0] = 0;
        if (!ensureAccess()) return false;
        snprintf(auth, sizeof(auth), "Bearer %s", access);
        code = https(url, "GET", "", "", auth);
    }
    if (code == 429) {
        setStatus("Spotify is busy, try again");
        return false;
    }
    if (code == 403) {
        setStatus("Spotify will not open that");
        return false;
    }
    if (code != 200) {
        setStatus("Spotify did not answer");
        return false;
    }
    return true;
}

void takePage(JsonArray items, Kind kind, int limit) {
    int n = 0;
    if (items.isNull()) return;
    for (JsonObject item : items) {
        if (n >= limit || published.count >= MAX_ITEMS) break;
        if (item.isNull()) continue;
        const char* id = item["id"] | "";
        const char* name = item["name"] | "";
        const char* sub = kind == Kind::Playlist ? item["owner"]["display_name"] | "" : firstArtist(item);
        if (kind == Kind::Artist) sub = "Artist";
        addItem(kind, id, name, sub);
        n++;
    }
}

// Spotify provider. A second service implements the same three steps.
bool spotifySignedIn() { return clientId[0] && refresh[0]; }

void spotifyLoad(bool search, const char* query, int offset) {
    if (!spotifySignedIn()) {
        setStatus("Sign in with tools/spotify_auth.py");
        return;
    }
    lock();
    published.loading = true;
    published.count = 0;
    published.offset = offset < 0 ? 0 : offset;
    published.more = false;
    strlcpy(published.query, search ? (query ? query : "") : "", sizeof(published.query));
    strlcpy(published.heading, search ? "Search" : "My playlists", sizeof(published.heading));
    unlock();
    char url[400];
    if (!search) {
        snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/playlists?limit=10&offset=%d", published.offset);
    } else {
        char q[180];
        urlEncode(query ? query : "", q, sizeof(q));
        snprintf(url, sizeof(url),
                 "https://api.spotify.com/v1/search?q=%s&type=track,album,playlist,artist&limit=5&offset=%d", q,
                 published.offset);
    }
    if (!spotifyGet(url)) return;
    JsonDocument filter;
    if (!search) {
        filter["items"][0]["id"] = true;
        filter["items"][0]["name"] = true;
        filter["items"][0]["owner"]["display_name"] = true;
        filter["next"] = true;
    } else {
        filter["tracks"]["items"][0]["id"] = true;
        filter["tracks"]["items"][0]["name"] = true;
        filter["tracks"]["items"][0]["artists"][0]["name"] = true;
        filter["albums"]["items"][0]["id"] = true;
        filter["albums"]["items"][0]["name"] = true;
        filter["albums"]["items"][0]["artists"][0]["name"] = true;
        filter["playlists"]["items"][0]["id"] = true;
        filter["playlists"]["items"][0]["name"] = true;
        filter["playlists"]["items"][0]["owner"]["display_name"] = true;
        filter["artists"]["items"][0]["id"] = true;
        filter["artists"]["items"][0]["name"] = true;
        filter["tracks"]["next"] = true;
    }
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) {
        setStatus("Could not read Spotify");
        return;
    }
    lock();
    if (!search) {
        takePage(doc["items"].as<JsonArray>(), Kind::Playlist, 10);
        published.more = !doc["next"].isNull();
    } else {
        takePage(doc["tracks"]["items"].as<JsonArray>(), Kind::Track, 5);
        takePage(doc["albums"]["items"].as<JsonArray>(), Kind::Album, 5);
        takePage(doc["playlists"]["items"].as<JsonArray>(), Kind::Playlist, 5);
        takePage(doc["artists"]["items"].as<JsonArray>(), Kind::Artist, 5);
        published.more = !doc["tracks"]["next"].isNull();
    }
    published.loading = false;
    published.signedIn = true;
    if (published.count == 0) strlcpy(published.status, search ? "No matches" : "No playlists", sizeof(published.status));
    else published.status[0] = 0;
    unlock();
    Serial.printf("[svc] %d items\n", published.count);
}

// Sonos addresses, from SoCo's ShareLink table. The fallback is the form this
// household already plays from a saved favorite.
bool spotifyDidl(const Item& item, char* uri, size_t uriCap, char* meta, size_t metaCap, bool favoriteForm) {
    const char* word = "track";
    const char* cls = "object.item.audioItem.musicTrack";
    const char* prefix = "";
    const char* key = "00032020";
    if (item.kind == Kind::Album) {
        word = "album";
        cls = "object.container.album.musicAlbum";
        prefix = "x-rincon-cpcontainer:1004206c";
        key = "1004206c";
    } else if (item.kind == Kind::Playlist) {
        word = "playlist";
        cls = "object.container.playlistContainer";
        prefix = "x-rincon-cpcontainer:1006206c";
        key = "1006206c";
    }
    char encoded[80];
    snprintf(encoded, sizeof(encoded), "spotify%%3a%s%%3a%s", word, item.id);
    if (!favoriteForm) {
        snprintf(uri, uriCap, "%s%s", prefix, encoded);
    } else if (item.kind == Kind::Track) {
        int sn = sonos::spotifyAccount();
        if (sn < 1) sn = 1;
        snprintf(uri, uriCap, "x-sonos-spotify:%s?sid=9&flags=8232&sn=%d", encoded, sn);
        key = "10032020";
    } else {
        int sn = sonos::spotifyAccount();
        if (sn < 1) sn = 1;
        snprintf(uri, uriCap, "%s%s?sid=9&flags=8300&sn=%d", prefix, encoded, sn);
    }
    char title[160];
    xmlEscape(item.title, title, sizeof(title));
    char id[120];
    snprintf(id, sizeof(id), "%s%s", key, encoded);
    int wrote = snprintf(meta, metaCap,
                         "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                         "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                         "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
                         "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
                         "<item id=\"%s\" parentID=\"-1\" restricted=\"true\">"
                         "<dc:title>%s</dc:title><upnp:class>%s</upnp:class>"
                         "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
                         "SA_RINCON2311_X_#Svc2311-0-Token</desc>",
                         id, title, cls);
    if (wrote < 0 || (size_t)wrote >= metaCap) return false;
    if (favoriteForm && item.kind == Kind::Track) {
        size_t used = strlen(meta);
        snprintf(meta + used, metaCap - used,
                 "<res protocolInfo=\"sonos.com-spotify:*:audio/x-spotify:*\">%s</res>", uri);
    }
    strlcat(meta, "</item></DIDL-Lite>", metaCap);
    return true;
}

bool spotifyPlay(const Item& item, bool append) {
    if (item.kind == Kind::Artist) {
        char q[80];
        snprintf(q, sizeof(q), "artist:\"%s\"", item.title);
        spotifyLoad(true, q, 0);
        return true;
    }
    char uri[320];
    char meta[1200];
    if (!spotifyDidl(item, uri, sizeof(uri), meta, sizeof(meta), false)) return false;
    Serial.printf("[svc] play %s\n", uri);
    if (sonos::enqueue(uri, meta, !append)) return true;
    Serial.println("[svc] share form rejected, trying favorite form");
    if (!spotifyDidl(item, uri, sizeof(uri), meta, sizeof(meta), true)) return false;
    return sonos::enqueue(uri, meta, !append);
}

void probe() {
    const char* hosts[] = {"https://api.spotify.com/v1/", "https://accounts.spotify.com/"};
    for (const char* url : hosts) {
        int code = https(url, "GET", "", "", "");
        Serial.printf("[svc] probe %s HTTP %d  heap %u  psram %u  stack %u\n", url, code,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

}  // namespace

void begin() {
    mu = xSemaphoreCreateMutex();
    httpBuf = (char*)heap_caps_malloc(HTTP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memset(&published, 0, sizeof(published));
    strlcpy(published.heading, "My playlists", sizeof(published.heading));
    loadCredentials();
    published.signedIn = clientId[0] && refresh[0];
    if (!published.signedIn) strlcpy(published.status, "Sign in with tools/spotify_auth.py", sizeof(published.status));
    Serial.printf("[svc] spotify %s\n", published.signedIn ? "signed in" : "not signed in");
}

void loop(bool online) {
    if (!mu) return;
    bool probeNow = false, saveNow = false, clearNow = false, listNow = false;
    char query[64];
    int offset = 0;
    int play = -1;
    bool append = false;
    query[0] = 0;
    lock();
    probeNow = wantProbe;
    wantProbe = false;
    saveNow = wantSave;
    wantSave = false;
    if (saveNow) {
        strlcpy(clientId, saveId, sizeof(clientId));
        strlcpy(refresh, saveRefresh, sizeof(refresh));
        strlcpy(secret, saveSecret, sizeof(secret));
        access[0] = 0;
    }
    clearNow = wantClear;
    wantClear = false;
    listNow = wantList;
    wantList = false;
    strlcpy(query, wantQuery, sizeof(query));
    offset = wantOffset;
    play = wantPlay;
    wantPlay = -1;
    append = wantAppend;
    Item item;
    bool haveItem = false;
    if (play >= 0 && play < published.count) {
        item = published.items[play];
        haveItem = true;
    }
    unlock();

    if (saveNow) {
        saveCredentials();
        published.signedIn = true;
        setStatus("Spotify signed in");
        Serial.println("[svc] credentials stored");
    }
    if (clearNow) {
        clientId[0] = refresh[0] = secret[0] = access[0] = 0;
        saveCredentials();
        lock();
        published.signedIn = false;
        published.count = 0;
        unlock();
        setStatus("Spotify signed out");
    }
    if (!online) return;
    if (probeNow) probe();
    if (haveItem) {
        if (!spotifyPlay(item, append)) setStatus("Could not play that");
    } else if (listNow) {
        spotifyLoad(query[0] != 0, query, offset);
    }
}

void storeCredentials(const char* provider, const char* id, const char* token, const char* sec) {
    if (!provider || strcmp(provider, "spotify") || !mu) return;
    lock();
    strlcpy(saveId, id ? id : "", sizeof(saveId));
    strlcpy(saveRefresh, token ? token : "", sizeof(saveRefresh));
    strlcpy(saveSecret, sec ? sec : "", sizeof(saveSecret));
    wantSave = true;
    unlock();
}

void clearCredentials(const char* provider) {
    if (!provider || strcmp(provider, "spotify") || !mu) return;
    lock();
    wantClear = true;
    unlock();
}

void requestProbe() {
    if (!mu) return;
    lock();
    wantProbe = true;
    unlock();
}

void requestPlaylists(int offset) {
    if (!mu) return;
    lock();
    wantQuery[0] = 0;
    wantOffset = offset;
    wantList = true;
    published.loading = true;
    unlock();
}

void requestSearch(const char* query, int offset) {
    if (!mu || !query) return;
    lock();
    strlcpy(wantQuery, query, sizeof(wantQuery));
    wantOffset = offset;
    wantList = true;
    published.loading = true;
    unlock();
}

void requestPlay(int index, bool append) {
    if (!mu) return;
    lock();
    wantPlay = index;
    wantAppend = append;
    unlock();
}

void copyView(View& out) {
    if (!mu) {
        memset(&out, 0, sizeof(out));
        return;
    }
    lock();
    out = published;
    unlock();
}

const char* providerName() { return "Spotify"; }

}  // namespace svc
