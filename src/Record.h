#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Who sent a reading: the run, the sender's start-up epoch, and a 64-bit
// sequence number that never wraps within an epoch.
struct ReadingId
{
    std::string runId;
    std::string epoch;
    uint64_t sequence = 0;

    std::string key() const;
};

struct Reading
{
    std::optional<ReadingId> id; // absent for readings ingested without identity (CLI)
    std::string signal;
    int64_t timestamp = 0;
    double value = 0.0;
};

// Names, run ids and epochs: 1-128 characters, starting with a letter or
// digit, then letters, digits or . _ : -
bool IsValidToken(const std::string &token);

// Shortest decimal text that reads back as exactly the same double.
std::string FormatValue(double value);
// Strict parse: the whole string must be a finite number.
bool ParseValue(const std::string &text, double &value);

uint32_t Crc32(const std::string &bytes);

// One log line: "<payload length> <crc32 hex> <payload>\n", where payload is
// "<run|-> <epoch|-> <sequence|-> <signal> <timestamp> <value>".
std::string EncodeRecordLine(const Reading &reading);
// Decodes a line without its trailing newline. On failure, explains why.
bool DecodeRecordLine(const std::string &line, Reading &out, std::string &why);

// Checks a reading before it is written: valid names and a finite value.
bool ValidateReading(const Reading &reading, std::string &why);
