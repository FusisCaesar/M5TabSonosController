#pragma once

#include <Arduino.h>

// Local Sonos control over HTTP port 1400. Runs its blocking work on the
// network task. command(), copyView(), and copyArt() are safe from the UI task.
namespace sonos {

constexpr int MAX_ROOMS = 16;
constexpr int MAX_FAVS = 40;

enum class Cmd : uint8_t {
    Play, Pause, Next, Prev, Volume, Mute, ToggleRoom, LoadFavorites, LoadQueue, PlayFavorite,
    JoinRoom, LeaveRoom, RoomVolume
};

struct RoomView {
    char name[40];
    char uuid[40];
    char group[40];    // uuid of the zone coordinator; shared by rooms that play together
    bool inGroup;      // the zone has more than one room
    bool leader;       // this room is the zone coordinator
    bool coordinator;  // the room this controller is showing
    bool playing;
    int volume;  // this room's own level, or -1 until it has been read
};

struct View {
    bool ready;
    bool household;
    bool loadingFavs;
    bool queue;  // the open list is this room's queue, otherwise Sonos favorites
    bool playing;
    int track;   // 1-based position in the current queue, or 0
    int volume;  // 0-100, or -1 until the first reading
    bool muted;
    int listGen; // bumps when the room or what it is playing changes
    int roomCount;
    int favCount;
    int artGen;
    char status[72];
    char groupName[40];
    char title[100];
    char artist[100];
    char album[100];
    char source[16];
    char queueName[72];
    char rel[16];
    char dur[16];
    RoomView rooms[MAX_ROOMS];
    char favs[MAX_FAVS][72];
};

void begin();
void loop();
// extra is the main slider's level when a volume gesture starts, otherwise -1.
void command(Cmd cmd, int value, const char* text, int extra = -1);
// Network task only. Adds a service item to the selected room's queue.
// playNow jumps to it and starts it. Returns false when the speaker rejects the item.
bool enqueue(const char* uri, const char* meta, bool playNow);
// Network task only. Plays a direct http(s) audio address on the selected room.
bool playDirect(const char* uri, const char* title);
bool seekRel(const char* hms);
// Name, channel and thumbnail address to show while the speaker plays the Tab5 relay. Network task only.
void setDirectInfo(const char* title, const char* artist, const char* artUrl);
// Account number Sonos appended to a Spotify address it was already playing, or 0.
int spotifyAccount();
void copyView(View& out);
// Copies the latest album-art JPEG into dst. Returns the byte count.
size_t copyArt(uint8_t* dst, size_t cap, int* gen);

}  // namespace sonos
