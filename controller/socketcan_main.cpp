// Runs the lighting controller on a Linux SocketCAN interface (real or vcan).
//
//   lighting_controller_can --iface vcan0 [--state-period-ms 100] [--tick-ms 10]
//                           [--timeout-ms 300] [--trace path.jsonl]

#include "controller_node.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace
{
volatile std::sig_atomic_t gStop = 0;

void OnSignal(int)
{
    gStop = 1;
}

uint64_t MonotonicMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

struct Options
{
    std::string iface;
    std::string tracePath;
    uint64_t statePeriodMs = 100;
    uint64_t tickMs = 10;
    uint64_t timeoutMs = 300;
};

bool ParseOptions(int argc, char *argv[], Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (i + 1 >= argc)
        {
            return false;
        }
        const std::string value = argv[++i];
        if (arg == "--iface")
        {
            options.iface = value;
        }
        else if (arg == "--trace")
        {
            options.tracePath = value;
        }
        else if (arg == "--state-period-ms")
        {
            options.statePeriodMs = std::stoull(value);
        }
        else if (arg == "--tick-ms")
        {
            options.tickMs = std::stoull(value);
        }
        else if (arg == "--timeout-ms")
        {
            options.timeoutMs = std::stoull(value);
        }
        else
        {
            return false;
        }
    }
    return !options.iface.empty() && options.tickMs > 0 && options.statePeriodMs > 0 && options.timeoutMs > 0;
}

// One JSON object per line, flushed immediately so a crash keeps what came before.
class Trace
{
public:
    explicit Trace(const std::string &path) : file_(path.empty() ? nullptr : std::fopen(path.c_str(), "a")) {}
    ~Trace()
    {
        if (file_)
        {
            std::fclose(file_);
        }
    }

    void write(uint64_t nowMs, const std::string &event, const std::string &detail)
    {
        if (!file_)
        {
            return;
        }
        std::fprintf(file_, "{\"t_ms\":%llu,\"event\":\"%s\",\"detail\":\"%s\"}\n",
                     static_cast<unsigned long long>(nowMs), event.c_str(), detail.c_str());
        std::fflush(file_);
    }

private:
    std::FILE *file_;
};

std::string StateDetail(const lab::LampState &state)
{
    return std::string("lamp=") + (state.lampOn ? "1" : "0") + " fallback=" + (state.fallbackActive ? "1" : "0") +
           " mode=" + lab::Describe(state.mode) + " alive=" + std::to_string(state.lastAlive);
}

int OpenCanSocket(const std::string &iface)
{
    const int fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0)
    {
        std::perror("socket");
        return -1;
    }

    ifreq request{};
    std::strncpy(request.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &request) < 0)
    {
        std::fprintf(stderr, "interface %s: %s\n", iface.c_str(), std::strerror(errno));
        close(fd);
        return -1;
    }

    // The kernel delivers only command and vehicle-state frames. Our own state
    // frames use a different id, so the controller never reads its own output.
    const can_filter filters[] = {
        {lab::kLampCommandId, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
        {lab::kVehicleStateId, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG},
    };
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters)) < 0)
    {
        std::perror("setsockopt CAN_RAW_FILTER");
        close(fd);
        return -1;
    }

    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = request.ifr_ifindex;
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    {
        std::perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

bool SendFrame(int fd, const lab::CanFrame &frame)
{
    can_frame raw{};
    raw.can_id = frame.id;
    raw.len = frame.length;
    std::memcpy(raw.data, frame.data.data(), frame.length);
    return write(fd, &raw, sizeof(raw)) == static_cast<ssize_t>(sizeof(raw));
}
} // namespace

int main(int argc, char *argv[])
{
    Options options;
    try
    {
        if (!ParseOptions(argc, argv, options))
        {
            std::fprintf(stderr, "usage: %s --iface NAME [--state-period-ms N] [--tick-ms N] [--timeout-ms N] "
                                 "[--trace PATH]\n",
                         argv[0]);
            return 1;
        }
    }
    catch (const std::exception &)
    {
        std::fprintf(stderr, "invalid numeric option\n");
        return 1;
    }

    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    const int fd = OpenCanSocket(options.iface);
    if (fd < 0)
    {
        return 2;
    }

    Trace trace(options.tracePath);
    lab::ControllerNode::Settings settings;
    settings.statePeriodMs = options.statePeriodMs;
    settings.timeoutMs = options.timeoutMs;
    lab::ControllerNode node(settings);
    trace.write(MonotonicMs(), "started", "iface=" + options.iface);

    pollfd waiter{fd, POLLIN, 0};
    while (!gStop)
    {
        // Wake on frames *and* on a fixed tick, so the timeout can fire after
        // commands stop arriving.
        const int ready = poll(&waiter, 1, static_cast<int>(options.tickMs));
        if (ready < 0 && errno != EINTR)
        {
            std::perror("poll");
            break;
        }

        for (;;)
        {
            can_frame raw{};
            const ssize_t got = read(fd, &raw, sizeof(raw));
            if (got < 0)
            {
                break; // EAGAIN: drained
            }
            lab::CanFrame frame;
            frame.id = raw.can_id & CAN_SFF_MASK;
            frame.length = raw.len;
            std::memcpy(frame.data.data(), raw.data, raw.len > 8 ? 8 : raw.len);
            if (got != static_cast<ssize_t>(sizeof(raw)) || (raw.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)))
            {
                trace.write(MonotonicMs(), "rejected", "not a classic data frame");
                continue;
            }
            const auto result = node.handleFrame(frame, MonotonicMs());
            trace.write(MonotonicMs(), result.event, result.detail);
        }

        const uint64_t now = MonotonicMs();
        const auto out = node.poll(now);
        if (node.expiredOnLastPoll())
        {
            trace.write(now, "timeout", "no fresh command within " + std::to_string(options.timeoutMs) + " ms");
        }
        if (out)
        {
            if (!SendFrame(fd, *out))
            {
                trace.write(now, "send_failed", std::strerror(errno));
            }
            else if (node.lastSendWasChange())
            {
                trace.write(now, "state", StateDetail(node.state()));
            }
        }
    }

    trace.write(MonotonicMs(), "stopped", "");
    close(fd);
    return 0;
}
