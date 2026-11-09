#pragma once

#include <stdint.h>

// Music services the Tab5 talks to directly. Spotify is the first provider.
// A later service adds a provider and a row in the Sonos hand-off table.
// loop() does the network work and must run on the network task.
namespace svc {

enum class Kind : uint8_t { Track, Album, Playlist, Artist, Radio };

constexpr int MAX_ITEMS = 20;

struct Item {
    Kind kind;
    char id[48];
    char title[72];
    char subtitle[72];
};

struct View {
    bool signedIn;
    bool loading;
    int count;
    int offset;
    bool more;
    char query[64];
    char heading[72];
    char status[80];
    bool canBack;
    bool pagePlayable;
    Kind pageKind;
    int pageSize;
    Item items[MAX_ITEMS];
};

void begin();
void loop(bool online);

// Serial. The refresh token stays in NVS. An empty secret is fine for PKCE.
void storeCredentials(const char* provider, const char* clientId, const char* refresh, const char* secret);
void clearCredentials(const char* provider);
void requestProbe();

void requestPlaylists(int offset);
void requestSearch(const char* query, int offset);
void requestPage(int offset);
void requestOpen(int index);
void requestBack();
void requestPlay(int index, bool append);
void requestPagePlay(bool append);

void copyView(View& out);
const char* providerName();

}  // namespace svc
