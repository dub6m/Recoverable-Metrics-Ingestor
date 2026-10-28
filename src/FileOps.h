#pragma once

#include <sys/types.h>

#include <functional>

// The file operations the log uses to write. Tests replace them to inject
// short writes, a full disk or sync errors; production uses PosixFileOps().
struct FileOps
{
    std::function<int(const char *path, int flags, mode_t mode)> open;
    std::function<ssize_t(int fd, const void *buffer, size_t count)> write;
    std::function<int(int fd)> sync; // fdatasync on Linux
    std::function<int(int fd, off_t length)> truncate;
};

FileOps PosixFileOps();
