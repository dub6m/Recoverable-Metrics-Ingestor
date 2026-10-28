// Encodes and decodes lab frames with the C++ codec, so tests can compare it
// with docs/lab.dbc (tests/sil/test_dbc_layout.py).
//
//   frame_tool encode-command <lamp 0|1> <alive 0-15>
//   frame_tool encode-vehicle <ignition 0|1> <battery raw>
//   frame_tool encode-state <lamp> <fallback> <mode 0-2> <alive> <alive valid>
//   frame_tool decode <id hex>#<data hex>
//
// Frames are printed in candump style: 100#1001

#include "frame_codec.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
std::string Format(const lab::CanFrame &frame)
{
    char buffer[40];
    int used = std::snprintf(buffer, sizeof(buffer), "%03X#", frame.id);
    for (uint8_t i = 0; i < frame.length; ++i)
    {
        used += std::snprintf(buffer + used, sizeof(buffer) - static_cast<size_t>(used), "%02X", frame.data[i]);
    }
    return buffer;
}

bool Parse(const std::string &text, lab::CanFrame &frame)
{
    const size_t hash = text.find('#');
    if (hash == std::string::npos || (text.size() - hash - 1) % 2 != 0 || (text.size() - hash - 1) / 2 > 8)
    {
        return false;
    }
    frame.id = static_cast<uint32_t>(std::stoul(text.substr(0, hash), nullptr, 16));
    frame.length = static_cast<uint8_t>((text.size() - hash - 1) / 2);
    for (uint8_t i = 0; i < frame.length; ++i)
    {
        frame.data[i] = static_cast<uint8_t>(std::stoul(text.substr(hash + 1 + i * 2, 2), nullptr, 16));
    }
    return true;
}

int Arg(char *argv[], int index)
{
    return std::atoi(argv[index]);
}
} // namespace

int main(int argc, char *argv[])
{
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "encode-command" && argc == 4)
    {
        std::printf("%s\n", Format(lab::EncodeLampCommand({Arg(argv, 2) != 0, static_cast<uint8_t>(Arg(argv, 3))})).c_str());
        return 0;
    }
    if (command == "encode-vehicle" && argc == 4)
    {
        std::printf("%s\n", Format(lab::EncodeVehicleState({Arg(argv, 2) != 0, static_cast<uint16_t>(Arg(argv, 3))})).c_str());
        return 0;
    }
    if (command == "encode-state" && argc == 7)
    {
        lab::LampState state;
        state.lampOn = Arg(argv, 2) != 0;
        state.fallbackActive = Arg(argv, 3) != 0;
        state.mode = static_cast<lab::Mode>(Arg(argv, 4));
        state.lastAlive = static_cast<uint8_t>(Arg(argv, 5));
        state.aliveValid = Arg(argv, 6) != 0;
        std::printf("%s\n", Format(lab::EncodeLampState(state)).c_str());
        return 0;
    }
    if (command == "decode" && argc == 3)
    {
        lab::CanFrame frame;
        if (!Parse(argv[2], frame))
        {
            std::fprintf(stderr, "bad frame text\n");
            return 1;
        }
        lab::DecodeError error = lab::DecodeError::WrongId;
        if (frame.id == lab::kLampCommandId)
        {
            lab::LampCommand out;
            error = lab::DecodeLampCommand(frame, out);
            if (error == lab::DecodeError::None)
            {
                std::printf("{\"LampRequest\":%d,\"AliveCounter\":%d}\n", out.lampOn, out.alive);
            }
        }
        else if (frame.id == lab::kVehicleStateId)
        {
            lab::VehicleState out;
            error = lab::DecodeVehicleState(frame, out);
            if (error == lab::DecodeError::None)
            {
                std::printf("{\"Ignition\":%d,\"BatteryRaw\":%d}\n", out.ignitionOn, out.batteryRaw);
            }
        }
        else if (frame.id == lab::kLampStateId)
        {
            lab::LampState out;
            error = lab::DecodeLampState(frame, out);
            if (error == lab::DecodeError::None)
            {
                std::printf("{\"LampOn\":%d,\"FallbackActive\":%d,\"Mode\":%d,\"LastAliveCounter\":%d,\"AliveValid\":%d}\n",
                            out.lampOn, out.fallbackActive, static_cast<int>(out.mode), out.lastAlive, out.aliveValid);
            }
        }
        if (error != lab::DecodeError::None)
        {
            std::printf("{\"error\":\"%s\"}\n", lab::Describe(error));
        }
        return 0;
    }
    std::fprintf(stderr, "usage: see the comment at the top of frame_tool.cpp\n");
    return 1;
}
