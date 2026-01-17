#pragma once

#include <string>
#include <functional>
#include <fstream>

class WriteAheadLog
{
public:
    explicit WriteAheadLog(const std::string &filePath);

    void append(
        const std::string &metricName,
        int64_t timestamp,
        double value);

    void flush();

    void replay(
        const std::function<void(
            const std::string &metricName,
            int64_t timeStamp,
            double value)> &callback);

    void close();

private:
    std::string filePath_;
    std::ofstream file_;
};