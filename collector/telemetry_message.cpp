#include "telemetry_message.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace lab
{
namespace
{
std::vector<std::string> SplitTopic(const std::string &topic)
{
    std::vector<std::string> levels;
    size_t start = 0;
    for (;;)
    {
        const size_t slash = topic.find('/', start);
        levels.push_back(topic.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos)
        {
            return levels;
        }
        start = slash + 1;
    }
}

nlohmann::json ReceiptBase(const ParsedTelemetry &message)
{
    return {{"v", 1},
            {"run_id", message.runId},
            {"gateway_epoch", message.reading.id ? message.reading.id->epoch : ""},
            {"sequence", message.reading.id ? message.reading.id->sequence : 0}};
}
} // namespace

ParsedTelemetry ParseTelemetry(const std::string &topic, const std::string &payload)
{
    ParsedTelemetry out;
    const auto levels = SplitTopic(topic);
    if (levels.size() != 5 || levels[0] != "vtlab" || levels[1] != "v1" || levels[3] != "telemetry" ||
        !IsValidToken(levels[2]) || !IsValidToken(levels[4]))
    {
        out.reason = "unexpected topic";
        return out;
    }
    out.runId = levels[2];
    const std::string &topicSignal = levels[4];

    if (payload.size() > kMaxPayloadBytes)
    {
        out.reason = "payload too large";
        return out;
    }
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded() || !json.is_object())
    {
        out.reason = "payload is not a JSON object";
        return out;
    }

    // Identity first, so even a rejected message can get a receipt.
    const auto run = json.find("run_id");
    const auto epoch = json.find("gateway_epoch");
    const auto sequence = json.find("sequence");
    if (run != json.end() && run->is_string() && run->get<std::string>() == out.runId && epoch != json.end() &&
        epoch->is_string() && IsValidToken(epoch->get<std::string>()) && sequence != json.end() &&
        sequence->is_number_unsigned())
    {
        out.reading.id = ReadingId{out.runId, epoch->get<std::string>(), sequence->get<uint64_t>()};
        out.identityKnown = true;
    }
    else
    {
        out.reason = "missing or invalid identity (run_id, gateway_epoch, sequence)";
        return out;
    }

    static const std::set<std::string> kFields = {"v",      "run_id",    "gateway_epoch", "sequence",
                                                  "signal", "timestamp", "value"};
    std::set<std::string> present;
    for (const auto &item : json.items())
    {
        present.insert(item.key());
    }
    if (present != kFields)
    {
        out.reason = "unexpected or missing fields";
        return out;
    }
    if (!json["v"].is_number_integer() || json["v"].get<int64_t>() != 1)
    {
        out.reason = "unsupported message version";
        return out;
    }
    if (!json["signal"].is_string() || json["signal"].get<std::string>() != topicSignal)
    {
        out.reason = "signal does not match topic";
        return out;
    }
    const auto &timestamp = json["timestamp"];
    if (!timestamp.is_number_integer() ||
        (timestamp.is_number_unsigned() &&
         timestamp.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
    {
        out.reason = "timestamp is not a 64-bit integer";
        return out;
    }
    if (!json["value"].is_number() || !std::isfinite(json["value"].get<double>()))
    {
        out.reason = "value is not a finite number";
        return out;
    }

    out.reading.signal = topicSignal;
    out.reading.timestamp = timestamp.get<int64_t>();
    out.reading.value = json["value"].get<double>();
    std::string why;
    if (!ValidateReading(out.reading, why))
    {
        out.reason = why;
        return out;
    }
    out.ok = true;
    return out;
}

std::string ReceiptTopic(const std::string &runId)
{
    return "vtlab/v1/" + runId + "/receipts";
}

std::string ReceiptPayload(const ParsedTelemetry &message, const IngestResult &result)
{
    nlohmann::json receipt = ReceiptBase(message);
    switch (result.status)
    {
    case IngestStatus::Stored:
    case IngestStatus::Duplicate:
        receipt["status"] = "stored";
        receipt["duplicate"] = result.status == IngestStatus::Duplicate;
        break;
    case IngestStatus::Conflict:
    case IngestStatus::Invalid:
        receipt["status"] = "rejected";
        receipt["reason"] = std::string(Describe(result.status)) + ": " + result.message;
        break;
    case IngestStatus::Failed:
        receipt["status"] = "failed";
        receipt["reason"] = result.message;
        break;
    }
    return receipt.dump();
}

std::string RejectionPayload(const ParsedTelemetry &message)
{
    nlohmann::json receipt = ReceiptBase(message);
    receipt["status"] = "rejected";
    receipt["reason"] = message.reason;
    return receipt.dump();
}
} // namespace lab
