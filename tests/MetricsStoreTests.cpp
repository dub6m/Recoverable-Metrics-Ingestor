#include "MemStore.h"
#include "MetricsStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
std::filesystem::path MakeTempWalPath()
{
    auto base = std::filesystem::temp_directory_path();
    base /= "metrics_store_test_wal.log";
    std::filesystem::remove(base);
    return base;
}
} // namespace

TEST(MetricsStoreOriginal, WalReplayAndQuery)
{
    const auto walPath = MakeTempWalPath();

    {
        MetricsStore store(walPath.string());
        store.startupRecover();
        store.ingest("cpu", 100, 0.5);
        store.ingest("cpu", 200, 0.6);
        store.ingest("mem", 150, 42.0);
        store.shutdown();
    }

    MetricsStore recovered(walPath.string());
    recovered.startupRecover();

    const auto cpuSamples = recovered.query("cpu", 0);
    ASSERT_EQ(cpuSamples.size(), 2u);
    EXPECT_EQ(cpuSamples[0].timestamp, 200);
    EXPECT_EQ(cpuSamples[1].timestamp, 100);

    const auto memSamples = recovered.query("mem", 100);
    ASSERT_EQ(memSamples.size(), 1u);
    EXPECT_EQ(memSamples[0].timestamp, 150);

    recovered.shutdown();
}

TEST(MetricsStoreOriginal, MemStoreEviction)
{
    MemStore store;
    store.append("latency", 10, 1.0);
    store.append("latency", 20, 1.1);
    store.append("latency", 30, 1.2);

    store.evictOlderThan(25);
    const auto samples = store.query("latency", 0);
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].timestamp, 30);
}
