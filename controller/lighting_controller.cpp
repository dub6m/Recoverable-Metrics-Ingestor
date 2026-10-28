#include "lighting_controller.h"

namespace lab
{
LightingController::LightingController(uint64_t timeoutMs) : timeoutMs_(timeoutMs) {}

LightingController::CommandOutcome LightingController::onCommand(const LampCommand &command, uint64_t nowMs)
{
    // An overdue timer expires before the new command is judged.
    tick(nowMs);

    // A repeated counter means a duplicate frame or a stuck sender. It must
    // not keep the lamp alive.
    if (aliveValid_ && command.alive == lastAlive_)
    {
        return CommandOutcome::RepeatedAlive;
    }

    aliveValid_ = true;
    lastAlive_ = command.alive;
    lastCommandMs_ = nowMs;
    commandedOn_ = command.lampOn;
    mode_ = Mode::Normal;
    return CommandOutcome::Accepted;
}

void LightingController::onVehicleState(const VehicleState &state, uint64_t)
{
    ignitionOn_ = state.ignitionOn;
}

bool LightingController::tick(uint64_t nowMs)
{
    // A command is on time while elapsed <= timeout; fallback starts at timeout + 1 ms.
    if (mode_ == Mode::Normal && nowMs - lastCommandMs_ > timeoutMs_)
    {
        mode_ = Mode::Expired;
        commandedOn_ = false; // a stale "on" never comes back without a new command
        return true;
    }
    return false;
}

LampState LightingController::state() const
{
    LampState out;
    out.mode = mode_;
    out.fallbackActive = mode_ != Mode::Normal;
    // Ignition off forces the lamp off; commands still keep the timer alive.
    out.lampOn = mode_ == Mode::Normal && commandedOn_ && ignitionOn_;
    out.lastAlive = lastAlive_;
    out.aliveValid = aliveValid_;
    return out;
}
} // namespace lab
