#pragma once

#include <Print.h>

// Wi-Fi through the Tab5's ESP32-C6. Credentials stay in NVS. The radio stays
// off until connect() or startSetup() runs, and both are called from the
// network task so WiFi.begin never blocks the pixels.
namespace net {

enum class State : uint8_t { Off, Connecting, Connected, Failed };

void begin();

// Copies the request. The network task is the one that calls WiFi.begin.
// Empty ssid uses the stored network.
void requestConnect(const char* ssid, const char* pass);
void requestForget();

void loop(Print& out);

bool hasStoredNetwork();
State state();
bool connected();
const char* ssid();
const char* localIp();
int rssi();

void startSetup(Print& out);
void stopSetup();
bool setupActive();
bool setupClosing();
const char* setupName();

}  // namespace net
