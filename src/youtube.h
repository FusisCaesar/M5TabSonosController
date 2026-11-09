#pragma once

#include <stddef.h>
#include <stdint.h>

namespace yt {

constexpr int MAX_ITEMS = 20;
constexpr int MAX_STARS = 24;

struct Item {
    char id[16];
    char title[80];
    char channel[48];
    int seconds;
};

struct View {
    bool loading;
    int count;
    bool more;
    char query[64];
    char heading[48];
    char status[80];
    Item items[MAX_ITEMS];
    // Starred videos, newest first. starThumb[i] is true once the thumbnail for stars[i] is downloaded.
    int starCount;
    Item stars[MAX_STARS];
    bool starThumb[MAX_STARS];
    // The video the Tab5 last handed to the speaker.
    char playingId[16];
    char playingTitle[80];
    char playingChannel[48];
};

void begin();
void loop(bool online);
void requestResolve(const char* videoId);
void requestSearch(const char* query);
void requestMore();
void requestPlay(int index);
void requestSeek();
void setClient(const char* name, const char* version, const char* ua);
void copyView(View& out);

// Adds the video to the starred list, or removes it when it is already there. Safe from the UI task.
void toggleStar(const char* id, const char* title, const char* channel);
// Stars or unstars the video that is playing now.
void starPlaying();
// Plays the starred video at that position.
void playStar(int index);
// Copies the downloaded thumbnail (JPEG) of a starred video. Returns the byte count, 0 if not ready.
size_t copyThumb(const char* id, uint8_t* dst, size_t cap);

}  // namespace yt
