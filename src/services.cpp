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

constexpr size_t HTTP_CAP = 128 * 1024;

SemaphoreHandle_t mu = nullptr;
char* httpBuf = nullptr;
View published;
char clientId[48] = "";
char refresh[400] = "";
char secret[80] = "";
char access[400] = "";
uint32_t accessUntil = 0;
int spotifyHttp = 0;

bool wantProbe = false;
bool wantSave = false;
bool wantClear = false;
bool wantList = false;
bool wantPlaylists = false;
bool wantReload = false;
bool wantBack = false;
bool wantPagePlay = false;
char wantQuery[64] = "";
int wantOffset = 0;
int wantPlay = -1;
bool wantOpen = false;
Item wantOpenItem;
bool wantAppend = false;

enum class Page : uint8_t { Playlists, Search, Album, Playlist, Artist, Track };

struct Crumb {
    Page page;
    Kind kind;
    bool playable;
    char id[48];
    char title[72];
    char query[64];
    char image[IMAGE_CAP];
    int offset;
    int pageSize;
};

constexpr int MAX_DEPTH = 8;
Crumb stack[MAX_DEPTH];
int depth = 0;
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

int https(const char* url, const char* method, const char* contentType, const char* payload, const char* bearer,
          size_t* got = nullptr) {
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
        if (got) *got = body.n;
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

void addItem(Kind kind, const char* id, const char* title, const char* subtitle, const char* image = nullptr) {
    if (!id || !id[0] || published.count >= MAX_ITEMS) return;
    Item& it = published.items[published.count++];
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    strlcpy(it.id, id, sizeof(it.id));
    strlcpy(it.title, title && title[0] ? title : "Untitled", sizeof(it.title));
    strlcpy(it.subtitle, subtitle ? subtitle : "", sizeof(it.subtitle));
    if (image && image[0]) strlcpy(it.image, image, sizeof(it.image));
}

// Smallest cover Spotify offers. Rows are 90px, and the 300px and 640px files are the slow part.
const char* coverUrl(JsonVariant images) {
    JsonArray arr = images.as<JsonArray>();
    if (arr.isNull()) return "";
    const char* best = "";
    int bestW = 1000000;
    const char* fallback = "";
    for (JsonObject im : arr) {
        const char* url = im["url"] | "";
        if (!url[0] || strlen(url) >= IMAGE_CAP) continue;
        fallback = url;
        int w = im["width"] | 0;
        if (w > 0 && w < bestW) {
            best = url;
            bestW = w;
        }
    }
    return best[0] ? best : fallback;
}

const char* firstArtist(JsonObject item) {
    JsonArray artists = item["artists"].as<JsonArray>();
    if (!artists.isNull() && artists.size() > 0) return artists[0]["name"] | "";
    return "";
}

bool spotifyGet(const char* url, bool report = true) {
    spotifyHttp = 0;
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
        Serial.printf("[svc] GET %d %s\n", code, url);
    }
    spotifyHttp = code;
    if (code == 429) {
        if (report) setStatus("Spotify is busy, try again");
        return false;
    }
    if (code == 403) {
        if (report) setStatus("Spotify will not open that");
        return false;
    }
    if (code != 200) {
        if (report) setStatus("Spotify did not answer");
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
        const char* image = kind == Kind::Track ? coverUrl(item["album"]["images"]) : coverUrl(item["images"]);
        addItem(kind, id, name, sub, image);
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
    published.canBack = depth > 0;
    published.pagePlayable = false;
    published.pageKind = Kind::Track;
    published.pageSize = 10;
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
        filter["items"][0]["images"][0]["url"] = true;
        filter["items"][0]["images"][0]["width"] = true;
        filter["next"] = true;
    } else {
        filter["tracks"]["items"][0]["id"] = true;
        filter["tracks"]["items"][0]["name"] = true;
        filter["tracks"]["items"][0]["artists"][0]["name"] = true;
        filter["tracks"]["items"][0]["album"]["images"][0]["url"] = true;
        filter["tracks"]["items"][0]["album"]["images"][0]["width"] = true;
        filter["albums"]["items"][0]["id"] = true;
        filter["albums"]["items"][0]["name"] = true;
        filter["albums"]["items"][0]["artists"][0]["name"] = true;
        filter["albums"]["items"][0]["images"][0]["url"] = true;
        filter["albums"]["items"][0]["images"][0]["width"] = true;
        filter["playlists"]["items"][0]["id"] = true;
        filter["playlists"]["items"][0]["name"] = true;
        filter["playlists"]["items"][0]["owner"]["display_name"] = true;
        filter["playlists"]["items"][0]["images"][0]["url"] = true;
        filter["playlists"]["items"][0]["images"][0]["width"] = true;
        filter["artists"]["items"][0]["id"] = true;
        filter["artists"]["items"][0]["name"] = true;
        filter["artists"]["items"][0]["images"][0]["url"] = true;
        filter["artists"]["items"][0]["images"][0]["width"] = true;
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
    published.canBack = depth > 0;
    published.pagePlayable = false;
    published.pageKind = Kind::Track;
    published.pageSize = 10;
    if (published.count == 0) strlcpy(published.status, search ? "No matches" : "No playlists", sizeof(published.status));
    else published.status[0] = 0;
    int covers = 0;
    for (int i = 0; i < published.count; i++)
        if (published.items[i].image[0]) covers++;
    unlock();
    Serial.printf("[svc] %d items, %d covers\n", published.count, covers);
}

// Sonos addresses, from SoCo's ShareLink table. The fallback is the form this
// household already plays from a saved favorite.
bool spotifyDidl(const Item& item, char* uri, size_t uriCap, char* meta, size_t metaCap, bool favoriteForm) {
    if (item.kind == Kind::Radio || item.kind == Kind::Artist) {
        int sn = sonos::spotifyAccount();
        if (sn < 1) sn = 1;
        snprintf(uri, uriCap, "x-sonosapi-radio:spotify%%3aartistRadio%%3a%s?sid=9&flags=8300&sn=%d", item.id, sn);
        char title[160];
        xmlEscape(item.title[0] ? item.title : "Radio", title, sizeof(title));
        char id[140];
        snprintf(id, sizeof(id), "100c206cspotify%%3aartistRadio%%3a%s", item.id);
        int wrote = snprintf(meta, metaCap,
                             "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                             "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                             "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
                             "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
                             "<item id=\"%s\" parentID=\"-1\" restricted=\"true\">"
                             "<dc:title>%s</dc:title>"
                             "<upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
                             "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
                             "SA_RINCON2311_X_#Svc2311-0-Token</desc></item></DIDL-Lite>",
                             id, title);
        return wrote > 0 && (size_t)wrote < metaCap;
    }
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
    if (item.kind == Kind::Artist || item.kind == Kind::Radio) {
        char uri[320];
        char meta[1200];
        if (!spotifyDidl(item, uri, sizeof(uri), meta, sizeof(meta), false)) return false;
        Serial.printf("[svc] radio %s\n", uri);
        return sonos::enqueue(uri, meta, !append);
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

void beginPage(const Crumb& c) {
    lock();
    published.loading = true;
    published.count = 0;
    published.offset = c.offset;
    published.more = false;
    published.pageSize = c.pageSize > 0 ? c.pageSize : 10;
    published.canBack = depth > 0;
    published.pagePlayable = c.playable;
    published.pageKind = c.kind;
    published.signedIn = true;
    published.status[0] = 0;
    const char* heading = c.title;
    if (c.page == Page::Search) heading = "Search";
    else if (c.page == Page::Playlists) heading = "My playlists";
    strlcpy(published.heading, heading && heading[0] ? heading : "Spotify", sizeof(published.heading));
    strlcpy(published.query, c.query, sizeof(published.query));
    unlock();
}

void finishPage(bool empty, const char* emptyText) {
    published.loading = false;
    published.signedIn = true;
    if (published.count == 0 && empty) strlcpy(published.status, emptyText, sizeof(published.status));
}

void loadAlbum(const Crumb& c) {
    beginPage(c);
    char url[180];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/albums/%s/tracks?limit=20&offset=%d", c.id, c.offset);
    if (!spotifyGet(url)) return;
    JsonDocument filter;
    filter["items"][0]["id"] = true;
    filter["items"][0]["name"] = true;
    filter["items"][0]["track_number"] = true;
    filter["items"][0]["artists"][0]["name"] = true;
    filter["next"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) {
        setStatus("Could not read Spotify");
        return;
    }
    lock();
    for (JsonObject item : doc["items"].as<JsonArray>()) {
        if (published.count >= MAX_ITEMS) break;
        const char* id = item["id"] | "";
        if (!id[0]) continue;
        char sub[72];
        snprintf(sub, sizeof(sub), "%d", item["track_number"] | (published.count + 1));
        const char* artist = firstArtist(item);
        if (artist && artist[0]) {
            size_t n = strlen(sub);
            snprintf(sub + n, sizeof(sub) - n, "  %s", artist);
        }
        addItem(Kind::Track, id, item["name"] | "", sub, c.image);
    }
    published.more = !doc["next"].isNull();
    finishPage(true, "No tracks");
    unlock();
}

void loadPlaylist(const Crumb& c) {
    beginPage(c);
    char url[420];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/playlists/%s/items?limit=20&offset=%d"
             "&fields=items(item(id,name,type,artists(name),album(images(url,width)))),next",
             c.id, c.offset);
    if (!spotifyGet(url, false)) {
        if (spotifyHttp == 403) setStatus("Spotify won't list these tracks. Play will start it.");
        else if (spotifyHttp == 429) setStatus("Spotify is busy, try again");
        else setStatus("Spotify did not answer");
        return;
    }
    JsonDocument filter;
    filter["items"][0]["item"]["id"] = true;
    filter["items"][0]["item"]["name"] = true;
    filter["items"][0]["item"]["type"] = true;
    filter["items"][0]["item"]["artists"][0]["name"] = true;
    filter["items"][0]["item"]["album"]["images"][0]["url"] = true;
    filter["items"][0]["item"]["album"]["images"][0]["width"] = true;
    filter["items"][0]["track"]["id"] = true;
    filter["items"][0]["track"]["name"] = true;
    filter["items"][0]["track"]["type"] = true;
    filter["items"][0]["track"]["artists"][0]["name"] = true;
    filter["items"][0]["track"]["album"]["images"][0]["url"] = true;
    filter["items"][0]["track"]["album"]["images"][0]["width"] = true;
    filter["next"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) {
        setStatus("Could not read Spotify");
        return;
    }
    lock();
    for (JsonObject wrap : doc["items"].as<JsonArray>()) {
        if (published.count >= MAX_ITEMS) break;
        JsonObject item = wrap["item"];
        if (item.isNull()) item = wrap["track"];
        if (item.isNull()) continue;
        const char* type = item["type"] | "track";
        if (strcmp(type, "track")) continue;
        const char* id = item["id"] | "";
        if (!id[0]) continue;
        addItem(Kind::Track, id, item["name"] | "", firstArtist(item), coverUrl(item["album"]["images"]));
    }
    published.more = !doc["next"].isNull();
    finishPage(true, "No tracks");
    unlock();
}

// A track's artist objects have no pictures. The artist endpoint does.
void copyArtistImage(const char* id, char* dst, size_t cap) {
    dst[0] = 0;
    char url[80];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/artists/%s", id);
    if (!spotifyGet(url, false)) return;
    JsonDocument filter;
    filter["images"][0]["url"] = true;
    filter["images"][0]["width"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) return;
    const char* image = coverUrl(doc["images"]);
    if (image && image[0]) strlcpy(dst, image, cap);
}

void loadTrack(const Crumb& c) {
    beginPage(c);
    char url[120];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/tracks/%s", c.id);
    if (!spotifyGet(url)) return;
    JsonDocument filter;
    filter["duration_ms"] = true;
    filter["artists"][0]["id"] = true;
    filter["artists"][0]["name"] = true;
    filter["album"]["id"] = true;
    filter["album"]["name"] = true;
    filter["album"]["images"][0]["url"] = true;
    filter["album"]["images"][0]["width"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) {
        setStatus("Could not read Spotify");
        return;
    }
    int ms = doc["duration_ms"] | 0;
    if (ms < 0) ms = 0;
    char clock[16];
    snprintf(clock, sizeof(clock), "%d:%02d", ms / 60000, (ms / 1000) % 60);
    char albumId[48] = "";
    char albumName[72] = "";
    char albumImage[IMAGE_CAP] = "";
    strlcpy(albumId, doc["album"]["id"] | "", sizeof(albumId));
    strlcpy(albumName, doc["album"]["name"] | "", sizeof(albumName));
    strlcpy(albumImage, coverUrl(doc["album"]["images"]), sizeof(albumImage));
    struct Who {
        char id[48];
        char name[72];
        char image[IMAGE_CAP];
    };
    Who who[4];
    int whoN = 0;
    for (JsonObject artist : doc["artists"].as<JsonArray>()) {
        if (whoN >= 4) break;
        const char* id = artist["id"] | "";
        if (!id[0]) continue;
        strlcpy(who[whoN].id, id, sizeof(who[whoN].id));
        strlcpy(who[whoN].name, artist["name"] | "Artist", sizeof(who[whoN].name));
        who[whoN].image[0] = 0;
        whoN++;
    }
    for (int i = 0; i < whoN; i++) copyArtistImage(who[i].id, who[i].image, sizeof(who[i].image));
    lock();
    strlcpy(published.status, clock, sizeof(published.status));
    if (albumId[0]) addItem(Kind::Album, albumId, albumName[0] ? albumName : "Album", "", albumImage);
    for (int i = 0; i < whoN && published.count < MAX_ITEMS; i++)
        addItem(Kind::Artist, who[i].id, who[i].name, "", who[i].image);
    finishPage(false, "");
    unlock();
}

void loadArtist(const Crumb& c) {
    beginPage(c);
    if (c.offset == 0) {
        lock();
        addItem(Kind::Radio, c.id, "Radio", "Starts a station", c.image);
        unlock();
        char topUrl[160];
        snprintf(topUrl, sizeof(topUrl), "https://api.spotify.com/v1/artists/%s/top-tracks?market=from_token", c.id);
        if (spotifyGet(topUrl, false)) {
            JsonDocument filter;
            filter["tracks"][0]["id"] = true;
            filter["tracks"][0]["name"] = true;
            filter["tracks"][0]["artists"][0]["name"] = true;
            filter["tracks"][0]["album"]["images"][0]["url"] = true;
            filter["tracks"][0]["album"]["images"][0]["width"] = true;
            JsonDocument doc;
            if (!deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) {
                lock();
                int n = 0;
                for (JsonObject item : doc["tracks"].as<JsonArray>()) {
                    if (n >= 5 || published.count >= MAX_ITEMS) break;
                    const char* id = item["id"] | "";
                    if (!id[0]) continue;
                    addItem(Kind::Track, id, item["name"] | "", "Popular", coverUrl(item["album"]["images"]));
                    n++;
                }
                unlock();
            }
        }
    }
    char url[220];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/artists/%s/albums?include_groups=album,single&market=from_token&limit=10&offset=%d",
             c.id, c.offset);
    if (!spotifyGet(url)) return;
    JsonDocument filter;
    filter["items"][0]["id"] = true;
    filter["items"][0]["name"] = true;
    filter["items"][0]["album_group"] = true;
    filter["items"][0]["release_date"] = true;
    filter["items"][0]["images"][0]["url"] = true;
    filter["items"][0]["images"][0]["width"] = true;
    filter["next"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, httpBuf, DeserializationOption::Filter(filter))) {
        setStatus("Could not read Spotify");
        return;
    }
    lock();
    for (JsonObject item : doc["items"].as<JsonArray>()) {
        if (published.count >= MAX_ITEMS) break;
        const char* id = item["id"] | "";
        if (!id[0]) continue;
        const char* group = item["album_group"] | "album";
        const char* date = item["release_date"] | "";
        char year[8] = "";
        if (date[0] && date[1] && date[2] && date[3]) {
            memcpy(year, date, 4);
            year[4] = 0;
        }
        char sub[72];
        if (!strcmp(group, "single")) snprintf(sub, sizeof(sub), "Single%s%s", year[0] ? "  " : "", year);
        else strlcpy(sub, year, sizeof(sub));
        addItem(Kind::Album, id, item["name"] | "", sub, coverUrl(item["images"]));
    }
    published.more = !doc["next"].isNull();
    finishPage(published.count == 0, "Nothing here");
    unlock();
}

