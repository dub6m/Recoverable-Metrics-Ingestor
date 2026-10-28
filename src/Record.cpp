#include "Record.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
constexpr size_t kMaxTokenLength = 128;
constexpr size_t kMaxPayloadLength = 1024;

std::array<uint32_t, 256> MakeCrcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
        {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

std::vector<std::string> Split(const std::string &text)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;)
    {
        const size_t space = text.find(' ', start);
        parts.push_back(text.substr(start, space == std::string::npos ? std::string::npos : space - start));
        if (space == std::string::npos)
        {
            return parts;
        }
        start = space + 1;
    }
}

bool ParseUnsigned(const std::string &text, uint64_t &value)
{
    if (text.empty() || text.size() > 20)
    {
        return false;
    }
    for (char c : text)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || *end != '\0')
    {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseSigned(const std::string &text, int64_t &value)
{
    if (text.empty() || text.size() > 20)
    {
        return false;
    }
    const size_t first = text[0] == '-' ? 1 : 0;
    if (first == text.size())
    {
        return false;
    }
    for (size_t i = first; i < text.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
        {
            return false;
        }
    }
    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || *end != '\0')
    {
        return false;
    }
    value = parsed;
    return true;
}
} // namespace

std::string ReadingId::key() const
{
    return runId + ' ' + epoch + ' ' + std::to_string(sequence);
}

bool IsValidToken(const std::string &token)
{
    if (token.empty() || token.size() > kMaxTokenLength || !std::isalnum(static_cast<unsigned char>(token[0])))
    {
        return false;
    }
    for (char c : token)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != ':' && c != '-')
        {
            return false;
        }
    }
    return true;
}

std::string FormatValue(double value)
{
    char buffer[40];
    for (int precision = 15; precision <= 17; ++precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value)
        {
            break;
        }
    }
    return buffer;
}

bool ParseValue(const std::string &text, double &value)
{
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0])))
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (*end != '\0' || !std::isfinite(parsed) || (errno == ERANGE && parsed != 0.0 && std::fabs(parsed) > 1.0))
    {
        return false;
    }
    value = parsed;
    return true;
}

uint32_t Crc32(const std::string &bytes)
{
    static const std::array<uint32_t, 256> table = MakeCrcTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : bytes)
    {
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string EncodeRecordLine(const Reading &reading)
{
    std::string payload;
    if (reading.id)
    {
        payload = reading.id->runId + ' ' + reading.id->epoch + ' ' + std::to_string(reading.id->sequence);
    }
    else
    {
        payload = "- - -";
    }
    payload += ' ' + reading.signal + ' ' + std::to_string(reading.timestamp) + ' ' + FormatValue(reading.value);

    char crc[9];
    std::snprintf(crc, sizeof(crc), "%08x", Crc32(payload));
    return std::to_string(payload.size()) + ' ' + crc + ' ' + payload + '\n';
}

bool DecodeRecordLine(const std::string &line, Reading &out, std::string &why)
{
    const size_t lengthEnd = line.find(' ');
    const size_t crcEnd = lengthEnd == std::string::npos ? std::string::npos : line.find(' ', lengthEnd + 1);
    if (crcEnd == std::string::npos)
    {
        why = "missing length or checksum";
        return false;
    }
    uint64_t length = 0;
    if (!ParseUnsigned(line.substr(0, lengthEnd), length) || length > kMaxPayloadLength)
    {
        why = "invalid length field";
        return false;
    }
    const std::string crcText = line.substr(lengthEnd + 1, crcEnd - lengthEnd - 1);
    if (crcText.size() != 8 || crcText.find_first_not_of("0123456789abcdef") != std::string::npos)
    {
        why = "invalid checksum field";
        return false;
    }
    const std::string payload = line.substr(crcEnd + 1);
    if (payload.size() != length)
    {
        why = "length does not match payload";
        return false;
    }
    if (std::strtoul(crcText.c_str(), nullptr, 16) != Crc32(payload))
    {
        why = "checksum does not match payload";
        return false;
    }

    const auto fields = Split(payload);
    if (fields.size() != 6)
    {
        why = "wrong number of fields";
        return false;
    }
    Reading reading;
    const bool anonymous = fields[0] == "-" && fields[1] == "-" && fields[2] == "-";
    if (!anonymous)
    {
        ReadingId id;
        id.runId = fields[0];
        id.epoch = fields[1];
        if (!ParseUnsigned(fields[2], id.sequence))
        {
            why = "invalid sequence";
            return false;
        }
        reading.id = id;
    }
    reading.signal = fields[3];
    if (!ParseSigned(fields[4], reading.timestamp))
    {
        why = "invalid timestamp";
        return false;
    }
    if (!ParseValue(fields[5], reading.value))
    {
        why = "invalid value";
        return false;
    }
    if (!ValidateReading(reading, why))
    {
        return false;
    }
    out = reading;
    return true;
}

bool ValidateReading(const Reading &reading, std::string &why)
{
    if (!IsValidToken(reading.signal))
    {
        why = "invalid signal name";
        return false;
    }
    if (reading.id && (!IsValidToken(reading.id->runId) || !IsValidToken(reading.id->epoch)))
    {
        why = "invalid run id or epoch";
        return false;
    }
    if (!std::isfinite(reading.value))
    {
        why = "value is not a finite number";
        return false;
    }
    return true;
}
