#pragma once

#include "frame_codec.h"

#include <cstdint>

namespace lab
{
// Pure lamp logic. Time is passed in (milliseconds on a monotonic clock), so
// tests control it exactly. The rules are written out in docs/protocol.md.
class LightingController
{
public:
    enum class CommandOutcome
    {
        Accepted,
        RepeatedAlive, // same alive counter as the last accepted command: ignored
    };

    explicit LightingController(uint64_t timeoutMs);

    CommandOutcome onCommand(const LampCommand &command, uint64_t nowMs);
    void onVehicleState(const VehicleState &state, uint64_t nowMs);

    // Advances time. Returns true if the command timeout fired on this call.
    bool tick(uint64_t nowMs);

    LampState state() const;

private:
    uint64_t timeoutMs_;
    Mode mode_ = Mode::NoCommand;
    bool commandedOn_ = false;
    bool ignitionOn_ = false; // off until the first vehicle-state frame says otherwise
    bool aliveValid_ = false;
    uint8_t lastAlive_ = 0;
    uint64_t lastCommandMs_ = 0;
};
} // namespace lab
