#include "frame_codec.h"

namespace lab
{
namespace
{
uint8_t VersionNibble(uint8_t byte)
{
    return static_cast<uint8_t>(byte >> 4);
}
} // namespace

bool LampState::operator==(const LampState &other) const
{
    return lampOn == other.lampOn && fallbackActive == other.fallbackActive && mode == other.mode &&
           lastAlive == other.lastAlive && aliveValid == other.aliveValid;
}

const char *Describe(DecodeError error)
{
    switch (error)
    {
    case DecodeError::None:
        return "ok";
    case DecodeError::WrongId:
        return "wrong id";
    case DecodeError::WrongLength:
        return "wrong length";
    case DecodeError::WrongVersion:
        return "wrong protocol version";
    case DecodeError::ReservedBitsSet:
        return "reserved bits set";
    case DecodeError::OutOfRange:
        return "value out of range";
    }
    return "unknown";
}

const char *Describe(Mode mode)
{
    switch (mode)
    {
    case Mode::NoCommand:
        return "no_command";
    case Mode::Normal:
        return "normal";
    case Mode::Expired:
        return "expired";
    }
    return "unknown";
}

// Byte 0: bit 0 LampRequest, bits 1-3 reserved, bits 4-7 version.
// Byte 1: bits 0-3 AliveCounter, bits 4-7 reserved.
DecodeError DecodeLampCommand(const CanFrame &frame, LampCommand &out)
{
    if (frame.id != kLampCommandId)
    {
        return DecodeError::WrongId;
    }
    if (frame.length != kLampCommandLength)
    {
        return DecodeError::WrongLength;
    }
    if (VersionNibble(frame.data[0]) != kProtocolVersion)
    {
        return DecodeError::WrongVersion;
    }
    if ((frame.data[0] & 0x0E) != 0 || (frame.data[1] & 0xF0) != 0)
    {
        return DecodeError::ReservedBitsSet;
    }
    out.lampOn = (frame.data[0] & 0x01) != 0;
    out.alive = frame.data[1] & 0x0F;
    return DecodeError::None;
}

// Byte 0: bit 0 Ignition, bits 1-3 reserved, bits 4-7 version.
// Bytes 1-2: BatteryVoltage raw, little-endian, 0.01 V per bit.
DecodeError DecodeVehicleState(const CanFrame &frame, VehicleState &out)
{
    if (frame.id != kVehicleStateId)
    {
        return DecodeError::WrongId;
    }
    if (frame.length != kVehicleStateLength)
    {
        return DecodeError::WrongLength;
    }
    if (VersionNibble(frame.data[0]) != kProtocolVersion)
    {
        return DecodeError::WrongVersion;
    }
    if ((frame.data[0] & 0x0E) != 0)
    {
        return DecodeError::ReservedBitsSet;
    }
    const uint16_t raw = static_cast<uint16_t>(frame.data[1] | (frame.data[2] << 8));
    if (raw < kBatteryRawMin || raw > kBatteryRawMax)
    {
        return DecodeError::OutOfRange;
    }
    out.ignitionOn = (frame.data[0] & 0x01) != 0;
    out.batteryRaw = raw;
    return DecodeError::None;
}

// Byte 0: bit 0 LampOn, bit 1 FallbackActive, bits 2-3 Mode, bits 4-7 version.
// Byte 1: bits 0-3 LastAliveCounter, bit 4 AliveValid, bits 5-7 reserved.
DecodeError DecodeLampState(const CanFrame &frame, LampState &out)
{
    if (frame.id != kLampStateId)
    {
        return DecodeError::WrongId;
    }
    if (frame.length != kLampStateLength)
    {
        return DecodeError::WrongLength;
    }
    if (VersionNibble(frame.data[0]) != kProtocolVersion)
    {
        return DecodeError::WrongVersion;
    }
    if ((frame.data[1] & 0xE0) != 0)
    {
        return DecodeError::ReservedBitsSet;
    }
    const uint8_t mode = (frame.data[0] >> 2) & 0x03;
    if (mode > static_cast<uint8_t>(Mode::Expired))
    {
        return DecodeError::OutOfRange;
    }
    out.lampOn = (frame.data[0] & 0x01) != 0;
    out.fallbackActive = (frame.data[0] & 0x02) != 0;
    out.mode = static_cast<Mode>(mode);
    out.lastAlive = frame.data[1] & 0x0F;
    out.aliveValid = (frame.data[1] & 0x10) != 0;
    return DecodeError::None;
}

CanFrame EncodeLampCommand(const LampCommand &command)
{
    CanFrame frame;
    frame.id = kLampCommandId;
    frame.length = kLampCommandLength;
    frame.data[0] = static_cast<uint8_t>((kProtocolVersion << 4) | (command.lampOn ? 0x01 : 0x00));
    frame.data[1] = command.alive & 0x0F;
    return frame;
}

CanFrame EncodeVehicleState(const VehicleState &state)
{
    CanFrame frame;
    frame.id = kVehicleStateId;
    frame.length = kVehicleStateLength;
    frame.data[0] = static_cast<uint8_t>((kProtocolVersion << 4) | (state.ignitionOn ? 0x01 : 0x00));
    frame.data[1] = static_cast<uint8_t>(state.batteryRaw & 0xFF);
    frame.data[2] = static_cast<uint8_t>(state.batteryRaw >> 8);
    return frame;
}

CanFrame EncodeLampState(const LampState &state)
{
    CanFrame frame;
    frame.id = kLampStateId;
    frame.length = kLampStateLength;
    frame.data[0] = static_cast<uint8_t>((kProtocolVersion << 4) | (static_cast<uint8_t>(state.mode) << 2) |
                                         (state.fallbackActive ? 0x02 : 0x00) | (state.lampOn ? 0x01 : 0x00));
    frame.data[1] = static_cast<uint8_t>((state.aliveValid ? 0x10 : 0x00) | (state.lastAlive & 0x0F));
    return frame;
}
} // namespace lab
