#include <M5Unified.h>

#include "board.h"
#include "net.h"
#include "services.h"
#include "sonos.h"
#include "ui.h"
#include "youtube.h"

#include <string.h>

static void netTask(void*) {
    uint32_t nextSetup = 0;
    if (!net::hasStoredNetwork()) net::startSetup(Serial);
    else net::requestConnect("", "");
    for (;;) {
        net::loop(Serial);
        yt::loop(net::connected());
        svc::loop(net::connected());
        if (net::connected()) sonos::loop();
        else if (!net::setupActive() && net::state() != net::State::Connecting && millis() > nextSetup) {
            nextSetup = millis() + 5000;
            if (net::state() == net::State::Failed || !net::hasStoredNetwork()) net::startSetup(Serial);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void serialCommands() {
    static char line[700];
    static int len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (len < (int)sizeof(line) - 1) line[len++] = c;
            continue;
        }
        line[len] = 0;
        len = 0;
        if (line[0] == 'W' && (line[1] == 0 || line[1] == ' ')) {
            const char* rest = line + 1;
            while (*rest == ' ') rest++;
            if (!*rest) {
                net::requestConnect("", "");
            } else {
                char ssid[33], pass[65];
                const char* sep = strchr(rest, '|');
                if (!sep) {
                    strlcpy(ssid, rest, sizeof(ssid));
                    pass[0] = 0;
                } else {
                    size_t n = (size_t)(sep - rest);
                    if (n > sizeof(ssid) - 1) n = sizeof(ssid) - 1;
                    memcpy(ssid, rest, n);
                    ssid[n] = 0;
                    strlcpy(pass, sep + 1, sizeof(pass));
                }
                net::requestConnect(ssid, pass);
            }
        } else if ((line[0] == 'P' || line[0] == 'p') && line[1] == 0) {
            ui::requestShot();
        } else if ((line[0] == 'U' || line[0] == 'u') && line[1] == ' ') {
            int tx = 0, ty = 0;
            if (sscanf(line + 2, "%d %d", &tx, &ty) == 2) ui::injectTap(tx, ty);
        } else if ((line[0] == 'F' || line[0] == 'f') && line[1] == 0) {
            net::requestForget();
        } else if ((line[0] == 'T' || line[0] == 't') && line[1] == 0) {
            svc::requestProbe();
        } else if ((line[0] == 'X' || line[0] == 'x') && (line[1] == 0 || line[1] == ' ')) {
            const char* name = line + 1;
            while (*name == ' ') name++;
            svc::clearCredentials(name[0] ? name : "spotify");
        } else if ((line[0] == 'S' || line[0] == 's') && (line[1] == 0 || line[1] == ' ')) {
            const char* rest = line + 1;
            while (*rest == ' ') rest++;
            char provider[16], id[48], token[400], secret[80];
            provider[0] = id[0] = token[0] = secret[0] = 0;
            char* fields[] = {provider, id, token, secret};
            size_t caps[] = {sizeof(provider), sizeof(id), sizeof(token), sizeof(secret)};
            for (int i = 0; i < 4 && rest && *rest; i++) {
                const char* sep = strchr(rest, '|');
                size_t n = sep ? (size_t)(sep - rest) : strlen(rest);
                if (n >= caps[i]) n = caps[i] - 1;
                memcpy(fields[i], rest, n);
                fields[i][n] = 0;
                rest = sep ? sep + 1 : nullptr;
            }
            svc::storeCredentials(provider, id, token, secret);
        } else if ((line[0] == 'Y' || line[0] == 'y') && (line[1] == 0 || line[1] == ' ')) {
            const char* rest = line + 1;
            while (*rest == ' ') rest++;
            if (*rest) yt::requestResolve(rest);
        } else if ((line[0] == 'Y' || line[0] == 'y') && (line[1] == 'q' || line[1] == 'Q') && line[2] == ' ') {
            yt::requestSearch(line + 3);
        } else if ((line[0] == 'Y' || line[0] == 'y') && (line[1] == 's' || line[1] == 'S') && line[2] == 0) {
            yt::requestSeek();
        } else if ((line[0] == 'Y' || line[0] == 'y') && (line[1] == 'k' || line[1] == 'K') && line[2] == 0) {
            yt::starPlaying();
        } else if ((line[0] == 'Y' || line[0] == 'y') && (line[1] == 'c' || line[1] == 'C') && line[2] == ' ') {
            char name[24], version[32], ua[180];
            name[0] = version[0] = ua[0] = 0;
            const char* rest = line + 3;
            const char* bar = strchr(rest, '|');
            if (bar) {
                size_t n = (size_t)(bar - rest);
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, rest, n);
                name[n] = 0;
                rest = bar + 1;
                bar = strchr(rest, '|');
                if (bar) {
                    n = (size_t)(bar - rest);
                    if (n >= sizeof(version)) n = sizeof(version) - 1;
                    memcpy(version, rest, n);
                    version[n] = 0;
                    strlcpy(ua, bar + 1, sizeof(ua));
                }
            }
            yt::setClient(name, version, ua);
        }
    }
}

void setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_imu = false;
    M5.begin(cfg);
    Serial.setTxTimeoutMs(0);
    board::begin();
    net::begin();
    sonos::begin();
    svc::begin();
    yt::begin();
    ui::begin();

    Serial.println("\n[boot] Tab5 Sonos");
    Serial.println("[boot] W ssid|password joins Wi-Fi, F forgets it");
    Serial.println("[boot] T probes Spotify TLS, X spotify signs out");
    Serial.println("[boot] Y videoId plays YouTube, Yq words searches, Ys seeks, Yk stars or unstars the playing video, Yc name|version|ua");
    Serial.printf("[boot] heap %u  psram %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    xTaskCreatePinnedToCore(netTask, "net", 32 * 1024, nullptr, 1, nullptr, 0);
}

void loop() {
    {
        board::I2CLock lock;
        M5.update();
    }
    ui::loop();
    serialCommands();
    vTaskDelay(pdMS_TO_TICKS(10));
}
