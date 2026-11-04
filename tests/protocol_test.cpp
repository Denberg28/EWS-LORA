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

    Message sos;
    sos.report.sequence = 1;
    sos.report.rover = 1;
    sos.nonce = UINT64_C(0x1122334455667788);
    Frame wire;
    assert(encode(key, sos, wire));
    const Frame golden = {{
        0x45, 0x57, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xc7, 0x8c,
        0x44, 0xc9, 0xb4, 0x4b, 0x42, 0x6b, 0xc4, 0xe9, 0xc8, 0xed,
        0xba, 0xff, 0x1c, 0x65
    }};
    assert(wire == golden);

    Message received;
    assert(decode(key, wire.data(), wire.size(), received));
    assert(received.nonce == sos.nonce && received.report.kind == Kind::Sos && !received.report.fix);
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

    uint32_t next, limit;
    assert(reserveSequenceBlock(0, next, limit) && next == 1 && limit == 256);
    assert(reserveSequenceBlock(limit, next, limit) && next == 257 && limit == 512);
    assert(!reserveSequenceBlock(5, next, limit));
    assert(!reserveSequenceBlock(UINT32_MAX - 255, next, limit));

    HoldButton heldAtBoot;
    assert(!heldAtBoot.update(true, 0, 2000));
    assert(!heldAtBoot.update(true, 30, 2000));
    assert(!heldAtBoot.update(true, 9000, 2000));
    assert(!heldAtBoot.update(false, 9001, 2000));
    assert(!heldAtBoot.update(false, 9031, 2000));
    assert(!heldAtBoot.update(true, 9100, 2000));
    assert(!heldAtBoot.update(false, 9110, 2000));
    assert(!heldAtBoot.update(true, 9120, 2000));
    assert(!heldAtBoot.update(true, 9150, 2000));
    assert(!heldAtBoot.update(true, 11149, 2000));
    assert(heldAtBoot.update(true, 11150, 2000));
    assert(!heldAtBoot.update(true, 20000, 5000));

    HoldButton clearButton;
    const uint32_t edge = UINT32_MAX - 100;
    assert(!clearButton.update(false, edge - 1, 5000));
    assert(!clearButton.update(true, edge, 5000));
    assert(!clearButton.update(true, edge + 30, 5000));
    assert(!clearButton.update(true, edge + 5029, 5000));
    assert(clearButton.update(true, edge + 5030, 5000));
    std::cout << "PASS: protocol, authenticated round trip, fleet routing, replay handling, state, and button\n";
}
