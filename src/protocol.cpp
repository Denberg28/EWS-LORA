#include "protocol.h"

#include <algorithm>
#include <cstring>
#ifdef ARDUINO
#include <mbedtls/md.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace ews {

void write32(uint8_t* out, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t read32(const uint8_t* in) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= uint32_t(in[i]) << (8 * i);
    }
    return value;
}

ReportBytes encodeReport(const Report& report) {
    ReportBytes out = {{'E', 'W', 3, static_cast<uint8_t>(report.kind), uint8_t(report.fix)}};
    write32(out.data() + 5, report.sequence);
    write32(out.data() + 9, static_cast<uint32_t>(report.latitude));
    write32(out.data() + 13, static_cast<uint32_t>(report.longitude));
    out[17] = report.rover;
    out[18] = report.route;
    std::copy_n(report.name.begin(), NameSize, out.begin() + LegacyReportSize);
    return out;
}

bool decodeReport(const uint8_t* in, Report& report) {
    if (in[0] != 'E' || in[1] != 'W' || in[2] != 3 || in[3] < 1 || in[3] > 7 ||
        in[4] > 1) {
        return false;
    }
    Report next;
    next.kind = static_cast<Kind>(in[3]);
    next.fix = in[4] != 0;
    next.sequence = read32(in + 5);
    const int64_t latitude = int64_t(read32(in + 9));
    const int64_t longitude = int64_t(read32(in + 13));
    const int64_t lat = latitude > INT32_MAX ? latitude - (INT64_C(1) << 32) : latitude;
    const int64_t lon = longitude > INT32_MAX ? longitude - (INT64_C(1) << 32) : longitude;
    next.rover = in[17];
    next.route = in[18];
    for (size_t i = 0; i < NameSize; ++i) {
        next.name[i] = static_cast<char>(in[LegacyReportSize + i]);
    }
    RoverName canonicalName{};
    if (!parseName(next.name.data(), canonicalName) || next.name != canonicalName) {
        return false;
    }
    if (!next.sequence || !next.rover || next.rover > MaxRovers || next.route > MaxNodes ||
        lat < -9000000 || lat > 9000000 || lon < -18000000 ||
        lon > 18000000 || (next.fix && isAck(next.kind)) ||
        (!next.fix && next.kind != Kind::Signal && (lat != 0 || lon != 0)) ||
        (next.kind == Kind::Signal &&
         (next.fix || !next.route || next.name[0] || lon || !validSignal(static_cast<int32_t>(lat)))) ||
        (next.kind == Kind::NodeStatus && (next.fix || next.route || next.name[0]))) {
        return false;
    }
    next.latitude = static_cast<int32_t>(lat);
    next.longitude = static_cast<int32_t>(lon);
    report = next;
    return true;
}

bool decodeStoredReport(const uint8_t* in, size_t size, Report& report) {
    if (size == ReportSize) {
        return decodeReport(in, report) && isIncident(report.kind);
    }
    if (size != LegacyReportSize || in[2] != 2 || (in[3] != 1 && in[3] != 2)) {
        return false;
    }
    ReportBytes upgraded{};
    std::copy_n(in, size, upgraded.begin());
    upgraded[2] = 3;
    return decodeReport(upgraded.data(), report);
}

bool parseName(const char* input, RoverName& name) {
    RoverName parsed{};
    for (size_t i = 0; input[i]; ++i) {
        const char ch = input[i];
        if (i == NameSize || !((ch >= 'A' && ch <= 'Z') ||
                               (ch >= '0' && ch <= '9') || ch == '-')) {
            return false;
        }
        parsed[i] = ch;
    }
    name = parsed;
    return true;
}

bool isAck(Kind kind) {
    return kind == Kind::Ack || kind == Kind::ClearAck;
}

bool isIncident(Kind kind) {
    return kind == Kind::Sos || kind == Kind::Clear;
}

bool validSignal(int32_t deciDbm) {
    return deciDbm >= -1450 && deciDbm <= -200;
}

bool needsSignal(const Message& message) {
    return message.report.kind == Kind::Sos && !message.report.fix;
}

Message makeSignal(const Message& sos, uint8_t node, int16_t deciDbm) {
    Message observation;
    observation.report.kind = Kind::Signal;
    observation.report.rover = sos.report.rover;
    observation.report.route = node;
    observation.report.sequence = sos.report.sequence;
    observation.report.latitude = deciDbm;
    observation.nonce = sos.nonce;
    return observation;
}

