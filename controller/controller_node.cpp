#include "controller_node.h"

namespace lab
{
ControllerNode::ControllerNode(Settings settings) : settings_(settings), controller_(settings.timeoutMs) {}

ControllerNode::FrameResult ControllerNode::handleFrame(const CanFrame &frame, uint64_t nowMs)
{
    FrameResult result;
    if (frame.id == kLampCommandId)
    {
        LampCommand command;
        const DecodeError error = DecodeLampCommand(frame, command);
        if (error != DecodeError::None)
        {
            result.event = "rejected";
            result.detail = std::string("lamp command: ") + Describe(error);
            return result;
        }
        const std::string what =
            std::string(command.lampOn ? "on" : "off") + " alive=" + std::to_string(command.alive);
        if (controller_.onCommand(command, nowMs) == LightingController::CommandOutcome::RepeatedAlive)
        {
            result.event = "command_repeated";
            result.detail = what;
            return result;
        }
        result.accepted = true;
        result.event = "command";
        result.detail = what;
        return result;
    }
    if (frame.id == kVehicleStateId)
    {
        VehicleState vehicle;
        const DecodeError error = DecodeVehicleState(frame, vehicle);
        if (error != DecodeError::None)
        {
            result.event = "rejected";
            result.detail = std::string("vehicle state: ") + Describe(error);
            return result;
        }
        controller_.onVehicleState(vehicle, nowMs);
        result.accepted = true;
        result.event = "vehicle_state";
        result.detail = std::string("ignition=") + (vehicle.ignitionOn ? "on" : "off");
        return result;
    }
    result.event = "rejected";
    result.detail = "unknown id";
    return result;
}

std::optional<CanFrame> ControllerNode::poll(uint64_t nowMs)
{
    expiredOnLastPoll_ = controller_.tick(nowMs);
    const LampState current = controller_.state();
    const bool changed = !lastSent_ || *lastSent_ != current;
    if (!changed && nowMs < nextSendMs_)
    {
        return std::nullopt;
    }
    lastSendWasChange_ = changed;
    lastSent_ = current;
    nextSendMs_ = nowMs + settings_.statePeriodMs;
    return EncodeLampState(current);
}
} // namespace lab
