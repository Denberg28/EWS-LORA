#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <U8x8lib.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "protocol.h"

#ifdef EWS_ROVER
#include <TinyGPSPlus.h>
#include <bootloader_random.h>
#include <esp_system.h>
#include <cmath>
#endif

#ifndef EWS_FREQ_MHZ
#define EWS_FREQ_MHZ 916.0f
#endif
#ifndef EWS_POWER_DBM
#define EWS_POWER_DBM 10
#endif

namespace {
using namespace ews;

constexpr int ButtonPin = 21;
constexpr int LedPin = 37;
constexpr uint32_t AckWindowMs = RelayWindowMs;
constexpr uint32_t BaseTxGapMs = 18000;
constexpr uint32_t FixMaxAgeMs = 10000;

uint8_t deviceId = 0;
uint8_t routeId = 0;

Preferences store;
Key key{};
U8X8_SSD1306_128X64_NONAME_HW_I2C screen(U8X8_PIN_NONE, 17, 18);
SX1262 radio = new Module(7, 33, 8, 34);
bool storeFault = false;
bool radioReady = false;
bool radioAttempted = false;
bool provisioned = false;
bool provisionEnabled = false;
bool screenReady = false;
uint32_t radioAt = 0;

#ifdef EWS_ROVER
TinyGPSPlus gps;
HoldButton button;
Mode mode = Mode::Idle;
uint32_t reservedThrough = 0;
uint32_t nextSequence = 0;
bool fixValid = false;
uint32_t fixAt = 0;
int32_t latitude = 0;
int32_t longitude = 0;
Message sent;
uint32_t sentAt = 0;
uint32_t jitterMs = 0;
bool sentOnce = false;
bool waitingAck = false;
bool recentAck = false;
bool lastAttemptAcked = false;
#elif defined(EWS_NODE)
Relay relay;
uint32_t forwardedCount = 0;
#else
struct RoverRecord {
    Report report;
    bool restored = false;
};
RoverRecord records[MaxRovers];
bool ackSent = false;
uint32_t ackSentAt = 0;
#endif

void row(uint8_t y, const char* text) {
    static char previous[8][17];
    if (!screenReady) {
        return;
    }
    char line[17];
    snprintf(line, sizeof(line), "%-16.16s", text);
    if (std::strcmp(line, previous[y]) != 0) {
        Wire.beginTransmission(0x3c);
        if (Wire.endTransmission() != 0) {
            screenReady = false;
            Serial.println("OLED ERROR - USE LED AND USB");
            return;
        }
        screen.drawString(0, y, line);
        memcpy(previous[y], line, sizeof(line));
    }
}

void setStoreFault() {
    storeFault = true;
    if (radioReady) {
        radio.standby();
        radioReady = false;
    }
    Serial.println("STORE ERROR");
}

bool validIdentity(uint8_t id, uint8_t route) {
#if defined(EWS_ROVER)
    return id >= 1 && id <= MaxRovers && route <= MaxNodes;
#elif defined(EWS_NODE)
    return id >= 1 && id <= MaxNodes && route == 0;
#else
    return id == 0 && route == 0;
#endif
}

bool loadConfig() {
    if (!store.isKey("config")) {
        return false;
    }
    uint8_t bytes[KeySize + 2];
    if (store.getBytesLength("config") != sizeof(bytes) ||
        store.getBytes("config", bytes, sizeof(bytes)) != sizeof(bytes) ||
        !validIdentity(bytes[0], bytes[1])) {
        setStoreFault();
        return false;
    }
    deviceId = bytes[0];
    routeId = bytes[1];
    uint8_t bits = 0;
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = bytes[i + 2];
        bits |= key[i];
    }
    if (!bits) {
        setStoreFault();
    }
    return !storeFault;
}

