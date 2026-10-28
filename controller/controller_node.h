#pragma once

#include "frame_codec.h"
#include "lighting_controller.h"

#include <cstdint>
#include <optional>
#include <string>

namespace lab
{
// Everything the CAN program does except I/O: decode incoming frames, run the
// controller, and decide when a state frame is due. Pure, so unit tests drive
// it with exact times.
class ControllerNode
{
public:
    struct Settings
    {
        uint64_t statePeriodMs = 100;
        uint64_t timeoutMs = 300;
    };

    struct FrameResult
    {
        bool accepted = false;
        std::string event;  // "command", "command_repeated", "vehicle_state", "rejected"
        std::string detail; // reason for rejection, or a short description
    };

    explicit ControllerNode(Settings settings);

    FrameResult handleFrame(const CanFrame &frame, uint64_t nowMs);

    // Advances time. Returns the state frame to send if the state changed or
    // the send period has elapsed.
    std::optional<CanFrame> poll(uint64_t nowMs);

    LampState state() const { return controller_.state(); }

    // True if the frame returned by the last poll() carried a changed state
    // (as opposed to a periodic repeat).
    bool lastSendWasChange() const { return lastSendWasChange_; }

    // True if the command timeout fired during the last poll().
    bool expiredOnLastPoll() const { return expiredOnLastPoll_; }

private:
    Settings settings_;
    LightingController controller_;
    std::optional<LampState> lastSent_;
    uint64_t nextSendMs_ = 0;
    bool lastSendWasChange_ = false;
    bool expiredOnLastPoll_ = false;
};
} // namespace lab
