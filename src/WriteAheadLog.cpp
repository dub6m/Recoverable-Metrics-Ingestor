#include "WriteAheadLog.h"
#include <sstream>

WriteAheadLog::WriteAheadLog(const std::string &filePath) : filePath_(filePath), file_(filePath, std::ios::binary | std::ios::app) {}

void WriteAheadLog::append(
    const std::string &metricName,
    int64_t timestamp,
    double value)
{
    file_ << metricName << " " << timestamp << " " << value << "\n";
}

void WriteAheadLog::flush()
{
    file_.flush();
}

void WriteAheadLog::replay(
    const std::function<void(
        const std::string &,
        int64_t,
        double)> &playback)
{
    flush();
    std::ifstream in(filePath_);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
        {
            continue;
        }

        std::istringstream iss(line);

        std::string metricName;
        int64_t timestamp;
        double value;

        if (iss >> metricName >> timestamp >> value)
        {
            playback(metricName, timestamp, value);
        }
        else
        {
            // TODO: Track or log parse failures for WAL replay.
        }
    }
}

void WriteAheadLog::close()
{
    file_.flush();
    file_.close();
}