void provision() {
    static char line[73];
    static size_t size = 0;
    static bool overflow = false;
    for (unsigned i = 0; i < 96 && Serial.available(); ++i) {
        const int ch = Serial.read();
        if (ch != '\n') {
            if (size < sizeof(line) - 1 && ch >= 32 && ch <= 126) {
                line[size++] = static_cast<char>(ch);
            } else {
                overflow = true;
            }
            continue;
        }
        line[size] = '\0';
        const uint8_t id = static_cast<uint8_t>(line[4] - '0');
        const uint8_t route = static_cast<uint8_t>(line[6] - '0');
        const bool valid = !overflow && size == 72 && std::strncmp(line, "CFG ", 4) == 0 &&
                           line[5] == ' ' && line[7] == ' ' && validIdentity(id, route) &&
                           parseKey(line + 8, key);
        memset(line, 0, sizeof(line));
        size = 0;
        overflow = false;
        if (!valid) {
            Serial.println("INVALID CONFIG");
            continue;
        }
        uint8_t bytes[KeySize + 2] = {id, route};
        memcpy(bytes + 2, key.data(), key.size());
        if (store.putBytes("config", bytes, sizeof(bytes)) != sizeof(bytes)) {
            setStoreFault();
            return;
        }
        provisionEnabled = false;
        Serial.println("CONFIG STORED - RESET BOARD");
        return;
    }
}

void radioFault(int16_t status) {
    radioReady = false;
    radioAt = millis();
    Serial.printf("RADIO ERROR %d\n", status);
#ifdef EWS_ROVER
    waitingAck = false;
    lastAttemptAcked = false;
#endif
}

bool listen() {
    const int16_t status = radio.startReceive();
    if (status != RADIOLIB_ERR_NONE) {
        radioFault(status);
        return false;
    }
    return true;
}

void initRadio(uint32_t now) {
    if (radioReady || (radioAttempted && !elapsed(now, radioAt, 2000))) {
        return;
    }
    radioAttempted = true;
    radioAt = now;
    // The vendor T3-S3 SX1262 example uses RadioLib's 1.6 V TCXO default.
    int16_t status = radio.begin(EWS_FREQ_MHZ, 125.0, 8, 5, 0x12, EWS_POWER_DBM, 8, 1.6);
    if (status == RADIOLIB_ERR_NONE) {
        status = radio.setDio2AsRfSwitch(true);
    }
    if (status != RADIOLIB_ERR_NONE) {
        radioFault(status);
        return;
    }
#ifdef EWS_ROVER
    radioReady = true;
#else
    radioReady = listen();
#endif
    if (radioReady) {
        Serial.println("RADIO READY");
    }
}

bool receive(Message& message) {
    if (!digitalRead(33)) {
        return false;
    }
    Frame frame;
    bool valid = false;
    if (radio.getPacketLength() == frame.size()) {
        valid = radio.readData(frame.data(), frame.size()) == RADIOLIB_ERR_NONE &&
                decode(key, frame.data(), frame.size(), message);
    }
    return listen() && valid;
}

bool transmit(const Message& message) {
    Frame frame;
    if (!encode(key, message, frame)) {
        radioFault(RADIOLIB_ERR_UNKNOWN);
        return false;
    }
    const int16_t status = radio.transmit(frame.data(), frame.size());
    if (status != RADIOLIB_ERR_NONE) {
        radioFault(status);
        return false;
    }
    return true;
}

#ifdef EWS_ROVER
bool saveBeacon(Mode nextMode, uint32_t limit) {
    uint8_t bytes[5] = {static_cast<uint8_t>(nextMode)};
    write32(bytes + 1, limit);
    if (store.putBytes("tx-v1", bytes, sizeof(bytes)) != sizeof(bytes)) {
        setStoreFault();
        return false;
    }
    mode = nextMode;
    reservedThrough = limit;
    return true;
}

void loadRole() {
    if (!store.isKey("tx-v1")) {
        return;
    }
    uint8_t bytes[5];
    if (store.getBytesLength("tx-v1") != sizeof(bytes) ||
        store.getBytes("tx-v1", bytes, sizeof(bytes)) != sizeof(bytes) ||
        bytes[0] > static_cast<uint8_t>(Mode::Clear) ||
        read32(bytes + 1) % SequenceBlock != 0) {
        setStoreFault();
        return;
    }
    mode = static_cast<Mode>(bytes[0]);
    reservedThrough = read32(bytes + 1);
}