void loadCrumb() {
    if (depth < 0) depth = 0;
    if (depth >= MAX_DEPTH) depth = MAX_DEPTH - 1;
    Crumb& c = stack[depth];
    if (c.page == Page::Album) loadAlbum(c);
    else if (c.page == Page::Playlist) loadPlaylist(c);
    else if (c.page == Page::Artist) loadArtist(c);
    else if (c.page == Page::Track) loadTrack(c);
    else spotifyLoad(c.page == Page::Search, c.query, c.offset);
}

void openItem(const Item& item) {
    if (!item.id[0]) {
        setStatus("Can't open that");
        return;
    }
    if (depth + 1 >= MAX_DEPTH) {
        setStatus("That's as deep as this list goes");
        return;
    }
    Crumb next = {};
    strlcpy(next.id, item.id, sizeof(next.id));
    strlcpy(next.title, item.title, sizeof(next.title));
    strlcpy(next.image, item.image, sizeof(next.image));
    if (item.kind == Kind::Track) {
        next.page = Page::Track;
        next.kind = Kind::Track;
        next.playable = true;
        next.pageSize = 20;
    } else if (item.kind == Kind::Album) {
        next.page = Page::Album;
        next.kind = Kind::Album;
        next.playable = true;
        next.pageSize = 20;
    } else if (item.kind == Kind::Playlist) {
        next.page = Page::Playlist;
        next.kind = Kind::Playlist;
        next.playable = true;
        next.pageSize = 20;
    } else if (item.kind == Kind::Artist) {
        next.page = Page::Artist;
        next.kind = Kind::Radio;
        next.playable = true;
        next.pageSize = 10;
    } else if (item.kind == Kind::Radio) {
        if (!spotifyPlay(item, false)) setStatus("Could not play that");
        else {
            lock();
            published.loading = false;
            unlock();
        }
        return;
    } else {
        setStatus("Can't open that");
        return;
    }
    depth++;
    stack[depth] = next;
    loadCrumb();
}

