#include "WriteAheadLog.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace
{
constexpr char kHeader[] = "METRICSWAL 2\n";
constexpr size_t kHeaderLength = sizeof(kHeader) - 1;

std::string ErrnoText(int error)
{
    return std::strerror(error);
}

bool ReadAll(int fd, std::string &out)
{
    char buffer[65536];
    off_t offset = 0;
    for (;;)
    {
        const ssize_t got = ::pread(fd, buffer, sizeof(buffer), offset);
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (got == 0)
        {
            return true;
        }
        out.append(buffer, static_cast<size_t>(got));
        offset += got;
    }
}

// The original store wrote "name timestamp value" lines.
bool LooksLikeOriginalFormat(const std::string &line)
{
    std::istringstream fields(line);
    std::string name;
    long long timestamp = 0;
    double value = 0.0;
    std::string extra;
    return static_cast<bool>(fields >> name >> timestamp >> value) && !(fields >> extra);
}

// A new file's directory entry is durable only once the directory is synced.
bool SyncParentDirectory(const std::string &path)
{
    std::string parent = std::filesystem::path(path).parent_path().string();
    if (parent.empty())
    {
        parent = ".";
    }
    const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0)
    {
        return false;
    }
    const bool ok = ::fsync(dirFd) == 0;
    ::close(dirFd);
    return ok;
}
} // namespace

WriteAheadLog::WriteAheadLog(std::string filePath, FileOps ops) : filePath_(std::move(filePath)), ops_(std::move(ops))
{
}

WriteAheadLog::~WriteAheadLog()
{
    close();
}