bool setMode(Mode nextMode) {
    if (!saveBeacon(nextMode, reservedThrough)) {
        return false;
    }
    // Retain the previous airtime timer across local mode changes.
    sent = Message{};
    recentAck = false;
    waitingAck = false;
    lastAttemptAcked = false;
    return true;
}

bool nextReport(Report& report) {
    if (!nextSequence || nextSequence > reservedThrough) {
        uint32_t first;
        uint32_t limit;
        if (!reserveSequenceBlock(reservedThrough, first, limit)) {
            setStoreFault();
            return false;
        }
        if (!saveBeacon(mode, limit)) {
            return false;
        }
        nextSequence = first;
    }
    report.sequence = nextSequence++;
    return true;
}

void updateGps(uint32_t now) {
    // Bound serial work per loop. No GPS wire is needed to send an SOS.
    for (unsigned i = 0; i < 128 && Serial1.available(); ++i) {
        gps.encode(Serial1.read());
    }
    if (gps.location.isUpdated()) {
        const bool valid = gps.location.isValid() && gps.location.age() <= FixMaxAgeMs;
        const double lat = gps.location.lat();
        const double lon = gps.location.lng();
        if (valid && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
            latitude = static_cast<int32_t>(std::lround(lat * 100000));
            longitude = static_cast<int32_t>(std::lround(lon * 100000));
            fixAt = now;
            fixValid = true;
        }
    }
    if (fixValid && elapsed(now, fixAt, FixMaxAgeMs)) {
        fixValid = false;
    }
}

void runRole(uint32_t now) {
    updateGps(now);
    if (button.update(digitalRead(ButtonPin) == LOW, now, mode == Mode::Sos ? 5000 : 2000)) {
        if (setMode(mode == Mode::Sos ? Mode::Clear : Mode::Sos)) {
            Serial.println(mode == Mode::Sos ? "SOS LATCHED" : "CLEAR PENDING");
        }
    }
    if (!radioReady || mode == Mode::Idle) {
        return;
    }
    if (waitingAck) {
        if (elapsed(now, sentAt, AckWindowMs)) {
            waitingAck = false;
            lastAttemptAcked = false;
            recentAck = false;
        } else {
            Message ack;
            if (receive(ack) && matchesAck(ack, sent)) {
                waitingAck = false;
                recentAck = true;
                lastAttemptAcked = true;
                Serial.printf("RX ACK %lu\n", static_cast<unsigned long>(sent.report.sequence));
                if (mode == Mode::Clear) {
                    setMode(Mode::Idle);
                }
            }
        }
        return;
    }
    const uint32_t retryMs = routeId ? 40000 : 20000;
    const uint32_t interval = retryMs + jitterMs + (lastAttemptAcked ? 10000 : 0);
    if (sentOnce && !elapsed(now, sentAt, interval)) {
        return;
    }

    Report report;
    report.rover = deviceId;
    report.route = routeId;
    report.kind = mode == Mode::Sos ? Kind::Sos : Kind::Clear;
    report.sequence = sent.report.sequence;
    report.fix = mode == Mode::Sos && fixValid;
    if (report.fix) {
        report.latitude = latitude;
        report.longitude = longitude;
    }
    if ((!report.sequence || encodeReport(report) != encodeReport(sent.report)) &&
        !nextReport(report)) {
        return;
    }

    sent.report = report;
    bootloader_random_enable();  // SAR entropy. Wi-Fi, BLE and ADC are unused.
    esp_fill_random(&sent.nonce, sizeof(sent.nonce));
    bootloader_random_disable();
    if (!sent.nonce) {
        return;
    }
    jitterMs = static_cast<uint32_t>(sent.nonce % 8000);
    sentAt = millis();
    sentOnce = true;
    if (transmit(sent) && listen()) {
        waitingAck = true;
        Serial.printf("TX %s %lu %s\n", mode == Mode::Sos ? "SOS" : "CLEAR",
                      static_cast<unsigned long>(report.sequence), report.fix ? "FIX" : "NO FIX");
    }
}

