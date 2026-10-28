#pragma once

#include "MetricsStore.h"

#include <cstddef>
#include <string>

namespace lab
{
// Telemetry arrives on vtlab/v1/<run_id>/telemetry/<signal> as a JSON object
// with exactly these fields: v (=1), run_id, gateway_epoch, sequence, signal,
// timestamp (integer), value (finite number). See docs/protocol.md.
constexpr const char *kTelemetryFilter = "vtlab/v1/+/telemetry/#";
constexpr size_t kMaxPayloadBytes = 1024;

struct ParsedTelemetry
{
    bool ok = false;
    bool identityKnown = false; // run, epoch and sequence readable: a receipt can be addressed
    std::string runId;
    Reading reading;
    std::string reason; // why it was rejected
};

ParsedTelemetry ParseTelemetry(const std::string &topic, const std::string &payload);

std::string ReceiptTopic(const std::string &runId);

// Receipt for a message that reached the store (stored, duplicate, conflict or failed).
std::string ReceiptPayload(const ParsedTelemetry &message, const IngestResult &result);

// Receipt for a message rejected before storage but with a readable identity.
std::string RejectionPayload(const ParsedTelemetry &message);
} // namespace lab