void playPage(bool append) {
    if (depth < 0 || !stack[depth].playable || !stack[depth].id[0]) return;
    Item item = {};
    item.kind = stack[depth].kind;
    strlcpy(item.id, stack[depth].id, sizeof(item.id));
    strlcpy(item.title, stack[depth].title, sizeof(item.title));
    if (!spotifyPlay(item, append)) setStatus("Could not play that");
}

struct Art {
    char url[IMAGE_CAP];
    uint8_t* data;
    size_t len;
};
Art arts[MAX_ITEMS];

void rememberArt(const char* url, uint8_t* data, size_t len) {
    lock();
    Art* slot = nullptr;
    for (Art& a : arts) {
        if (a.url[0] && !strcmp(a.url, url)) {
            heap_caps_free(a.data);
            a.data = data;
            a.len = data ? len : 0;
            unlock();
            return;
        }
        if (!slot && !a.url[0]) slot = &a;
    }
    if (!slot) {
        heap_caps_free(data);
        unlock();
        return;
    }
    strlcpy(slot->url, url, sizeof(slot->url));
    slot->data = data;
    slot->len = data ? len : 0;
    unlock();
}

void hostOf(const char* url, char* dst, size_t cap) {
    dst[0] = 0;
    if (!url || !cap) return;
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = 0;
    while (p[n] && p[n] != '/' && p[n] != ':' && n + 1 < cap) {
        dst[n] = p[n];
        n++;
    }
    dst[n] = 0;
}