void roleDisplay(uint32_t now) {
    row(2, mode == Mode::Idle ? "READY" : mode == Mode::Sos ? "SOS ACTIVE" : "CLEAR PENDING");
    row(3, mode == Mode::Idle ? "HOLD 2s FOR SOS" : recentAck ? "RX ACK SEEN" : "WAIT ACK");
    row(4, fixValid ? "GPS FIX" : "NO GPS FIX");
    row(5, mode == Mode::Sos ? "HOLD 5s: CLEAR" : mode == Mode::Clear ? "HOLD 2s: SOS" : "");
    char identity[17];
    snprintf(identity, sizeof(identity), "ID %u / %s %u", deviceId,
             routeId ? "NODE" : "DIRECT", routeId);
    row(6, identity);
    digitalWrite(LedPin, mode != Mode::Idle && (recentAck || (now / 250) % 2));
}
#elif defined(EWS_NODE)
void loadRole() {}

void runRole(uint32_t now) {
    Message message;
    if (!radioReady || !receive(message)) {
        return;
    }
    const bool isAck = message.report.kind == Kind::Ack;
    if (!(isAck ? relay.downlink(message, now) : relay.uplink(message, deviceId, now))) {
        return;
    }
    delay(100);  // Let the previous sender turn around before a relay transmission.
    if (transmit(message)) {
        ++forwardedCount;
        listen();
        Serial.printf("FORWARDED %s rover=%u seq=%lu\n", isAck ? "ACK" : "REPORT",
                      message.report.rover, static_cast<unsigned long>(message.report.sequence));
    }
}

void roleDisplay(uint32_t now) {
    char line[17];
    snprintf(line, sizeof(line), "NODE %u", deviceId);
    row(2, line);
    const bool awaiting = relay.awaiting(now);
    row(3, awaiting ? "WAITING BASE ACK" : "LISTENING");
    snprintf(line, sizeof(line), "FWD %lu", static_cast<unsigned long>(forwardedCount));
    row(4, line);
    digitalWrite(LedPin, awaiting);
}
#else
void reportKey(uint8_t rover, char (&out)[5]) {
    snprintf(out, sizeof(out), "r%u", rover);
}

void logReport(const char* action, const Report& report) {
    Serial.printf("%s %s rover=%u route=%u seq=%lu", action,
                  report.kind == Kind::Sos ? "SOS" : "CLEAR", report.rover, report.route,
                  static_cast<unsigned long>(report.sequence));
    if (report.fix) {
        Serial.printf(" lat=%.5f lon=%.5f", report.latitude / 100000.0, report.longitude / 100000.0);
    } else if (report.kind == Kind::Sos) {
        Serial.print(" NO GPS FIX");
    }
    Serial.println();
}

void loadRole() {
    for (uint8_t id = 1; id <= MaxRovers; ++id) {
        char storageKey[5];
        reportKey(id, storageKey);
        if (!store.isKey(storageKey)) {
            continue;
        }
        auto& record = records[id - 1];
        ReportBytes bytes;
        if (store.getBytesLength(storageKey) != bytes.size() ||
            store.getBytes(storageKey, bytes.data(), bytes.size()) != bytes.size() ||
            !decodeReport(bytes.data(), record.report) || record.report.kind == Kind::Ack ||
            record.report.rover != id) {
            setStoreFault();
            return;
        }
        record.restored = true;
        logReport("RESTORED", record.report);
    }
}

void runRole(uint32_t now) {
    Message request;
    if (!radioReady || !receive(request) || !acceptsAtBase(request)) {
        return;
    }
    auto& record = records[request.report.rover - 1];  // decode validates the ID.
    const Arrival arrival = classify(request.report, record.report);
    if (arrival == Arrival::Reject) {
        return;
    }
    if (arrival == Arrival::New) {
        char storageKey[5];
        reportKey(request.report.rover, storageKey);
        const auto bytes = encodeReport(request.report);
        if (store.putBytes(storageKey, bytes.data(), bytes.size()) != bytes.size()) {
            setStoreFault();
            return;
        }
        record.report = request.report;
        record.restored = false;
    }
    if (!ackSent || elapsed(now, ackSentAt, BaseTxGapMs)) {
        delay(100);
        ackSentAt = millis();
        ackSent = true;
        if (transmit(makeAck(request))) {
            listen();
        }
    }
    if (arrival == Arrival::New) {
        logReport("RECEIVED", record.report);
    }
}

