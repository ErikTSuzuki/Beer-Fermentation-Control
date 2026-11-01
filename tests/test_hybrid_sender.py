"""Test the actual HTTP/JSON sender with mocked Wi-Fi and HTTP responses.
Run: python tests/test_hybrid_sender.py [path-to-g++]
Uses the project's real ArduinoJson headers. Generated files: build/hybrid-tests.
"""
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
FIRMWARE = PROJECT.parent / "firmware_spidnel/iSpindel/pio/src"
ARDUINO_JSON = FIRMWARE.parents[1] / ".pio/libdeps/d1_mini_standalone/ArduinoJson/src"
BUILD = PROJECT / "build/hybrid-tests"

WIFI_MOCK = r'''
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
using String = std::string;
struct IPAddress {
    uint32_t value;
    explicit IPAddress(uint32_t v = 0) : value(v) {}
    bool operator==(const IPAddress &v) const { return value == v.value; }
    std::string toString() const { return "192.168.50.1"; }
};
struct Mock {
    uint32_t time = 0;
    int connections = 0, sends = 0, reply = 0, successConnection = 1;
    int ackMode = 0;
    bool connected = false, dhcp = true, begin = true;
    int status = 200, goodAfterSend = 0;
    bool mode = true, drop = false, haveGateway = true;
    std::string ssid, password;
    std::vector<std::vector<uint8_t> > packets;
    std::string response;
};
extern Mock mock;
enum { WIFI_STA = 1, WL_CONNECTED = 3 };
struct WifiMock {
    void persistent(bool) {}
    void setAutoConnect(bool) {}
    void setAutoReconnect(bool) {}
    bool mode(int) { return mock.mode; }
    bool config(IPAddress, IPAddress, IPAddress) { return mock.dhcp; }
    void disconnect() { mock.connected = false; }
    void begin(const char *ssid, const char *password) {
        mock.ssid = ssid; mock.password = password;
        ++mock.connections;
        mock.connected = mock.successConnection > 0 &&
                         mock.connections >= mock.successConnection;
    }
    int status() { return mock.connected ? WL_CONNECTED : 0; }
    int channel() { return mock.connections == 1 ? 1 : 11; }
    IPAddress gatewayIP() { return IPAddress(mock.haveGateway ? 1 : 0); }
};
extern WifiMock WiFi;
struct EspMock { uint32_t getChipId() { return 0x123456; } };
extern EspMock ESP;
struct SerialMock {
    template<class... T> void printf(const char *, T...) {}
    void println(const char *) {}
};
extern SerialMock Serial;
inline uint32_t millis() { return mock.time; }
inline void delay(uint32_t ms) { mock.time += ms; }

struct WiFiClient {};
'''
HTTP_MOCK = r'''
#pragma once
#include "ESP8266WiFi.h"
#include "BrewProtocol.h"
#include <ArduinoJson.h>
#include <cassert>
enum { HTTP_CODE_OK = 200 };
struct HTTPClient {
    void setTimeout(int timeout) { assert(timeout == 2000); }
    void setReuse(bool reuse) { assert(!reuse); }
    bool begin(WiFiClient &, const String &host, uint16_t port, const String &path) {
        assert(host == "192.168.50.1" && port == BREW_HTTP_PORT && path == "/ispindel");
        return mock.begin;
    }
    void addHeader(const char *key, const char *value) {
        assert(std::string(key) == "Content-Type" && std::string(value) == "application/json");
    }
    int POST(const uint8_t *bytes, size_t size) {
        ++mock.sends;
        mock.packets.push_back(std::vector<uint8_t>(bytes, bytes + size));
        StaticJsonDocument<768> request;
        assert(!deserializeJson(request, bytes, size));
        StaticJsonDocument<384> response;
        response["accepted"] = true;
        response["version"] = BREW_VERSION;
        response["sensor_id"] = request["sensor_id"].as<uint32_t>();
        response["boot_id"] = request["boot_id"].as<uint32_t>();
        response["sequence"] = request["sequence"].as<uint32_t>();
        switch (mock.ackMode) {
            case 1: response["accepted"] = false; break;
            case 2: response["version"] = 2; break;
            case 3: response["sensor_id"] = 99; break;
            case 4: response["boot_id"] = 99; break;
            case 5: response["sequence"] = 99; break;
            case 6: response.remove("accepted"); break;
            case 7: response["accepted"] = "true"; break;
            case 8: response["sequence"] = "1"; break;
            case 9: response["sequence"] = -1; break;
        }
        mock.response.clear();
        serializeJson(response, mock.response);
        if (mock.ackMode == 10) mock.response = "{broken";
        if (mock.ackMode == 11) mock.response = "[]";
        if (mock.ackMode == 12) mock.response = "";
        if (mock.ackMode == 13) mock.response = std::string(257, 'x');
        if (mock.drop && mock.connections == 1) {
            mock.connected = false;
            return -1;
        }
        mock.time += 20;
        if (mock.goodAfterSend && mock.sends >= mock.goodAfterSend) return 200;
        return mock.status;
    }
    int getSize() {
        if (mock.ackMode == 14) return -1;
        if (mock.ackMode == 15) return int(mock.response.size()) + 1;
        return int(mock.response.size());
    }
    String getString() { return mock.response; }
    void end() {}
};
'''
TEST = r'''
#include "BrewWifi.h"
#include <cassert>
#include <iostream>
Mock mock;
WifiMock WiFi;
EspMock ESP;
SerialMock Serial;
bool send(const char *ssid = "LocalESP32", const char *password = "local-password") {
    return brewSendReading(20.5f, 45.0f, 1.05f, 4.1f, ssid, password);
}
void reset() { mock = Mock(); brewBootId = 0; brewSequence = 0; }
void sameReading() {
    for (size_t i = 1; i < mock.packets.size(); ++i)
        assert(mock.packets[i] == mock.packets[0]);
}
int main() {
    int scenarios = 0;
    reset(); assert(send());
    assert(mock.connections == 1 && mock.sends == 1 && !mock.connected);
    assert(mock.ssid == "LocalESP32" && mock.password == "local-password");
    std::cout << "SAMPLE " << std::string(mock.packets[0].begin(), mock.packets[0].end()) << "\n";
    ++scenarios;
    for (int mode = 1; mode <= 15; ++mode) {
        reset(); mock.ackMode = mode; assert(!send());
        assert(mock.connections == 2 && mock.sends == 6 && !mock.connected);
        sameReading(); ++scenarios;
    }
    for (int status : {400, 403, 409, 413, 415, 422}) {
        reset(); mock.status = status; assert(!send());
        assert(mock.connections == 1 && mock.sends == 1 && !mock.connected); ++scenarios;
    }
    reset(); mock.status = 503; mock.goodAfterSend = 2; assert(send());
    assert(mock.sends == 2); sameReading(); ++scenarios;
    reset(); mock.status = -1; assert(!send()); assert(mock.sends == 6); sameReading(); ++scenarios;
    reset(); mock.status = 408; assert(!send()); assert(mock.sends == 6); ++scenarios;
    reset(); mock.drop = true; assert(send());
    assert(mock.connections == 2 && mock.sends == 2); sameReading(); ++scenarios;
    reset(); mock.successConnection = 2; assert(send());
    assert(mock.connections == 2 && mock.time >= 15000); ++scenarios;
    reset(); mock.successConnection = 0; assert(!send());
    assert(mock.connections == 2 && mock.sends == 0);
    assert(mock.time >= 30000 && mock.time < 31000); ++scenarios;
    reset(); mock.dhcp = false; assert(!send() && mock.connections == 0); ++scenarios;
    reset(); mock.mode = false; assert(!send() && mock.connections == 0); ++scenarios;
    reset(); mock.begin = false; assert(!send() && mock.sends == 0); ++scenarios;
    reset(); mock.haveGateway = false; assert(!send() && mock.sends == 0); ++scenarios;
    reset(); assert(!send("LocalESP32", "short") && mock.connections == 0); ++scenarios;
    reset(); assert(send("", ""));
    assert(mock.ssid == brewDefaultSsid && mock.password == brewDefaultPassword); ++scenarios;
    reset(); mock.time = UINT32_MAX - 100; mock.successConnection = 0;
    assert(!send()); assert(mock.connections == 2); ++scenarios;
    reset(); assert(!brewSendReading(NAN, 45, 1, 4, "local", "password"));
    assert(mock.connections == 0); ++scenarios;
    reset(); assert(send()); assert(send());
    StaticJsonDocument<768> first, second;
    assert(!deserializeJson(first, &mock.packets[0][0], mock.packets[0].size()));
    assert(!deserializeJson(second, &mock.packets[1][0], mock.packets[1].size()));
    assert(second["sequence"].as<uint32_t>() == first["sequence"].as<uint32_t>() + 1);
    assert(second["boot_id"].as<uint32_t>() == first["boot_id"].as<uint32_t>()); ++scenarios;
    reset(); brewSequence = UINT32_MAX; assert(send());
    assert(brewSequence == 1); ++scenarios;
    std::cout << scenarios << " HTTP sender scenarios passed\n";
}
'''

def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("g++")
    if not compiler:
        raise SystemExit("Host g++ required; pass its path as an argument.")
    BUILD.mkdir(parents=True, exist_ok=True)
    for name, source in {
        "ESP8266WiFi.h": WIFI_MOCK,
        "ESP8266HTTPClient.h": HTTP_MOCK,
        "user_interface.h": "#pragma once\n#include <stdint.h>\ninline uint32_t os_random() { return 123; }\n",
        "transport_test.cpp": TEST,
    }.items():
        (BUILD / name).write_text(source, encoding="utf-8")
    executable = BUILD / ("transport_test.exe" if sys.platform == "win32" else "transport_test")
    subprocess.run([
        compiler, "-std=c++11", "-Wall", "-Wextra",
        "-I" + str(BUILD), "-I" + str(FIRMWARE), "-I" + str(ARDUINO_JSON),
        str(BUILD / "transport_test.cpp"), "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=10)
    print(result.stdout, end="")
    sample = next(line[7:] for line in result.stdout.splitlines() if line.startswith("SAMPLE "))
    (BUILD / "sender-sample.json").write_text(sample, encoding="utf-8")

if __name__ == "__main__":
    main()
