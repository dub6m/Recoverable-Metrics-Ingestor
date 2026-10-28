#include "FileOps.h"

#include <fcntl.h>
#include <unistd.h>

FileOps PosixFileOps()
{
    FileOps ops;
    ops.open = [](const char *path, int flags, mode_t mode) { return ::open(path, flags, mode); };
    ops.write = [](int fd, const void *buffer, size_t count) { return ::write(fd, buffer, count); };
#ifdef __linux__
    ops.sync = [](int fd) { return ::fdatasync(fd); };
#else
    ops.sync = [](int fd) { return ::fsync(fd); };
#endif
    ops.truncate = [](int fd, off_t length) { return ::ftruncate(fd, length); };
    return ops;
}