WalResult WriteAheadLog::open(Mode mode, const RecordCallback &onRecord, OpenInfo &info)
{
    if (fd_ >= 0)
    {
        return WalResult::Error("log is already open");
    }
    info = OpenInfo{};
    mode_ = mode;
    failed_ = false;

    bool created = false;
    if (mode == Mode::ReadOnly)
    {
        fd_ = ops_.open(filePath_.c_str(), O_RDONLY | O_CLOEXEC, 0);
    }
    else
    {
        fd_ = ops_.open(filePath_.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        created = fd_ >= 0;
        if (fd_ < 0 && errno == EEXIST)
        {
            fd_ = ops_.open(filePath_.c_str(), O_RDWR | O_APPEND | O_CLOEXEC, 0);
        }
    }
    if (fd_ < 0)
    {
        const int error = errno;
        return WalResult::Error("cannot open log " + filePath_ + ": " + ErrnoText(error));
    }

    if (mode == Mode::ReadWrite && ::flock(fd_, LOCK_EX | LOCK_NB) != 0)
    {
        const int error = errno;
        close();
        return WalResult::Error(error == EWOULDBLOCK ? "log " + filePath_ + " is in use by another process"
                                                     : "cannot lock log " + filePath_ + ": " + ErrnoText(error));
    }

    std::string content;
    if (!ReadAll(fd_, content))
    {
        const int error = errno;
        close();
        return WalResult::Error("cannot read log " + filePath_ + ": " + ErrnoText(error));
    }

    // Header. An empty file, or a header cut short by a crash while the file
    // was being created, holds no records yet.
    if (content.compare(0, kHeaderLength, kHeader) != 0)
    {
        const bool partialHeader =
            content.size() < kHeaderLength && std::string(kHeader).compare(0, content.size(), content) == 0;
        if (!partialHeader)
        {
            const std::string firstLine = content.substr(0, content.find('\n'));
            close();
            if (firstLine.rfind("METRICSWAL ", 0) == 0)
            {
                return WalResult::Error(filePath_ + " uses log format '" + firstLine +
                                        "'; this store reads METRICSWAL 2 only");
            }
            if (LooksLikeOriginalFormat(firstLine))
            {
                return WalResult::Error(filePath_ +
                                        " was written by the original store ('name timestamp value' lines). "
                                        "Old logs are not migrated: move the file aside or use a new log path");
            }
            return WalResult::Error(filePath_ + " is not a metrics log (unrecognised first line)");
        }
        if (mode == Mode::ReadOnly)
        {
            return WalResult::Ok();
        }
        if (!content.empty())
        {
            info.discardedTailBytes = content.size();
            if (ops_.truncate(fd_, 0) != 0)
            {
                const int error = errno;
                close();
                return WalResult::Error("cannot reset incomplete log header: " + ErrnoText(error));
            }
        }
        size_ = 0;
        WalResult written = writeAll(kHeader);
        if (written.ok && ops_.sync(fd_) != 0)
        {
            written = WalResult::Error("cannot sync new log header: " + ErrnoText(errno));
        }
        if (written.ok && created && !SyncParentDirectory(filePath_))
        {
            written = WalResult::Error("cannot sync the log's directory: " + ErrnoText(errno));
        }
        if (!written.ok)
        {
            close();
            return written;
        }
        size_ = kHeaderLength;
        return WalResult::Ok();
    }

    // Records. Every line that ends in a newline must be valid; only bytes
    // after the last newline can be an incomplete record from a crash.
    size_t offset = kHeaderLength;
    size_t line = 1;
    while (offset < content.size())
    {
        const size_t end = content.find('\n', offset);
        if (end == std::string::npos)
        {
            break;
        }
        ++line;
        Reading reading;
        std::string why;
        if (!DecodeRecordLine(content.substr(offset, end - offset), reading, why))
        {
            close();
            return WalResult::Error(filePath_ + ": corrupt record at line " + std::to_string(line) + " (byte " +
                                    std::to_string(offset) + "): " + why +
                                    ". Refusing to start; inspect or restore the log before continuing");
        }
        const WalResult accepted = onRecord(reading, line);
        if (!accepted.ok)
        {
            close();
            return WalResult::Error(filePath_ + ": line " + std::to_string(line) + ": " + accepted.message);
        }
        ++info.records;
        offset = end + 1;
    }

    if (offset < content.size())
    {
        info.discardedTailBytes = content.size() - offset;
        if (mode == Mode::ReadWrite)
        {
            // Cut the incomplete record off before appending, so the next
            // record never lands after it.
            if (ops_.truncate(fd_, static_cast<off_t>(offset)) != 0 || ops_.sync(fd_) != 0)
            {
                const int error = errno;
                close();
                return WalResult::Error("cannot remove incomplete final record: " + ErrnoText(error));
            }
        }
    }
    size_ = offset;
    return WalResult::Ok();
}

WalResult WriteAheadLog::append(const Reading &reading)
{
    if (fd_ < 0 || mode_ != Mode::ReadWrite)
    {
        return WalResult::Error("log is not open for writing");
    }
    if (failed_)
    {
        return WalResult::Error("log stopped accepting writes after an earlier storage error; restart the store");
    }

    const std::string line = EncodeRecordLine(reading);
    const WalResult written = writeAll(line);
    if (!written.ok)
    {
        return written;
    }
    if (ops_.sync(fd_) != 0)
    {
        // After a failed sync the file's contents are unknown. Stop writing;
        // a restart re-reads what actually reached the file.
        return fail("sync failed: " + ErrnoText(errno) + "; the log stops accepting writes until restart");
    }
    size_ += line.size();
    return WalResult::Ok();
}

WalResult WriteAheadLog::writeAll(const std::string &bytes)
{
    size_t written = 0;
    while (written < bytes.size())
    {
        const ssize_t got = ops_.write(fd_, bytes.data() + written, bytes.size() - written);
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got <= 0)
        {
            const std::string why = got < 0 ? ErrnoText(errno) : "no bytes written";
            // Remove whatever part of the record reached the file.
            if (ops_.truncate(fd_, static_cast<off_t>(size_)) != 0)
            {
                return fail("write failed (" + why +
                            ") and the partial record could not be removed; the log stops accepting writes until "
                            "restart");
            }
            return WalResult::Error("write failed: " + why);
        }
        written += static_cast<size_t>(got);
    }
    return WalResult::Ok();
}

WalResult WriteAheadLog::fail(const std::string &message)
{
    failed_ = true;
    return WalResult::Error(message);
}

void WriteAheadLog::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_); // also releases the lock
        fd_ = -1;
    }
}
