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
    sos.nonce = UINT64_C(0x1122334455667788);
    Frame wire;
    assert(encode(key, sos, wire));
    const Frame golden = {{
        0x45, 0x57, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x77, 0x66,
        0x55, 0x44, 0x33, 0x22, 0x11, 0x46, 0x2c, 0x56, 0xc4, 0x41,
        0xf1, 0xd5, 0xc8, 0xb3, 0x63, 0x8b, 0x6b, 0xeb, 0x1a, 0x2b, 0x9d
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
    clear.nonce = 7;
    assert(encode(key, clear, wire));
    assert(decode(key, wire.data(), wire.size(), received));
    assert(classify(received.report, receiverState) == Arrival::New);
    const ReportBytes nvs = encodeReport(received.report);
    Report afterReset;
    assert(decodeReport(nvs.data(), afterReset));
    assert(classify(sos.report, afterReset) == Arrival::Reject);
    assert(classify(clear.report, afterReset) == Arrival::Duplicate);

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
    std::cout << "PASS: protocol, authenticated round trip, replay handling, state, and button\n";
}
