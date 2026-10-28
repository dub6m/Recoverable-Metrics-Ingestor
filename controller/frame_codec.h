#pragma once

// Byte layouts for the lab's CAN frames. docs/lab.dbc is the reference;
// tests/sil/test_dbc_layout.py checks these functions against it.

#include <array>
#include <cstdint>

namespace lab
{
constexpr uint32_t kLampCommandId = 0x100;
constexpr uint32_t kVehicleStateId = 0x110;
constexpr uint32_t kLampStateId = 0x200;
constexpr uint8_t kProtocolVersion = 1;

constexpr uint8_t kLampCommandLength = 2;
constexpr uint8_t kVehicleStateLength = 3;
constexpr uint8_t kLampStateLength = 2;

constexpr uint16_t kBatteryRawMin = 600;  // 6.00 V
constexpr uint16_t kBatteryRawMax = 1800; // 18.00 V

struct CanFrame
{
    uint32_t id = 0;
    uint8_t length = 0;
    std::array<uint8_t, 8> data{};
};

struct LampCommand
{
    bool lampOn = false;
    uint8_t alive = 0; // 0..15
};

struct VehicleState
{
    bool ignitionOn = false;
    uint16_t batteryRaw = 0; // volts * 100
};

enum class Mode : uint8_t
{
    NoCommand = 0,
    Normal = 1,
    Expired = 2,
};

struct LampState
{
    bool lampOn = false;
    bool fallbackActive = true;
    Mode mode = Mode::NoCommand;
    uint8_t lastAlive = 0;
    bool aliveValid = false;

    bool operator==(const LampState &other) const;
    bool operator!=(const LampState &other) const { return !(*this == other); }
};

enum class DecodeError
{
    None,
    WrongId,
    WrongLength,
    WrongVersion,
    ReservedBitsSet,
    OutOfRange,
};

const char *Describe(DecodeError error);
const char *Describe(Mode mode);

DecodeError DecodeLampCommand(const CanFrame &frame, LampCommand &out);
DecodeError DecodeVehicleState(const CanFrame &frame, VehicleState &out);
DecodeError DecodeLampState(const CanFrame &frame, LampState &out);

CanFrame EncodeLampCommand(const LampCommand &command);
CanFrame EncodeVehicleState(const VehicleState &state);
CanFrame EncodeLampState(const LampState &state);
} // namespace lab