void roleDisplay(uint32_t now) {
    const RoverRecord* active[MaxRovers];
    uint8_t count = 0;
    for (const auto& record : records) {
        if (record.report.sequence && record.report.kind == Kind::Sos) {
            active[count++] = &record;
        }
    }
    if (!count) {
        row(2, "LISTENING");
        row(3, "NO ACTIVE SOS");
        row(4, "");
        row(5, "");
        row(6, "");
        digitalWrite(LedPin, LOW);
        return;
    }

    const auto& record = *active[(now / 3500) % count];
    char line[17];
    snprintf(line, sizeof(line), "SOS R%u (%u TOTAL)", record.report.rover, count);
    row(2, line);
    row(3, record.restored ? "RESTORED RECORD" : "LAST REPORTED");
    if (record.report.fix) {
        snprintf(line, sizeof(line), "LAT %+.5f", record.report.latitude / 100000.0);
        row(4, line);
        snprintf(line, sizeof(line), "LON %+.5f", record.report.longitude / 100000.0);
        row(5, line);
    } else {
        row(4, "NO GPS FIX");
        row(5, "");
    }
    snprintf(line, sizeof(line), record.report.route ? "VIA NODE %u" : "DIRECT LINK",
             record.report.route);
    row(6, line);
    digitalWrite(LedPin, (now / 250) % 2);
}
#endif

void updateDisplay(uint32_t now) {
    static uint32_t lastDraw = 0;
    if (!elapsed(now, lastDraw, 200)) {
        return;
    }
    lastDraw = now;
    if (storeFault || !provisioned) {
        row(2, storeFault ? "STORE ERROR" : "CONFIG NEEDED");
        row(3, storeFault ? "RADIO DISABLED" : provisionEnabled ? "USB CONFIG MODE" : "RESET TO START");
        row(4, "");
        row(5, "");
        row(6, "");
        digitalWrite(LedPin, storeFault && now % 1000 < 100);
        return;
    }
    roleDisplay(now);
    row(7, radioReady ? "" : "RADIO ERROR");
    if (!radioReady) {
        digitalWrite(LedPin, now % 1000 < 100);
    }
}
}  // namespace

void setup() {
    pinMode(ButtonPin, INPUT_PULLUP);
    pinMode(LedPin, OUTPUT);
    digitalWrite(LedPin, LOW);
    delay(40);
    const bool serviceRequested = digitalRead(ButtonPin) == LOW;
    Serial.begin(115200);
    // Arduino-ESP32 2.0.x / ESP-IDF 4.4 watchdog API.
    const esp_err_t watchdog = esp_task_wdt_init(8, true);
    if (watchdog != ESP_OK ||
        (esp_task_wdt_status(nullptr) != ESP_OK && esp_task_wdt_add(nullptr) != ESP_OK)) {
        setStoreFault();  // Disable operation when the required watchdog cannot be armed.
    }
    Wire.begin(18, 17);
    Wire.setTimeOut(20);
    Wire.beginTransmission(0x3c);
    screenReady = Wire.endTransmission() == 0;
    if (screenReady) {
        screen.begin();
        Wire.setTimeOut(20);
        screen.setFont(u8x8_font_chroma48medium8_r);
    } else {
        Serial.println("OLED ERROR - USE LED AND USB");
    }
#ifdef EWS_ROVER
    row(0, "EWS LORA ROVER");
    Serial1.begin(9600, SERIAL_8N1, 44, -1);
#elif defined(EWS_NODE)
    row(0, "EWS LORA NODE");
#else
    row(0, "EWS LORA BASE");
#endif
    SPI.begin(5, 3, 6, 7);
    if (storeFault || !store.begin("ews2", false)) {
        setStoreFault();
        return;
    }
    provisioned = loadConfig();
    if (provisioned) {
        loadRole();
    } else {
        provisionEnabled = serviceRequested && !storeFault;
    }
}

void loop() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_reset();
    }
    const uint32_t now = millis();
    if (!storeFault && provisioned) {
        initRadio(now);
        runRole(millis());
    } else if (!storeFault && provisionEnabled) {
        provision();
    }
    updateDisplay(millis());
    delay(5);
}