bool matchesSignal(const Message& observation, const Message& sos) {
    return observation.report.kind == Kind::Signal && !observation.forwarded &&
           observation.report.route >= 1 && observation.report.route <= MaxNodes &&
           validSignal(observation.report.latitude) && needsSignal(sos) && sos.nonce &&
           observation.report.rover == sos.report.rover &&
           observation.report.sequence == sos.report.sequence && observation.nonce == sos.nonce;
}

bool acceptsTelemetryAtBase(const Message& message) {
    return (message.report.kind == Kind::Position || message.report.kind == Kind::NodeStatus) &&
           message.forwarded == (message.report.route != 0);
}

bool TelemetryCursor::accept(const Message& message) {
    if (!message.nonce || !message.report.sequence ||
        (message.nonce == session && message.report.sequence <= sequence) ||
        message.nonce == previous) {
        return false;
    }
    if (message.nonce != session) {
        previous = session;
        session = message.nonce;
    }
    sequence = message.report.sequence;
    return true;
}

bool peerReport(const Message& message, uint8_t ownId, Report& report) {
    if (message.report.rover == ownId ||
        (message.report.kind != Kind::Sos && message.report.kind != Kind::ClearAck)) {
        return false;
    }
    report = message.report;
    if (report.kind == Kind::ClearAck) {
        report.kind = Kind::Clear;
    }
    return true;
}

static bool makeTag(const Key& key, const uint8_t* body, uint8_t* output) {
#ifdef ARDUINO
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key.data(), key.size(), body, BodySize, output) == 0;
#else
    unsigned int length = 0;
    return HMAC(EVP_sha256(), key.data(), key.size(), body, BodySize, output, &length) &&
           length == 32;
#endif
}

bool encode(const Key& key, const Message& message, Frame& frame) {
    const auto bytes = encodeReport(message.report);
    Report validated;
    if (!message.nonce || message.report.name.back() != 0 || (message.forwarded && (!message.report.route || message.report.kind == Kind::Signal)) || !decodeReport(bytes.data(), validated)) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), frame.begin());
    frame[ReportSize] = message.forwarded;
    for (size_t i = 0; i < 8; ++i) {
        frame[ReportSize + 1 + i] = static_cast<uint8_t>(message.nonce >> (8 * i));
    }
    uint8_t hash[32];
    if (!makeTag(key, frame.data(), hash)) {
        return false;
    }
    std::copy(hash, hash + TagSize, frame.begin() + BodySize);
    return true;
}

bool decode(const Key& key, const uint8_t* data, size_t length, Message& message) {
    if (length != FrameSize) {
        return false;
    }
    Message next;
    if (!decodeReport(data, next.report) || data[ReportSize] > 1 ||
        (data[ReportSize] && (!next.report.route || next.report.kind == Kind::Signal))) {
        return false;
    }
    next.forwarded = data[ReportSize] != 0;
    uint8_t hash[32];
    if (!makeTag(key, data, hash)) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < TagSize; ++i) {
        diff |= hash[i] ^ data[BodySize + i];
    }
    for (size_t i = 0; i < 8; ++i) {
        next.nonce |= uint64_t(data[ReportSize + 1 + i]) << (8 * i);
    }
    if (diff || !next.nonce) {
        return false;
    }
    message = next;
    return true;
}

