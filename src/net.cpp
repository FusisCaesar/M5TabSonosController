#include "net.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace net {
namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t SCAN_INTERVAL_MS = 15000;

State st = State::Off;
uint32_t connectStartMs = 0;
String pendingSsid, pendingPass, currentSsid;
char ipText[16] = "";

WebServer* server = nullptr;
DNSServer* dns = nullptr;
char apName[24] = "";
uint32_t lastScanMs = 0;
uint32_t connectedInSetupMs = 0;
bool connectFromPage = false;

SemaphoreHandle_t mu = nullptr;
bool connectReq = false;
bool forgetReq = false;
char reqSsid[33] = "";
char reqPass[65] = "";

String htmlEscape(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

const char* PAGE_HEAD =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Tab5 Sonos</title><style>body{font-family:sans-serif;margin:24px;max-width:420px}"
    "input,button{font-size:18px;width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
    "button{background:#1c1c1c;color:#fff;border:0;border-radius:6px}</style></head><body>";

void handleRoot() {
    String page = PAGE_HEAD;
    page += "<h2>Tab5 Sonos</h2><p>Choose the 2.4 GHz network this controller should join.</p>"
            "<form method=post action=/save><label>Network</label>"
            "<input name=s list=nets autocomplete=off required><datalist id=nets>";
    int n = WiFi.scanComplete();
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (s.length()) page += "<option value=\"" + htmlEscape(s) + "\">";
    }
    page += "</datalist><label>Password</label><input name=p type=password autocomplete=off>"
            "<button>Connect</button></form>";
    if (n > 0) {
        page += "<p>Networks in range:<br>";
        for (int i = 0; i < n && i < 12; i++) page += htmlEscape(WiFi.SSID(i)) + " (" + WiFi.RSSI(i) + " dBm)<br>";
        page += "</p>";
    }
    page += "</body></html>";
    server->send(200, "text/html", page);
}

void handleSave() {
    String s = server->arg("s"), p = server->arg("p");
    s.trim();
    if (s.isEmpty()) {
        server->sendHeader("Location", "/");
        server->send(302);
        return;
    }
    String page = PAGE_HEAD;
    page += "<h2>Connecting...</h2><p>The Tab5 is joining <b>" + htmlEscape(s) +
            "</b>. This setup network closes once that works. If the password was wrong, "
            "the page stays open.</p></body></html>";
    server->send(200, "text/html", page);
    pendingSsid = s;
    pendingPass = p;
    currentSsid = s;
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(s.c_str(), p.c_str());
    connectStartMs = millis();
    st = State::Connecting;
    connectFromPage = true;
}

void handleOther() {
    server->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server->send(302);
}

void remember() {
    Preferences prefs;
    prefs.begin("net", false);
    prefs.putString("ssid", pendingSsid);
    prefs.putString("pass", pendingPass);
    prefs.end();
}

}  // namespace

void begin() {
    if (!mu) mu = xSemaphoreCreateMutex();
}

void requestConnect(const char* ssid, const char* pass) {
    if (!mu) return;
    xSemaphoreTake(mu, portMAX_DELAY);
    strlcpy(reqSsid, ssid ? ssid : "", sizeof(reqSsid));
    strlcpy(reqPass, pass ? pass : "", sizeof(reqPass));
    connectReq = true;
    xSemaphoreGive(mu);
}

void requestForget() {
    if (!mu) return;
    xSemaphoreTake(mu, portMAX_DELAY);
    forgetReq = true;
    xSemaphoreGive(mu);
}

bool hasStoredNetwork() {
    Preferences prefs;
    prefs.begin("net", false);
    bool has = prefs.isKey("ssid") && prefs.getString("ssid", "").length() > 0;
    prefs.end();
    return has;
}

static bool loadStored(String& s, String& p) {
    Preferences prefs;
    prefs.begin("net", true);
    s = prefs.getString("ssid", "");
    p = prefs.getString("pass", "");
    prefs.end();
    return s.length() > 0;
}

