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
    ReportBytes out = {{'E', 'W', 2, static_cast<uint8_t>(report.kind), uint8_t(report.fix)}};
    write32(out.data() + 5, report.sequence);
    write32(out.data() + 9, static_cast<uint32_t>(report.latitude));
    write32(out.data() + 13, static_cast<uint32_t>(report.longitude));
    out[17] = report.rover;
    out[18] = report.route;
    return out;
}

bool decodeReport(const uint8_t* in, Report& report) {
    if (in[0] != 'E' || in[1] != 'W' || in[2] != 2 || in[3] < 1 || in[3] > 3 ||
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
    if (!next.sequence || !next.rover || next.rover > MaxRovers || next.route > MaxNodes ||
        lat < -9000000 || lat > 9000000 || lon < -18000000 ||
        lon > 18000000 || (next.fix && next.kind != Kind::Sos) ||
        (!next.fix && (lat != 0 || lon != 0))) {
        return false;
    }
    next.latitude = static_cast<int32_t>(lat);
    next.longitude = static_cast<int32_t>(lon);
    report = next;
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
    if (!message.nonce || (message.forwarded && !message.report.route) || !decodeReport(bytes.data(), validated)) {
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
        (data[ReportSize] && !next.report.route)) {
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

Arrival classify(const Report& incoming, const Report& stored) {
    if (!incoming.sequence || incoming.kind == Kind::Ack ||
        (stored.sequence && incoming.rover != stored.rover) || incoming.sequence < stored.sequence) {
        return Arrival::Reject;
    }
    if (incoming.sequence > stored.sequence) {
        return Arrival::New;
    }
    return encodeReport(incoming) == encodeReport(stored) ? Arrival::Duplicate : Arrival::Reject;
}

bool acceptsAtBase(const Message& request) {
    return request.report.kind != Kind::Ack && request.forwarded == (request.report.route != 0);
}

Message makeAck(const Message& request) {
    Message ack;
    ack.report.kind = Kind::Ack;
    ack.report.sequence = request.report.sequence;
    ack.report.rover = request.report.rover;
    ack.report.route = request.report.route;
    ack.nonce = request.nonce;
    return ack;
}

static bool sameTransaction(const Message& ack, const Message& sent) {
    return ack.report.kind == Kind::Ack && ack.report.sequence == sent.report.sequence &&
           ack.report.rover == sent.report.rover && ack.report.route == sent.report.route &&
           ack.nonce == sent.nonce && sent.nonce != 0;
}

bool matchesAck(const Message& ack, const Message& sent) {
    return sameTransaction(ack, sent) && ack.forwarded == (sent.report.route != 0);
}

bool Relay::uplink(Message& request, uint8_t node, uint32_t now) {
    if (!node || request.report.route != node || request.report.kind == Kind::Ack ||
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

bool HoldButton::update(bool pressed, uint32_t now, uint32_t holdMs) {
    if (pressed != raw) {
        raw = pressed;
        edgeAt = now;
    }
    if (!elapsed(now, edgeAt, 30)) {
        return false;
    }
    if (stable != raw) {
        stable = raw;
        pressedAt = now;
    }
    if (!stable) {
        armed = true;
        fired = false;
    } else if (armed && !fired && elapsed(now, pressedAt, holdMs)) {
        fired = true;
        return true;
    }
    return false;
}

}  // namespace ews
