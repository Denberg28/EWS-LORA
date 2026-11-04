// Base-only USB monitor feed. Observations are volatile and never change a saved incident.
struct RoverObservation {
    TelemetryCursor cursor;
    Report latest;
    int64_t contact = -1;
    int64_t fixAt = -1;
    uint32_t point = 0;
    uint32_t segment = 0;
    int32_t latitude = 0;
    int32_t longitude = 0;
    uint8_t fixState = 0;  // unknown, no fix, fix
};
struct NodeObservation {
    TelemetryCursor cursor;
    int64_t heartbeat = -1;
    int64_t forwarded = -1;
};
RoverObservation roverObservations[MaxRovers];
NodeObservation nodeObservations[MaxNodes];
uint64_t monitorBoot = 0;

int64_t monitorSeconds() {
    return esp_timer_get_time() / 1000000;
}

void observeRover(const Report& report) {
    auto& rover = roverObservations[report.rover - 1];
    const int64_t now = monitorSeconds();
    rover.contact = now;
    rover.latest = report;
    if (report.route) {
        nodeObservations[report.route - 1].forwarded = now;
    }
    // CLEAR can contain a cached incident coordinate. Do not draw it as a new fix.
    if (report.kind == Kind::Clear) {
        rover.fixState = 0;
        return;
    }
    if (report.fix) {
        if (rover.fixState != 2 || now - rover.fixAt > 170) {
            ++rover.segment;
        }
        rover.fixAt = now;
        rover.latitude = report.latitude;
        rover.longitude = report.longitude;
        ++rover.point;
    }
    rover.fixState = report.fix ? 2 : 1;
}

void observeTelemetry(const Message& message) {
    const auto& report = message.report;
    if (report.kind == Kind::NodeStatus) {
        auto& node = nodeObservations[report.rover - 1];
        if (node.cursor.accept(message)) {
            node.heartbeat = monitorSeconds();
        }
    } else if (roverObservations[report.rover - 1].cursor.accept(message)) {
        observeRover(report);
    }
}

long ageAt(int64_t now, int64_t then) {
    if (then < 0) {
        return -1;
    }
    const auto age = now - then;
    return age > INT32_MAX ? INT32_MAX : static_cast<long>(age);
}

void streamMonitor(uint32_t now) {
    static uint32_t started = 0;
    static uint8_t rowIndex = 0;
    static bool streaming = false;
    constexpr uint8_t EndRow = 1 + MaxRovers + MaxNodes;
    if (!Serial || (streaming && elapsed(now, started, 1500))) {
        streaming = false;
        return;
    }
    if (!streaming) {
        if (!elapsed(now, started, 2000)) {
            return;
        }
        started = now;
        rowIndex = 0;
        streaming = true;
    }

    const auto seconds = monitorSeconds();
    char line[112];
    int length;
    if (!rowIndex) {
        const char* state = storeFault ? "STORE" : !provisioned ? "CONFIG" :
                            radioReady ? "READY" : "RADIO";
        length = snprintf(line, sizeof(line), "\nEWS1,B,%llu,%llu,%lld,%s\n",
                          static_cast<unsigned long long>(ESP.getEfuseMac()),
                          static_cast<unsigned long long>(monitorBoot),
                          static_cast<long long>(seconds), state);
    } else if (rowIndex <= MaxRovers) {
        const auto& rover = roverObservations[rowIndex - 1];
        const auto& stored = records[rowIndex - 1];
        const auto& identityReport = rover.contact < 0 ? stored.report : rover.latest;
        const bool cached = !rover.point && stored.report.fix;
        const unsigned alarm = !stored.report.sequence ? 0 : stored.report.kind == Kind::Sos ? 1 : 2;
        length = snprintf(line, sizeof(line), "EWS1,R,%u,%u,%s,%u,%u,%ld,%u,%ld,%ld,%ld,%lu,%lu,%u\n",
                          rowIndex, identityReport.route, identityReport.name.data(), alarm,
                          unsigned(stored.restored), ageAt(seconds, rover.contact), rover.fixState,
                          ageAt(seconds, rover.fixAt),
                          static_cast<long>(cached ? stored.report.latitude : rover.latitude),
                          static_cast<long>(cached ? stored.report.longitude : rover.longitude),
                          static_cast<unsigned long>(rover.point),
                          static_cast<unsigned long>(rover.segment), unsigned(cached));
    } else if (rowIndex < EndRow) {
        const auto& node = nodeObservations[rowIndex - MaxRovers - 1];
        length = snprintf(line, sizeof(line), "EWS1,N,%u,%ld,%ld\n", rowIndex - MaxRovers,
                          ageAt(seconds, node.heartbeat), ageAt(seconds, node.forwarded));
    } else {
        length = snprintf(line, sizeof(line), "EWS1,E\n");
    }

    if (length <= 0 || length >= int(sizeof(line))) {
        streaming = false;
        return;
    }
    if (Serial.availableForWrite() < length) {
        return;
    }
    if (Serial.write(reinterpret_cast<const uint8_t*>(line), length) != size_t(length)) {
        streaming = false;
        return;
    }
    streaming = rowIndex++ != EndRow;
}