static void startJoin(const String& s, const String& p, Print& out) {
    out.printf("[wifi] connecting to '%s' (password %u chars)\n", s.c_str(), (unsigned)p.length());
    connectFromPage = false;
    pendingSsid = s;
    pendingPass = p;
    currentSsid = s;
    ipText[0] = 0;
    WiFi.mode(server ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(s.c_str(), p.c_str());
    connectStartMs = millis();
    st = State::Connecting;
}

void loop(Print& out) {
    if (mu) {
        xSemaphoreTake(mu, portMAX_DELAY);
        bool doForget = forgetReq;
        bool doConnect = connectReq;
        char ssid[33], pass[65];
        strlcpy(ssid, reqSsid, sizeof(ssid));
        strlcpy(pass, reqPass, sizeof(pass));
        forgetReq = false;
        connectReq = false;
        xSemaphoreGive(mu);
        if (doForget) {
            if (server) stopSetup();
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            st = State::Off;
            ipText[0] = 0;
            Preferences prefs;
            prefs.begin("net", false);
            prefs.remove("ssid");
            prefs.remove("pass");
            prefs.end();
            currentSsid = "";
            pendingSsid = "";
            pendingPass = "";
            out.println("[wifi] forgot the stored network");
            startSetup(out);
        } else if (doConnect) {
            connectFromPage = false;
            String s = ssid, p = pass;
            if (s.isEmpty() && !loadStored(s, p)) {
                out.println("[wifi] ERR no stored network");
                st = State::Failed;
            } else {
                startJoin(s, p, out);
            }
        }
    }

    if (st == State::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            IPAddress ip = WiFi.localIP();
            snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            out.printf("[wifi] OK %s rssi %d dBm in %lu ms\n", ipText, WiFi.RSSI(),
                       (unsigned long)(millis() - connectStartMs));
            remember();
            st = State::Connected;
            if (server && connectFromPage) connectedInSetupMs = millis();
            connectFromPage = false;
        } else if (millis() - connectStartMs > CONNECT_TIMEOUT_MS) {
            out.printf("[wifi] ERR not connected after %lu ms (status %d)\n",
                       (unsigned long)(millis() - connectStartMs), (int)WiFi.status());
            WiFi.disconnect(false);
            if (!server) WiFi.mode(WIFI_OFF);
            ipText[0] = 0;
            st = State::Failed;
        }
    } else if (st == State::Connected && WiFi.status() != WL_CONNECTED) {
        out.println("[wifi] connection lost");
        ipText[0] = 0;
        st = State::Failed;
    }

    if (server) {
        dns->processNextRequest();
        server->handleClient();
        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING && millis() - lastScanMs > SCAN_INTERVAL_MS &&
            st != State::Connecting) {
            lastScanMs = millis();
            WiFi.scanNetworks(true);
        }
        if (connectedInSetupMs && millis() - connectedInSetupMs > 5000) stopSetup();
    }
}

State state() { return st; }

bool connected() { return st == State::Connected && WiFi.status() == WL_CONNECTED; }

const char* ssid() { return currentSsid.c_str(); }

const char* localIp() { return ipText; }

int rssi() { return connected() ? WiFi.RSSI() : 0; }

void startSetup(Print& out) {
    if (server) return;
    snprintf(apName, sizeof(apName), "TabSonos-%04X", (unsigned)(ESP.getEfuseMac() & 0xFFFF));
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    if (!WiFi.softAP(apName)) {
        out.println("[wifi] ERR could not start the setup network");
        WiFi.mode(st == State::Connected || st == State::Connecting ? WIFI_STA : WIFI_OFF);
        return;
    }
    dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP());
    server = new WebServer(80);
    server->on("/", HTTP_GET, handleRoot);
    server->on("/save", HTTP_POST, handleSave);
    server->onNotFound(handleOther);
    server->begin();
    connectedInSetupMs = 0;
    lastScanMs = millis();
    WiFi.scanNetworks(true);
    out.printf("[wifi] setup network '%s' at http://%s/\n", apName, WiFi.softAPIP().toString().c_str());
}

void stopSetup() {
    if (!server) return;
    server->stop();
    delete server;
    server = nullptr;
    dns->stop();
    delete dns;
    dns = nullptr;
    WiFi.scanDelete();
    WiFi.softAPdisconnect(true);
    WiFi.mode(st == State::Connected || st == State::Connecting ? WIFI_STA : WIFI_OFF);
    connectedInSetupMs = 0;
    Serial.println("[wifi] setup network closed");
}

bool setupActive() { return server != nullptr; }

bool setupClosing() { return server && connectedInSetupMs; }

const char* setupName() { return apName; }

}  // namespace net
