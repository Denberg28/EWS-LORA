#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ews {

constexpr uint8_t MaxRovers = 8;
constexpr uint8_t MaxNodes = 8;
constexpr uint32_t RelayWindowMs = 4000;
constexpr uint32_t RelayGapMs = 38000;

constexpr size_t KeySize = 32;
constexpr size_t NameSize = 8;
constexpr size_t LegacyReportSize = 19;
constexpr size_t ReportSize = LegacyReportSize + NameSize;
constexpr size_t BodySize = ReportSize + 9;
constexpr size_t TagSize = 16;
constexpr size_t FrameSize = BodySize + TagSize;

using Key = std::array<uint8_t, KeySize>;
using RoverName = std::array<char, NameSize + 1>;
using ReportBytes = std::array<uint8_t, ReportSize>;
using Frame = std::array<uint8_t, FrameSize>;

enum class Kind : uint8_t { Sos = 1, Clear = 2, Ack = 3, ClearAck = 4, Position = 5, NodeStatus = 6, Signal = 7 };
enum class Mode : uint8_t { Idle, Sos, Clear };
enum class Role : uint8_t { Base = 'B', Node = 'N', Rover = 'R' };

struct Identity {
    uint8_t id = 0;
    uint8_t route = 0;
};

struct Report {
    Kind kind = Kind::Sos;
    uint32_t sequence = 0;
    bool fix = false;
    int32_t latitude = 0;
    int32_t longitude = 0;
    RoverName name{};
    uint8_t rover = 0;
    uint8_t route = 0;
};

struct Message {
    Report report;
    uint64_t nonce = 0;
    bool forwarded = false;
};

void write32(uint8_t* out, uint32_t value);
uint32_t read32(const uint8_t* in);
ReportBytes encodeReport(const Report& report);
bool decodeReport(const uint8_t* in, Report& report);
bool decodeStoredReport(const uint8_t* in, size_t size, Report& report);
bool parseName(const char* input, RoverName& name);
bool isAck(Kind kind);
bool isIncident(Kind kind);
constexpr uint32_t SignalStartMs = 4600;
constexpr uint32_t SignalSlotMs = 450;
constexpr uint32_t SignalWindowMs = 9500;
bool validSignal(int32_t deciDbm);
bool needsSignal(const Message& message);
Message makeSignal(const Message& sos, uint8_t node, int16_t deciDbm);
bool matchesSignal(const Message& observation, const Message& sos);
bool acceptsTelemetryAtBase(const Message& message);

// Volatile ordering for unacknowledged observations. Incident replay protection is separate.
class TelemetryCursor {
public:
    bool accept(const Message& message);

private:
    uint64_t session = 0;
    uint64_t previous = 0;
    uint32_t sequence = 0;
};
bool peerReport(const Message& message, uint8_t ownId, Report& report);
bool encode(const Key& key, const Message& message, Frame& frame);
bool decode(const Key& key, const uint8_t* data, size_t length, Message& message);
bool parseKey(const char* hex, Key& key);
bool validIdentity(Role role, Identity identity);
bool parseConfig(const char* line, Role role, Identity& identity, Key& key);

enum class Arrival { Reject, Duplicate, New };
Arrival classify(const Report& incoming, const Report& stored);
bool acceptsAtBase(const Message& request);
Message makeAck(const Message& request);
bool matchesAck(const Message& ack, const Message& sent);

class Relay {
public:
    bool uplink(Message& request, uint8_t node, uint32_t now);
    bool downlink(Message& ack, uint32_t now);
    bool awaiting(uint32_t now) const;

private:
    Message last;
    uint32_t openedAt = 0;
    bool used = false;
    bool pending = false;
};

constexpr uint32_t SequenceBlock = 256;
bool reserveSequenceBlock(uint32_t highWater, uint32_t& next, uint32_t& limit);

inline bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
    return now - since >= interval;
}

class SosButton {
public:
    bool update(bool pressed, uint32_t now, uint32_t holdMs);

private:
    uint32_t edgeAt = 0;
    uint32_t pressedAt = 0;
    uint32_t tappedAt = 0;
    enum class State { AwaitRelease, Ready, Tap, WaitSecond, Hold };
    State state = State::AwaitRelease;
    bool raw = false;
    bool stable = false;
};

}  // namespace ews
