// T4: the collector's message checks, without a broker.

#include "telemetry_message.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using lab::ParsedTelemetry;
using lab::ParseTelemetry;

namespace
{
const std::string kTopic = "vtlab/v1/run1/telemetry/vehicle.battery_v";

nlohmann::json Valid()
{
    return {{"v", 1},
            {"run_id", "run1"},
            {"gateway_epoch", "a1b2c3"},
            {"sequence", 42},
            {"signal", "vehicle.battery_v"},
            {"timestamp", 1793136069828},
            {"value", 12.34}};
}

ParsedTelemetry ParseWith(const std::string &key, const nlohmann::json &value)
{
    nlohmann::json message = Valid();
    message[key] = value;
    return ParseTelemetry(kTopic, message.dump());
}
} // namespace

TEST(TelemetryMessage, AcceptsAValidReading)
{
    const ParsedTelemetry parsed = ParseTelemetry(kTopic, Valid().dump());
    ASSERT_TRUE(parsed.ok) << parsed.reason;
    EXPECT_EQ(parsed.runId, "run1");
    EXPECT_EQ(parsed.reading.id->epoch, "a1b2c3");
    EXPECT_EQ(parsed.reading.id->sequence, 42u);
    EXPECT_EQ(parsed.reading.signal, "vehicle.battery_v");
    EXPECT_EQ(parsed.reading.timestamp, 1793136069828);
    EXPECT_EQ(parsed.reading.value, 12.34);
}

TEST(TelemetryMessage, IntegerValuesAreAccepted)
{
    EXPECT_TRUE(ParseWith("value", 1).ok);
}

TEST(TelemetryMessage, RejectsUnexpectedTopicsWithoutIdentity)
{
    for (const std::string topic : {"vtlab/v1/run1/telemetry", "vtlab/v2/run1/telemetry/x", "other/v1/run1/telemetry/x",
                                    "vtlab/v1/run 1/telemetry/x", "vtlab/v1/run1/telemetry/x/extra"})
    {
        const ParsedTelemetry parsed = ParseTelemetry(topic, Valid().dump());
        EXPECT_FALSE(parsed.ok) << topic;
        EXPECT_FALSE(parsed.identityKnown) << topic;
    }
}

TEST(TelemetryMessage, RejectsPayloadsThatAreNotJsonObjects)
{
    EXPECT_FALSE(ParseTelemetry(kTopic, "not json").ok);
    EXPECT_FALSE(ParseTelemetry(kTopic, "[1,2]").ok);
    EXPECT_FALSE(ParseTelemetry(kTopic, std::string(2000, ' ')).ok);
}

TEST(TelemetryMessage, RejectsBadIdentity)
{
    EXPECT_FALSE(ParseWith("run_id", "other-run").identityKnown);
    EXPECT_FALSE(ParseWith("sequence", -1).identityKnown);
    EXPECT_FALSE(ParseWith("sequence", 1.5).identityKnown);
    EXPECT_FALSE(ParseWith("gateway_epoch", "bad epoch").identityKnown);
}

TEST(TelemetryMessage, RejectsBadFieldsButKeepsIdentityForTheReceipt)
{
    const ParsedTelemetry wrongValue = ParseWith("value", "12.34");
    EXPECT_FALSE(wrongValue.ok);
    EXPECT_TRUE(wrongValue.identityKnown);
    EXPECT_EQ(wrongValue.reason, "value is not a finite number");

    EXPECT_FALSE(ParseWith("v", 2).ok);
    EXPECT_FALSE(ParseWith("signal", "lamp.on").ok) << "signal must match the topic";
    EXPECT_FALSE(ParseWith("timestamp", 1.5).ok);
    EXPECT_FALSE(ParseWith("timestamp", 18446744073709551615ull).ok);
    EXPECT_FALSE(ParseWith("extra", true).ok);

    nlohmann::json missing = Valid();
    missing.erase("timestamp");
    const ParsedTelemetry parsed = ParseTelemetry(kTopic, missing.dump());
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.reason, "unexpected or missing fields");
}

TEST(TelemetryMessage, RejectsOutOfRangeNumbers)
{
    std::string text = Valid().dump();
    text.replace(text.find("12.34"), 5, "1e999");
    EXPECT_FALSE(ParseTelemetry(kTopic, text).ok);
}

TEST(TelemetryMessage, ReceiptsCarryIdentityAndOutcome)
{
    const ParsedTelemetry parsed = ParseTelemetry(kTopic, Valid().dump());
    const auto stored = nlohmann::json::parse(lab::ReceiptPayload(parsed, {IngestStatus::Stored, ""}));
    EXPECT_EQ(stored["status"], "stored");
    EXPECT_EQ(stored["duplicate"], false);
    EXPECT_EQ(stored["sequence"], 42);
    EXPECT_EQ(stored["gateway_epoch"], "a1b2c3");

    const auto duplicate = nlohmann::json::parse(lab::ReceiptPayload(parsed, {IngestStatus::Duplicate, ""}));
    EXPECT_EQ(duplicate["status"], "stored");
    EXPECT_EQ(duplicate["duplicate"], true);

    const auto conflict = nlohmann::json::parse(lab::ReceiptPayload(parsed, {IngestStatus::Conflict, "x"}));
    EXPECT_EQ(conflict["status"], "rejected");

    const auto failed = nlohmann::json::parse(lab::ReceiptPayload(parsed, {IngestStatus::Failed, "disk"}));
    EXPECT_EQ(failed["status"], "failed");
    EXPECT_EQ(lab::ReceiptTopic("run1"), "vtlab/v1/run1/receipts");
}