bool imageBytes(const uint8_t* p, size_t n) {
    if (n > 2 && p[0] == 0xFF && p[1] == 0xD8) return true;
    return n > 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G';
}

void serviceOneArt() {
    static char urls[MAX_ITEMS][IMAGE_CAP];
    int n = 0;
    lock();
    for (int i = 0; i < published.count && n < MAX_ITEMS; i++) {
        const char* u = published.items[i].image;
        if (!u[0]) continue;
        bool seen = false;
        for (int k = 0; k < n; k++)
            if (!strcmp(urls[k], u)) seen = true;
        if (!seen) strlcpy(urls[n++], u, IMAGE_CAP);
    }
    for (Art& a : arts) {
        if (!a.url[0]) continue;
        bool keep = false;
        for (int i = 0; i < n; i++)
            if (!strcmp(urls[i], a.url)) keep = true;
        if (keep) continue;
        heap_caps_free(a.data);
        a = {};
    }
    int missing = 0;
    for (int i = 0; i < n; i++) {
        bool have = false;
        for (Art& a : arts)
            if (!strcmp(a.url, urls[i])) have = true;
        if (have) continue;
        if (missing != i) memcpy(urls[missing], urls[i], IMAGE_CAP);
        missing++;
    }
    unlock();
    if (!missing) return;

    for (int i = 0; i < missing; i++) {
        char hi[48];
        hostOf(urls[i], hi, sizeof(hi));
        for (int j = i + 1; j < missing; j++) {
            char hj[48];
            hostOf(urls[j], hj, sizeof(hj));
            if (strcmp(hi, hj) <= 0) continue;
            char tmp[IMAGE_CAP];
            memcpy(tmp, urls[i], IMAGE_CAP);
            memcpy(urls[i], urls[j], IMAGE_CAP);
            memcpy(urls[j], tmp, IMAGE_CAP);
            memcpy(hi, hj, sizeof(hi));
        }
    }

    static uint32_t pauseUntil = 0;
    if ((int32_t)(millis() - pauseUntil) < 0) return;

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(3);
    HTTPClient http;
    http.setReuse(true);
    http.setConnectTimeout(2000);
    http.setTimeout(2500);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    for (int i = 0; i < missing; i++) {
        const char* url = urls[i];
        if (strncmp(url, "https://", 8)) {
            rememberArt(url, nullptr, 0);
            continue;
        }
        uint32_t t0 = millis();
        int code = -1;
        size_t got = 0;
        if (http.begin(client, url)) {
            code = http.GET();
            if (code == 200) {
                Body body;
                body.buf = httpBuf;
                body.cap = HTTP_CAP;
                http.writeToStream(&body);
                got = body.n;
            }
            http.end();
        }
        uint32_t took = millis() - t0;
        bool picture = code == 200 && got + 1 < HTTP_CAP && imageBytes((uint8_t*)httpBuf, got);
        if (picture) {
            uint8_t* keep = (uint8_t*)heap_caps_malloc(got, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (keep) memcpy(keep, httpBuf, got);
            rememberArt(url, keep, keep ? got : 0);
            Serial.printf("[svc] art %s %u in %ums\n", keep ? "ok" : "miss", (unsigned)got, (unsigned)took);
        } else if (code <= 0) {
            Serial.printf("[svc] art retry %d in %ums\n", code, (unsigned)took);
            pauseUntil = millis() + 600;
            break;
        } else {
            rememberArt(url, nullptr, 0);
            Serial.printf("[svc] art miss %d in %ums\n", code, (unsigned)took);
        }
    }
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
    strlcpy(published.heading, "Search", sizeof(published.heading));
    loadCredentials();
    published.signedIn = clientId[0] && refresh[0];
    if (!published.signedIn) strlcpy(published.status, "Sign in with tools/spotify_auth.py", sizeof(published.status));
    Serial.printf("[svc] spotify %s\n", published.signedIn ? "signed in" : "not signed in");
}

void loop(bool online) {
    if (!mu) return;
    bool probeNow = false, saveNow = false, clearNow = false, listNow = false, playlistsNow = false;
    bool doBack = false, doReload = false, doPage = false;
    char query[64];
    int offset = 0;
    int play = -1;
    bool append = false;
    bool haveOpen = false;
    Item opened = {};
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
    playlistsNow = wantPlaylists;
    wantPlaylists = false;
    doBack = wantBack;
    wantBack = false;
    doReload = wantReload;
    wantReload = false;
    doPage = wantPagePlay;
    wantPagePlay = false;
    strlcpy(query, wantQuery, sizeof(query));
    offset = wantOffset;
    play = wantPlay;
    wantPlay = -1;
    haveOpen = wantOpen;
    wantOpen = false;
    if (haveOpen) opened = wantOpenItem;
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
    } else if (haveOpen) {
        openItem(opened);
    } else if (doPage) {
        playPage(append);
    } else if (doBack) {
        if (depth > 0) depth--;
        loadCrumb();
    } else if (doReload) {
        stack[depth].offset = offset < 0 ? 0 : offset;
        loadCrumb();
    } else if (listNow) {
        depth = 0;
        memset(&stack[0], 0, sizeof(stack[0]));
        if (!playlistsNow && !query[0]) {
            stack[0].page = Page::Search;
            lock();
            published.loading = false;
            published.count = 0;
            published.offset = 0;
            published.more = false;
            published.canBack = false;
            published.pagePlayable = false;
            published.pageKind = Kind::Track;
            published.pageSize = 10;
            published.query[0] = 0;
            published.status[0] = 0;
            published.signedIn = clientId[0] && refresh[0];
            strlcpy(published.heading, "Search", sizeof(published.heading));
            unlock();
        } else {
            stack[0].page = playlistsNow ? Page::Playlists : Page::Search;
            stack[0].offset = offset < 0 ? 0 : offset;
            stack[0].pageSize = 10;
            strlcpy(stack[0].query, query, sizeof(stack[0].query));
            loadCrumb();
        }
    } else if (online) {
        serviceOneArt();
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
    wantPlaylists = true;
    wantReload = false;
    wantBack = false;
    wantOpen = false;
    wantPlay = -1;
    wantPagePlay = false;
    published.loading = true;
    published.count = 0;
    unlock();
}

void requestSearch(const char* query, int offset) {
    if (!mu || !query) return;
    lock();
    strlcpy(wantQuery, query, sizeof(wantQuery));
    wantOffset = offset;
    wantList = true;
    wantPlaylists = false;
    wantReload = false;
    wantBack = false;
    wantOpen = false;
    wantPlay = -1;
    wantPagePlay = false;
    published.count = 0;
    published.canBack = false;
    published.pagePlayable = false;
    if (!query[0]) {
        published.loading = false;
        published.offset = 0;
        published.more = false;
        published.query[0] = 0;
        published.status[0] = 0;
        strlcpy(published.heading, "Search", sizeof(published.heading));
    } else {
        published.loading = true;
    }
    unlock();
}

void requestPage(int offset) {
    if (!mu) return;
    lock();
    wantOffset = offset < 0 ? 0 : offset;
    wantReload = true;
    wantList = false;
    wantBack = false;
    wantOpen = false;
    published.loading = true;
    published.count = 0;
    unlock();
}

void requestOpen(int index) {
    if (!mu) return;
    lock();
    if (index >= 0 && index < published.count) {
        wantOpenItem = published.items[index];
        wantOpen = true;
        wantPlay = -1;
        wantPagePlay = false;
        wantList = false;
        wantReload = false;
        wantBack = false;
        published.loading = true;
        published.count = 0;
    }
    unlock();
}

void requestBack() {
    if (!mu) return;
    lock();
    wantBack = true;
    wantList = false;
    wantReload = false;
    wantOpen = false;
    wantPlay = -1;
    wantPagePlay = false;
    published.loading = true;
    published.count = 0;
    unlock();
}

void requestPlay(int index, bool append) {
    if (!mu) return;
    lock();
    wantPlay = index;
    wantAppend = append;
    wantOpen = false;
    wantPagePlay = false;
    unlock();
}

void requestPagePlay(bool append) {
    if (!mu) return;
    lock();
    wantPagePlay = true;
    wantAppend = append;
    wantPlay = -1;
    wantOpen = false;
    unlock();
}

size_t copyArt(const char* url, uint8_t* dst, size_t cap, bool* failed) {
    if (failed) *failed = false;
    if (!mu || !url || !url[0] || !dst) return 0;
    lock();
    size_t n = 0;
    for (Art& a : arts) {
        if (strcmp(a.url, url)) continue;
        if (a.data && a.len && a.len <= cap) {
            memcpy(dst, a.data, a.len);
            n = a.len;
        } else {
            if (failed) *failed = true;
        }
        break;
    }
    unlock();
    return n;
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