bool parseKey(const char* hex, Key& key) {
    if (std::strlen(hex) != KeySize * 2) {
        return false;
    }
    Key parsed;
    uint8_t combined = 0;
    for (size_t i = 0; i < KeySize; ++i) {
        uint8_t byte = 0;
        for (size_t j = 0; j < 2; ++j) {
            const char digit = hex[i * 2 + j];
            if (digit >= '0' && digit <= '9') {
                byte = uint8_t(byte * 16 + digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                byte = uint8_t(byte * 16 + digit - 'a' + 10);
            } else {
                return false;
            }
        }
        parsed[i] = byte;
        combined |= byte;
    }
    if (!combined) {
        return false;
    }
    key = parsed;
    return true;
}

bool validIdentity(Role role, Identity identity) {
    switch (role) {
        case Role::Base:
            return identity.id == 0 && identity.route == 0;
        case Role::Node:
            return identity.id >= 1 && identity.id <= MaxNodes && identity.route == 0;
        case Role::Rover:
            return identity.id >= 1 && identity.id <= MaxRovers && identity.route <= MaxNodes;
    }
    return false;
}

bool parseConfig(const char* line, Role role, Identity& identity, Key& key) {
    if (std::strlen(line) != 74 || std::strncmp(line, "CFG ", 4) != 0 ||
        line[4] != static_cast<char>(role) || line[5] != ' ' || line[7] != ' ' ||
        line[9] != ' ') {
        return false;
    }
    Identity next;
    next.id = static_cast<uint8_t>(line[6] - '0');
    next.route = static_cast<uint8_t>(line[8] - '0');
    if (!validIdentity(role, next) || !parseKey(line + 10, key)) {
        return false;
    }
    identity = next;
    return true;
}

Arrival classify(const Report& incoming, const Report& stored) {
    if (!incoming.sequence || !isIncident(incoming.kind) ||
        (stored.sequence && incoming.rover != stored.rover) || incoming.sequence < stored.sequence) {
        return Arrival::Reject;
    }
    if (incoming.sequence > stored.sequence) {
        return Arrival::New;
    }
    return encodeReport(incoming) == encodeReport(stored) ? Arrival::Duplicate : Arrival::Reject;
}

bool acceptsAtBase(const Message& request) {
    return isIncident(request.report.kind) && request.forwarded == (request.report.route != 0);
}

Message makeAck(const Message& request) {
    Message ack;
    ack.report.kind = request.report.kind == Kind::Clear ? Kind::ClearAck : Kind::Ack;
    ack.report.name = request.report.name;
    ack.report.sequence = request.report.sequence;
    ack.report.rover = request.report.rover;
    ack.report.route = request.report.route;
    ack.nonce = request.nonce;
    return ack;
}

static bool sameTransaction(const Message& ack, const Message& sent) {
    const Kind expected = sent.report.kind == Kind::Clear ? Kind::ClearAck : Kind::Ack;
    return ack.report.kind == expected && ack.report.sequence == sent.report.sequence &&
           ack.report.rover == sent.report.rover && ack.report.route == sent.report.route &&
           ack.nonce == sent.nonce && sent.nonce != 0;
}

bool matchesAck(const Message& ack, const Message& sent) {
    return sameTransaction(ack, sent) && ack.forwarded == (sent.report.route != 0);
}

bool Relay::uplink(Message& request, uint8_t node, uint32_t now) {
    if (!node || request.report.route != node || !isIncident(request.report.kind) ||
        request.forwarded ||
        (used && (!elapsed(now, openedAt, RelayGapMs) || request.nonce == last.nonce))) {
        return false;
    }
    used = true;
    pending = true;
    openedAt = now;
    last = request;
    request.forwarded = true;
    return true;
}

bool Relay::awaiting(uint32_t now) const {
    return pending && !elapsed(now, openedAt, RelayWindowMs);
}

bool Relay::downlink(Message& ack, uint32_t now) {
    if (!awaiting(now) || ack.forwarded || !sameTransaction(ack, last)) {
        return false;
    }
    pending = false;
    ack.forwarded = true;
    return true;
}

bool reserveSequenceBlock(uint32_t highWater, uint32_t& next, uint32_t& limit) {
    if (highWater > UINT32_MAX - SequenceBlock || highWater % SequenceBlock != 0) {
        return false;
    }
    next = highWater + 1;
    limit = highWater + SequenceBlock;
    return true;
}

bool SosButton::update(bool pressed, uint32_t now, uint32_t holdMs) {
    constexpr uint32_t DebounceMs = 30;
    constexpr uint32_t MaxTapMs = 450;
    constexpr uint32_t SecondPressMs = 1000;
    if (pressed != raw) {
        raw = pressed;
        edgeAt = now;
    }
    if (!elapsed(now, edgeAt, DebounceMs)) {
        return false;
    }
    if (stable != raw) {
        stable = raw;
        if (stable) {
            if (state == State::Ready) {
                state = State::Tap;
            } else if (state == State::WaitSecond && !elapsed(now, tappedAt, SecondPressMs)) {
                state = State::Hold;
            } else {
                state = State::AwaitRelease;
            }
            pressedAt = now;
        } else if (state == State::Tap && !elapsed(now, pressedAt, MaxTapMs)) {
            state = State::WaitSecond;
            tappedAt = now;
        } else {
            state = State::Ready;
        }
    } else if (!stable && (state == State::AwaitRelease ||
                           (state == State::WaitSecond && elapsed(now, tappedAt, SecondPressMs)))) {
        state = State::Ready;
    }
    if (stable && state == State::Hold && elapsed(now, pressedAt, holdMs)) {
        state = State::AwaitRelease;
        return true;
    }
    return false;
}

}  // namespace ews
