// T3: pure controller tests with exact, injected time. The table-driven test
// replays tests/expected/lamp_transitions.json, the same table the SIL tests
// replay against the real controller over vcan.

#include "controller_node.h"
#include "frame_codec.h"
#include "lighting_controller.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <string>

using lab::CanFrame;
using lab::ControllerNode;
using lab::LampState;
using lab::LightingController;
using lab::Mode;

namespace
{
CanFrame RawFrame(const std::string &idText, const std::string &hex)
{
    CanFrame frame;
    frame.id = static_cast<uint32_t>(std::stoul(idText, nullptr, 16));
    frame.length = static_cast<uint8_t>(hex.size() / 2);
    for (size_t i = 0; i < frame.length; ++i)
    {
        frame.data[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return frame;
}

Mode ParseMode(const std::string &text)
{
    if (text == "normal")
    {
        return Mode::Normal;
    }
    if (text == "expired")
    {
        return Mode::Expired;
    }
    return Mode::NoCommand;
}

nlohmann::json LoadTable()
{
    std::ifstream in(std::string(LAB_EXPECTED_DIR) + "/lamp_transitions.json");
    return nlohmann::json::parse(in);
}
} // namespace

TEST(ControllerTable, EveryScenarioMatchesExactly)
{
    const auto table = LoadTable();
    ASSERT_GE(table.at("scenarios").size(), 10u);
    for (const auto &scenario : table.at("scenarios"))
    {
        const std::string name = scenario.at("name");
        ControllerNode::Settings settings;
        settings.timeoutMs = table.at("timeout_ms");
        ControllerNode node(settings);
        size_t checks = 0;
        for (const auto &step : scenario.at("steps"))
        {
            const uint64_t at = step.at("at_ms");
            node.poll(at); // time passes up to this step, as the real loop's tick does
            if (step.contains("vehicle"))
            {
                lab::VehicleState vehicle;
                vehicle.ignitionOn = step["vehicle"].at("ignition");
                vehicle.batteryRaw = static_cast<uint16_t>(std::lround(step["vehicle"].at("battery_v").get<double>() * 100));
                node.handleFrame(lab::EncodeVehicleState(vehicle), at);
            }
            else if (step.contains("command"))
            {
                lab::LampCommand command;
                command.lampOn = step["command"].at("lamp");
                command.alive = step["command"].at("alive");
                node.handleFrame(lab::EncodeLampCommand(command), at);
            }
            else if (step.contains("raw"))
            {
                const auto result = node.handleFrame(RawFrame(step["raw"].at("id"), step["raw"].at("data")), at);
                EXPECT_FALSE(result.accepted) << name << " @" << at << ": invalid frame was accepted ("
                                              << step["raw"].value("why", "") << ")";
            }
            else if (step.contains("expect"))
            {
                const auto &expect = step["expect"];
                const LampState state = node.state();
                const std::string where = name + " @" + std::to_string(at) + "ms";
                EXPECT_EQ(state.lampOn, expect.at("lamp_on").get<bool>()) << where;
                EXPECT_EQ(state.fallbackActive, expect.at("fallback").get<bool>()) << where;
                EXPECT_EQ(state.mode, ParseMode(expect.at("mode"))) << where << " mode=" << lab::Describe(state.mode);
                if (expect.contains("alive"))
                {
                    EXPECT_EQ(state.lastAlive, expect["alive"].get<int>()) << where;
                }
                if (expect.contains("alive_valid"))
                {
                    EXPECT_EQ(state.aliveValid, expect["alive_valid"].get<bool>()) << where;
                }
                ++checks;
            }
        }
        EXPECT_GT(checks, 0u) << name << " has no checks";
    }
}

// ------------------------------------------------------------ direct rules

TEST(LightingController, StartsInFallbackWithLampOff)
{
    LightingController controller(300);
    const LampState state = controller.state();
    EXPECT_FALSE(state.lampOn);
    EXPECT_TRUE(state.fallbackActive);
    EXPECT_EQ(state.mode, Mode::NoCommand);
}

TEST(LightingController, ExpiresOneMillisecondAfterTheTimeout)
{
    LightingController controller(300);
    controller.onVehicleState({true, 1250}, 1000);
    controller.onCommand({true, 1}, 1000);
    EXPECT_FALSE(controller.tick(1300));
    EXPECT_TRUE(controller.state().lampOn);
    EXPECT_TRUE(controller.tick(1301));
    EXPECT_FALSE(controller.state().lampOn);
    EXPECT_EQ(controller.state().mode, Mode::Expired);
    EXPECT_FALSE(controller.tick(5000)) << "timeout should fire once";
}

TEST(LightingController, RepeatedAliveIsReportedAndIgnored)
{
    LightingController controller(300);
    EXPECT_EQ(controller.onCommand({true, 4}, 0), LightingController::CommandOutcome::Accepted);
    EXPECT_EQ(controller.onCommand({false, 4}, 10), LightingController::CommandOutcome::RepeatedAlive);
    EXPECT_EQ(controller.onCommand({false, 5}, 20), LightingController::CommandOutcome::Accepted);
}

TEST(LightingController, StaleOnCommandDoesNotReturnAfterExpiry)
{
    LightingController controller(300);
    controller.onVehicleState({true, 1250}, 0);
    controller.onCommand({true, 1}, 0);
    controller.tick(400);
    controller.onVehicleState({true, 1250}, 500); // other traffic doesn't revive the lamp
    controller.tick(600);
    EXPECT_FALSE(controller.state().lampOn);
    EXPECT_TRUE(controller.state().fallbackActive);
}

TEST(ControllerNode, SendsOnChangeAndOnEveryPeriod)
{
    ControllerNode::Settings settings;
    settings.statePeriodMs = 100;
    settings.timeoutMs = 300;
    ControllerNode node(settings);
    ASSERT_TRUE(node.poll(0).has_value()) << "first state is sent at startup";
    EXPECT_FALSE(node.poll(50).has_value());
    EXPECT_TRUE(node.poll(100).has_value()) << "periodic repeat";
    EXPECT_FALSE(node.lastSendWasChange());
    node.handleFrame(lab::EncodeLampCommand({true, 1}), 120);
    EXPECT_TRUE(node.poll(120).has_value()) << "state change is sent at once";
    EXPECT_TRUE(node.lastSendWasChange());
    EXPECT_FALSE(node.poll(150).has_value());
    EXPECT_TRUE(node.poll(421).has_value()) << "timeout is a change";
    EXPECT_TRUE(node.expiredOnLastPoll());
}

// ------------------------------------------------------------ frame codec

TEST(FrameCodec, EncodedFramesDecodeToTheSameValues)
{
    for (int lamp = 0; lamp <= 1; ++lamp)
    {
        for (uint8_t alive = 0; alive < 16; ++alive)
        {
            lab::LampCommand decoded;
            ASSERT_EQ(lab::DecodeLampCommand(lab::EncodeLampCommand({lamp == 1, alive}), decoded),
                      lab::DecodeError::None);
            EXPECT_EQ(decoded.lampOn, lamp == 1);
            EXPECT_EQ(decoded.alive, alive);
        }
    }
    lab::VehicleState vehicle;
    ASSERT_EQ(lab::DecodeVehicleState(lab::EncodeVehicleState({true, 1234}), vehicle), lab::DecodeError::None);
    EXPECT_TRUE(vehicle.ignitionOn);
    EXPECT_EQ(vehicle.batteryRaw, 1234);

    LampState state;
    state.lampOn = true;
    state.fallbackActive = false;
    state.mode = Mode::Normal;
    state.lastAlive = 9;
    state.aliveValid = true;
    LampState decodedState;
    ASSERT_EQ(lab::DecodeLampState(lab::EncodeLampState(state), decodedState), lab::DecodeError::None);
    EXPECT_EQ(decodedState, state);
}

TEST(FrameCodec, RejectsInvalidFramesWithAReason)
{
    lab::LampCommand command;
    EXPECT_EQ(lab::DecodeLampCommand(RawFrame("100", "10"), command), lab::DecodeError::WrongLength);
    EXPECT_EQ(lab::DecodeLampCommand(RawFrame("100", "2001"), command), lab::DecodeError::WrongVersion);
    EXPECT_EQ(lab::DecodeLampCommand(RawFrame("100", "1801"), command), lab::DecodeError::ReservedBitsSet);
    EXPECT_EQ(lab::DecodeLampCommand(RawFrame("101", "1001"), command), lab::DecodeError::WrongId);
    lab::VehicleState vehicle;
    EXPECT_EQ(lab::DecodeVehicleState(RawFrame("110", "105702"), vehicle), lab::DecodeError::OutOfRange); // 5.99 V
    EXPECT_EQ(lab::DecodeVehicleState(RawFrame("110", "100907"), vehicle), lab::DecodeError::OutOfRange); // 18.01 V
    EXPECT_EQ(lab::DecodeVehicleState(RawFrame("110", "105802"), vehicle), lab::DecodeError::None);       // 6.00 V
}
