#include "protocol.h"

#include <cassert>
#include <iostream>

using namespace ews;

int main() {
    Key key;
    assert(parseKey("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key));
    assert(!parseKey("0000000000000000000000000000000000000000000000000000000000000000", key));
    assert(!parseKey("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1ez", key));
    assert(!parseKey("123", key));

    Identity identity;
    const char* roverConfig = "CFG R 4 2 000102030405060708090a0b0c0d0e0f"
                              "101112131415161718191a1b1c1d1e1f";
    assert(parseConfig(roverConfig, Role::Rover, identity, key));
    assert(identity.id == 4 && identity.route == 2);
    assert(!parseConfig(roverConfig, Role::Node, identity, key));
    assert(!parseConfig("CFG N 4 0 short", Role::Node, identity, key));
    identity.id = 9;
    assert(!validIdentity(Role::Rover, identity));

    Message sos;
    sos.report.sequence = 1;
    sos.report.rover = 1;
    assert(parseName("ALPHA-1", sos.report.name));
    sos.nonce = UINT64_C(0x1122334455667788);
    Frame wire;
    assert(encode(key, sos, wire));
    const Frame golden = {{
        0x45, 0x57, 0x03, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x41,
        0x4c, 0x50, 0x48, 0x41, 0x2d, 0x31, 0x00, 0x00, 0x88, 0x77,
        0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xbd, 0x7b, 0x87, 0x45,
        0x6d, 0x91, 0x85, 0x69, 0x01, 0x4e, 0x11, 0x17, 0x7d, 0x81,
        0xb8, 0x98
    }};
    assert(wire == golden);

    Message received;
    assert(decode(key, wire.data(), wire.size(), received));
    assert(received.nonce == sos.nonce && received.report.kind == Kind::Sos && !received.report.fix);
    assert(received.report.name == sos.report.name);
    RoverName invalidName{};
    assert(!parseName("ALPHA-TOO", invalidName));
    assert(!parseName("Alpha", invalidName));
    auto invalid = sos;
    invalid.report.name[1] = 0;  // Reject bytes hidden beyond the first NUL.
    assert(!encode(key, invalid, wire));
    assert(!decode(key, wire.data(), wire.size() - 1, received));
    assert(!decode(key, wire.data(), wire.size() + 1, received));
    Key wrong = key;
    wrong[0] ^= 1;
    assert(!decode(wrong, wire.data(), wire.size(), received));
    for (size_t i = 0; i < wire.size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            wire[i] ^= 1 << bit;
            assert(!decode(key, wire.data(), wire.size(), received));
            wire[i] ^= 1 << bit;
        }
    }

    Report receiverState;
    assert(classify(sos.report, receiverState) == Arrival::New);
    receiverState = sos.report;
    assert(classify(sos.report, receiverState) == Arrival::Duplicate);

    Message ack;
    ack.report.kind = Kind::Ack;
    ack.report.sequence = receiverState.sequence;
    ack.report.rover = 1;
    ack.nonce = sos.nonce;
    assert(encode(key, ack, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(matchesAck(received, sos));
    sos.nonce++;
    assert(!matchesAck(received, sos));  // Delayed ACK from previous attempt.
    assert(classify(received.report, receiverState) == Arrival::Reject);

    sos.report.fix = true;
    sos.report.latitude = -9000000;
    sos.report.longitude = 18000000;
    assert(classify(sos.report, receiverState) == Arrival::Reject);  // Conflicting duplicate.
    sos.report.sequence++;
    assert(encode(key, sos, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(received.report.latitude == -9000000 && received.report.longitude == 18000000);
    assert(classify(received.report, receiverState) == Arrival::New);
    receiverState = received.report;
    sos.report.longitude++;
    assert(!encode(key, sos, wire));
    sos.report.fix = false;
    assert(!encode(key, sos, wire));  // Coordinates cannot be hidden under a no-fix flag.
    sos.report.latitude = sos.report.longitude = 0;

    Message clear;
    clear.report.kind = Kind::Clear;
    clear.report.sequence = 3;
    clear.report.rover = 1;
    clear.nonce = 7;
    assert(encode(key, clear, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(classify(received.report, receiverState) == Arrival::New);
    const ReportBytes nvs = encodeReport(received.report);
    Report afterReset;
    assert(decodeReport(nvs.data(), afterReset));
    assert(classify(sos.report, afterReset) == Arrival::Reject);
    assert(classify(clear.report, afterReset) == Arrival::Duplicate);
    auto legacy = nvs;
    legacy[2] = 2;
    Report fromV2;
    assert(decodeStoredReport(legacy.data(), LegacyReportSize, fromV2));
    assert(fromV2.kind == Kind::Clear && fromV2.sequence == clear.report.sequence && !fromV2.name[0]);
    assert(!decodeStoredReport(legacy.data(), LegacyReportSize - 1, fromV2));

    Report peer;
    assert(peerReport(sos, 2, peer));
    assert(!peerReport(sos, 1, peer));
    assert(!peerReport(clear, 2, peer));  // An unconfirmed cancellation cannot clear a peer.
    const auto clearAck = makeAck(clear);
    assert(clearAck.report.kind == Kind::ClearAck);
    assert(!acceptsAtBase(clearAck));
    assert(matchesAck(clearAck, clear));
    auto wrongKind = clearAck;
    wrongKind.report.kind = Kind::Ack;
    assert(!matchesAck(wrongKind, clear));
    assert(peerReport(clearAck, 2, peer) && peer.kind == Kind::Clear);
    assert(classify(peer, receiverState) == Arrival::New);
    assert(classify(sos.report, peer) == Arrival::Reject);

    Message other = clear;
    other.report.rover = 2;
    assert(classify(other.report, afterReset) == Arrival::Reject);
    assert(classify(other.report, Report{}) == Arrival::New);
    assert(acceptsAtBase(other));
    other.report.rover = MaxRovers + 1;
    assert(!encode(key, other, wire));

    Message routed = sos;
    routed.report.sequence = 15;
    routed.report.route = 2;
    const Message original = routed;
    assert(!acceptsAtBase(routed));
    Relay node;
    const uint32_t relayAt = UINT32_MAX - 40;
    assert(!node.uplink(routed, 1, relayAt));
    assert(node.uplink(routed, 2, relayAt));
    assert(!node.uplink(routed, 2, relayAt + 1));
    assert(encode(key, routed, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(acceptsAtBase(received));
    Message baseAck = makeAck(received);
    assert(!acceptsAtBase(baseAck));
    assert(!matchesAck(baseAck, original));
    Message wrongAck = baseAck;
    wrongAck.report.rover++;
    assert(!node.downlink(wrongAck, relayAt + 50));
    wrongAck = baseAck;
    wrongAck.nonce++;
    assert(!node.downlink(wrongAck, relayAt + 60));
    assert(node.downlink(baseAck, relayAt + 70));
    assert(!node.downlink(baseAck, relayAt + 80));
    assert(encode(key, baseAck, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(matchesAck(received, original));
    routed = original;
    assert(!node.uplink(routed, 2, relayAt + RelayGapMs));  // Exact replay.
    routed.nonce++;
    assert(!node.uplink(routed, 2, relayAt + RelayGapMs - 1));
    assert(node.uplink(routed, 2, relayAt + RelayGapMs));
    baseAck = makeAck(routed);
    assert(!node.downlink(baseAck, relayAt + RelayGapMs + RelayWindowMs));

    Message position = original;
    position.report.kind = Kind::Position;
    assert(encode(key, position, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(!acceptsAtBase(received) && !acceptsTelemetryAtBase(received));
    assert(!peerReport(received, 2, peer));
    assert(classify(received.report, Report{}) == Arrival::Reject);
    assert(!node.uplink(received, 2, relayAt + 2 * RelayGapMs));
    assert(!decodeStoredReport(wire.data(), ReportSize, peer));
    received.forwarded = true;
    assert(acceptsTelemetryAtBase(received) && !acceptsAtBase(received));
    received.forwarded = false;
    received.report.route = 0;
    assert(acceptsTelemetryAtBase(received));
    TelemetryCursor cursor;
    assert(cursor.accept(received));
    assert(!cursor.accept(received));
    received.report.sequence++;
    assert(cursor.accept(received));
    received.report.sequence--;
    assert(!cursor.accept(received));
    const auto oldSession = received;
    received.nonce++;
    received.report.sequence = 1;
    assert(cursor.accept(received));
    assert(!cursor.accept(oldSession));

    Message heartbeat;
    heartbeat.report.kind = Kind::NodeStatus;
    heartbeat.report.rover = 2;
    heartbeat.report.sequence = 1;
    heartbeat.nonce = 22;
    assert(encode(key, heartbeat, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(acceptsTelemetryAtBase(received) && !acceptsAtBase(received));
    heartbeat.report.route = 2;
    assert(!encode(key, heartbeat, wire));
    heartbeat.report.route = 0;
    heartbeat.report.fix = true;
    assert(!encode(key, heartbeat, wire));

    uint32_t next, limit;
    assert(reserveSequenceBlock(0, next, limit) && next == 1 && limit == 256);
    assert(reserveSequenceBlock(limit, next, limit) && next == 257 && limit == 512);
    assert(!reserveSequenceBlock(5, next, limit));
    assert(!reserveSequenceBlock(UINT32_MAX - 255, next, limit));

    SosButton button;
    assert(!button.update(true, 0, 2000));
    assert(!button.update(true, 30, 2000));
    assert(!button.update(true, 9000, 2000));
    assert(!button.update(false, 9010, 2000));
    assert(!button.update(false, 9040, 2000));

    const auto edge = [&button](bool pressed, uint32_t now, uint32_t hold) {
        assert(!button.update(pressed, now, hold));
        assert(!button.update(pressed, now + 30, hold));
    };
    edge(true, 9100, 2000);
    assert(!button.update(true, 12000, 2000)); // Single long hold.
    edge(false, 12010, 2000);
    edge(true, 12100, 2000);
    edge(false, 12200, 2000);
    edge(true, 13300, 2000);
    assert(!button.update(true, 16000, 2000)); // Late second press.
    edge(false, 16010, 2000);

    assert(!button.update(true, 16100, 2000));
    assert(!button.update(false, 16110, 2000)); // Contact bounce.
    edge(true, 16120, 2000);
    edge(false, 16220, 2000);
    edge(true, 16400, 2000);
    assert(!button.update(true, 18429, 2000));
    assert(button.update(true, 18430, 2000));
    assert(!button.update(true, 23000, 3000)); // Release required.
    edge(false, 23010, 3000);

    const uint32_t wrap = UINT32_MAX - 250;
    edge(true, wrap, 3000);
    edge(false, wrap + 100, 3000);
    edge(true, wrap + 220, 3000);
    assert(!button.update(true, wrap + 3249, 3000));
    assert(button.update(true, wrap + 3250, 3000));
    std::cout << "PASS: protocol, authenticated round trip, fleet routing, replay handling, state, and button\n";
}
