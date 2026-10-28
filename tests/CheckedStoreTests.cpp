// T2: checked persistence. Failure paths go through the real WriteAheadLog
// code with injected file-operation errors.

#include "MetricsStore.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
class TempDir
{
public:
    TempDir()
    {
        std::string pattern = (fs::temp_directory_path() / "vtlab-store-XXXXXX").string();
        path_ = ::mkdtemp(pattern.data());
    }
    ~TempDir() { fs::remove_all(path_); }
    std::string file(const std::string &name) const { return (fs::path(path_) / name).string(); }

private:
    std::string path_;
};

std::string ReadFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void WriteFile(const std::string &path, const std::string &content)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

void AppendFile(const std::string &path, const std::string &content)
{
    std::ofstream(path, std::ios::binary | std::ios::app) << content;
}

Reading Make(uint64_t sequence, const std::string &signal, int64_t timestamp, double value)
{
    Reading reading;
    reading.id = ReadingId{"run1", "epoch1", sequence};
    reading.signal = signal;
    reading.timestamp = timestamp;
    reading.value = value;
    return reading;
}

bool SameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

// Injectable faults. Armed after the store opens, so the header write succeeds.
struct Faults
{
    bool armed = false;
    size_t shortWriteBytes = 0; // first write call writes only this many bytes
    bool shortDone = false;
    int writeErrno = 0;         // then (or immediately) fail with this errno
    bool failSync = false;
    bool failTruncate = false;
};

FileOps FaultyOps(std::shared_ptr<Faults> faults)
{
    FileOps ops = PosixFileOps();
    const auto realWrite = ops.write;
    const auto realSync = ops.sync;
    const auto realTruncate = ops.truncate;
    ops.write = [faults, realWrite](int fd, const void *buffer, size_t count) -> ssize_t {
        if (faults->armed)
        {
            if (faults->shortWriteBytes > 0 && !faults->shortDone)
            {
                faults->shortDone = true;
                return realWrite(fd, buffer, std::min(count, faults->shortWriteBytes));
            }
            if (faults->writeErrno != 0)
            {
                errno = faults->writeErrno;
                return -1;
            }
        }
        return realWrite(fd, buffer, count);
    };
    ops.sync = [faults, realSync](int fd) {
        if (faults->armed && faults->failSync)
        {
            errno = EIO;
            return -1;
        }
        return realSync(fd);
    };
    ops.truncate = [faults, realTruncate](int fd, off_t length) {
        if (faults->armed && faults->failTruncate)
        {
            errno = EIO;
            return -1;
        }
        return realTruncate(fd, length);
    };
    return ops;
}

size_t RecordCount(const std::string &walPath)
{
    MetricsStore store(walPath);
    const StoreResult opened = store.startupRecover();
    EXPECT_TRUE(opened.ok) << opened.message;
    return store.readings().size();
}
} // namespace

// ------------------------------------------------------------ exact values

TEST(CheckedStore, RoundTripsNonIntegerReadingsExactlyAfterRestart)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    std::ifstream fixture(std::string(LAB_FIXTURE_DIR) + "/round_trip_values.txt");
    ASSERT_TRUE(fixture.good());

    std::vector<double> expected;
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        std::string line;
        uint64_t sequence = 0;
        while (std::getline(fixture, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            std::istringstream fields(line);
            std::string decimal;
            std::string hex;
            fields >> decimal >> hex;
            const double fromDecimal = std::strtod(decimal.c_str(), nullptr);
            const double exact = std::strtod(hex.c_str(), nullptr);
            ASSERT_TRUE(SameBits(fromDecimal, exact)) << "fixture line is inconsistent: " << line;
            ASSERT_EQ(store.ingest(Make(sequence++, "fixture.value", 1, fromDecimal)).status, IngestStatus::Stored);
            expected.push_back(exact);
        }
        store.shutdown();
    }
    ASSERT_GE(expected.size(), 10u);

    MetricsStore restarted(wal);
    ASSERT_TRUE(restarted.startupRecover().ok);
    ASSERT_EQ(restarted.readings().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_TRUE(SameBits(restarted.readings()[i].value, expected[i]))
            << "value " << i << ": got " << FormatValue(restarted.readings()[i].value);
        EXPECT_EQ(restarted.readings()[i].id->sequence, i);
    }
}

TEST(CheckedStore, RecordLineRoundTripsAllFields)
{
    Reading reading = Make(18446744073709551615ull, "vehicle.battery_v", -9223372036854775807ll, -273.15);
    const std::string line = EncodeRecordLine(reading);
    ASSERT_EQ(line.back(), '\n');
    Reading decoded;
    std::string why;
    ASSERT_TRUE(DecodeRecordLine(line.substr(0, line.size() - 1), decoded, why)) << why;
    EXPECT_EQ(decoded.id->sequence, reading.id->sequence);
    EXPECT_EQ(decoded.timestamp, reading.timestamp);
    EXPECT_TRUE(SameBits(decoded.value, reading.value));
}

// ------------------------------------------------------------ write and sync failures

TEST(CheckedStore, MissingDirectoryAcceptsNothing)
{
    TempDir dir;
    MetricsStore store(dir.file("no-such-dir/store.wal"));
    const StoreResult opened = store.startupRecover();
    EXPECT_FALSE(opened.ok);
    EXPECT_NE(opened.message.find("cannot open log"), std::string::npos) << opened.message;
    const IngestResult result = store.ingest(Make(0, "lamp.on", 1, 1.0));
    EXPECT_EQ(result.status, IngestStatus::Failed);
    EXPECT_TRUE(store.query("lamp.on", 0).empty());
    EXPECT_TRUE(store.readings().empty());
}

TEST(CheckedStore, FullDiskOnWriteAcceptsNothing)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    auto faults = std::make_shared<Faults>();
    MetricsStore store(wal, FaultyOps(faults));
    ASSERT_TRUE(store.startupRecover().ok);
    const auto sizeBefore = fs::file_size(wal);

    faults->armed = true;
    faults->writeErrno = ENOSPC;
    const IngestResult result = store.ingest(Make(0, "lamp.on", 1, 1.0));
    EXPECT_EQ(result.status, IngestStatus::Failed);
    EXPECT_NE(result.message.find("No space left"), std::string::npos) << result.message;
    EXPECT_TRUE(store.query("lamp.on", 0).empty()) << "memory changed although the write failed";
    EXPECT_EQ(fs::file_size(wal), sizeBefore);

    faults->armed = false; // disk has space again; nothing was left half-written
    EXPECT_EQ(store.ingest(Make(1, "lamp.on", 2, 0.0)).status, IngestStatus::Stored);
    store.shutdown();
    EXPECT_EQ(RecordCount(wal), 1u);
}

TEST(CheckedStore, ShortWriteThenErrorLeavesNoPartialRecord)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    auto faults = std::make_shared<Faults>();
    MetricsStore store(wal, FaultyOps(faults));
    ASSERT_TRUE(store.startupRecover().ok);
    ASSERT_EQ(store.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);
    const auto sizeBefore = fs::file_size(wal);

    faults->armed = true;
    faults->shortWriteBytes = 5;
    faults->writeErrno = ENOSPC;
    EXPECT_EQ(store.ingest(Make(1, "lamp.on", 2, 0.0)).status, IngestStatus::Failed);
    EXPECT_EQ(fs::file_size(wal), sizeBefore) << "partial record was left in the log";
    EXPECT_EQ(store.query("lamp.on", 0).size(), 1u);

    faults->armed = false;
    EXPECT_EQ(store.ingest(Make(2, "lamp.on", 3, 1.0)).status, IngestStatus::Stored);
    store.shutdown();
    EXPECT_EQ(RecordCount(wal), 2u);
}

TEST(CheckedStore, SyncErrorAcceptsNothingAndStopsWrites)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    auto faults = std::make_shared<Faults>();
    MetricsStore store(wal, FaultyOps(faults));
    ASSERT_TRUE(store.startupRecover().ok);

    faults->armed = true;
    faults->failSync = true;
    const IngestResult result = store.ingest(Make(0, "lamp.on", 1, 1.0));
    EXPECT_EQ(result.status, IngestStatus::Failed);
    EXPECT_TRUE(store.query("lamp.on", 0).empty());

    faults->armed = false;
    const IngestResult later = store.ingest(Make(1, "lamp.on", 2, 1.0));
    EXPECT_EQ(later.status, IngestStatus::Failed);
    EXPECT_NE(later.message.find("restart"), std::string::npos) << later.message;
    store.shutdown();

    // The record's bytes did reach the file before the sync failed, so after a
    // restart it is there. A retry with the same identity is a duplicate, not a
    // second copy.
    MetricsStore restarted(wal);
    ASSERT_TRUE(restarted.startupRecover().ok);
    EXPECT_EQ(restarted.readings().size(), 1u);
    EXPECT_EQ(restarted.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Duplicate);
    EXPECT_EQ(restarted.readings().size(), 1u);
}

TEST(CheckedStore, FailedRollbackStopsWritesAndRestartCutsThePartialRecord)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    auto faults = std::make_shared<Faults>();
    MetricsStore store(wal, FaultyOps(faults));
    ASSERT_TRUE(store.startupRecover().ok);
    ASSERT_EQ(store.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);

    faults->armed = true;
    faults->shortWriteBytes = 7;
    faults->writeErrno = EIO;
    faults->failTruncate = true;
    EXPECT_EQ(store.ingest(Make(1, "lamp.on", 2, 0.0)).status, IngestStatus::Failed);
    faults->armed = false;
    EXPECT_EQ(store.ingest(Make(2, "lamp.on", 3, 0.0)).status, IngestStatus::Failed);
    store.shutdown();

    MetricsStore restarted(wal);
    const StoreResult opened = restarted.startupRecover();
    ASSERT_TRUE(opened.ok) << opened.message;
    EXPECT_EQ(opened.records, 1u);
    EXPECT_EQ(opened.discardedTailBytes, 7u);
}

// ------------------------------------------------------------ recovery

TEST(CheckedStore, IncompleteFinalRecordIsCutOffBeforeNewAppends)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        ASSERT_EQ(store.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);
        ASSERT_EQ(store.ingest(Make(1, "lamp.on", 2, 0.0)).status, IngestStatus::Stored);
    }
    const auto completeSize = fs::file_size(wal);
    const std::string torn = EncodeRecordLine(Make(2, "lamp.on", 3, 1.0)).substr(0, 12);
    AppendFile(wal, torn);

    {
        MetricsStore store(wal);
        const StoreResult opened = store.startupRecover();
        ASSERT_TRUE(opened.ok) << opened.message;
        EXPECT_EQ(opened.records, 2u);
        EXPECT_EQ(opened.discardedTailBytes, torn.size());
        EXPECT_EQ(fs::file_size(wal), completeSize) << "incomplete record was not cut off";
        ASSERT_EQ(store.ingest(Make(3, "lamp.on", 4, 1.0)).status, IngestStatus::Stored);
    }

    MetricsStore restarted(wal);
    const StoreResult opened = restarted.startupRecover();
    ASSERT_TRUE(opened.ok) << opened.message;
    EXPECT_EQ(opened.records, 3u);
    EXPECT_EQ(opened.discardedTailBytes, 0u);
}

TEST(CheckedStore, TwoCrashRestartCyclesInARowStartCleanly)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    uint64_t sequence = 0;
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        {
            MetricsStore store(wal);
            const StoreResult opened = store.startupRecover();
            ASSERT_TRUE(opened.ok) << "cycle " << cycle << ": " << opened.message;
            ASSERT_EQ(store.ingest(Make(sequence, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);
            ++sequence;
        }
        // Crash mid-write: an incomplete record at the end of the file.
        AppendFile(wal, EncodeRecordLine(Make(1000 + sequence, "lamp.on", 9, 0.0)).substr(0, 20));
    }
    MetricsStore restarted(wal);
    const StoreResult opened = restarted.startupRecover();
    ASSERT_TRUE(opened.ok) << opened.message;
    EXPECT_EQ(opened.records, 2u);
    for (const auto &reading : restarted.readings())
    {
        EXPECT_LT(reading.id->sequence, 1000u) << "an incomplete record came back";
    }
}

TEST(CheckedStore, InteriorCorruptionRefusesStartup)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        for (uint64_t i = 0; i < 3; ++i)
        {
            ASSERT_EQ(store.ingest(Make(i, "lamp.on", static_cast<int64_t>(i), 1.0)).status, IngestStatus::Stored);
        }
    }
    std::string content = ReadFile(wal);
    const size_t secondRecord = content.find('\n', content.find('\n') + 1) + 1; // header, record 1
    const size_t flip = content.find("lamp.on", secondRecord);
    content[flip] = 'L';
    WriteFile(wal, content);

    MetricsStore store(wal);
    const StoreResult opened = store.startupRecover();
    EXPECT_FALSE(opened.ok);
    EXPECT_NE(opened.message.find("corrupt record at line 3"), std::string::npos) << opened.message;
    EXPECT_NE(opened.message.find("checksum"), std::string::npos) << opened.message;
    EXPECT_TRUE(store.readings().empty());
    EXPECT_EQ(store.ingest(Make(9, "lamp.on", 9, 1.0)).status, IngestStatus::Failed);
    EXPECT_EQ(ReadFile(wal), content) << "a refused log must not be changed";

    MetricsStore reader(wal);
    EXPECT_FALSE(reader.openReadOnly().ok);
}

TEST(CheckedStore, CorruptCompleteFinalLineIsNotTreatedAsIncomplete)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        ASSERT_EQ(store.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);
    }
    AppendFile(wal, "garbage that ends with a newline\n");
    MetricsStore store(wal);
    EXPECT_FALSE(store.startupRecover().ok);
}

TEST(CheckedStore, OriginalFormatLogIsRefusedNotMigrated)
{
    TempDir dir;
    const std::string wal = dir.file("old.wal");
    WriteFile(wal, "cpu 100 0.5\ncpu 200 0.6\n");
    MetricsStore store(wal);
    const StoreResult opened = store.startupRecover();
    EXPECT_FALSE(opened.ok);
    EXPECT_NE(opened.message.find("original store"), std::string::npos) << opened.message;
    EXPECT_EQ(ReadFile(wal), "cpu 100 0.5\ncpu 200 0.6\n");
}

TEST(CheckedStore, UnknownLogVersionIsRefused)
{
    TempDir dir;
    const std::string wal = dir.file("future.wal");
    WriteFile(wal, "METRICSWAL 3\n");
    MetricsStore store(wal);
    const StoreResult opened = store.startupRecover();
    EXPECT_FALSE(opened.ok);
    EXPECT_NE(opened.message.find("METRICSWAL 3"), std::string::npos) << opened.message;
}

TEST(CheckedStore, IncompleteHeaderIsTreatedAsAnEmptyLog)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    WriteFile(wal, "METRIC");
    MetricsStore store(wal);
    const StoreResult opened = store.startupRecover();
    ASSERT_TRUE(opened.ok) << opened.message;
    EXPECT_EQ(opened.records, 0u);
    EXPECT_EQ(ReadFile(wal), "METRICSWAL 2\n");
}

TEST(CheckedStore, ReadOnlyOpenLeavesAnIncompleteRecordAlone)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        ASSERT_EQ(store.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);
    }
    AppendFile(wal, "57 0000");
    const std::string before = ReadFile(wal);
    MetricsStore reader(wal);
    const StoreResult opened = reader.openReadOnly();
    ASSERT_TRUE(opened.ok) << opened.message;
    EXPECT_EQ(reader.readings().size(), 1u);
    EXPECT_EQ(ReadFile(wal), before);
    EXPECT_EQ(reader.ingest(Make(1, "lamp.on", 2, 1.0)).status, IngestStatus::Failed);
}

// ------------------------------------------------------------ identity and duplicates

TEST(CheckedStore, DuplicatesAndConflictsAreDetectedAcrossRestarts)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        ASSERT_EQ(store.ingest(Make(7, "vehicle.battery_v", 100, 12.34)).status, IngestStatus::Stored);
    }
    for (int restart = 0; restart < 2; ++restart)
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        EXPECT_EQ(store.ingest(Make(7, "vehicle.battery_v", 100, 12.34)).status, IngestStatus::Duplicate);
        const IngestResult conflict = store.ingest(Make(7, "vehicle.battery_v", 100, 12.35));
        EXPECT_EQ(conflict.status, IngestStatus::Conflict);
        EXPECT_EQ(store.readings().size(), 1u) << "restart repeated records or dedup entries";
    }
    EXPECT_EQ(RecordCount(wal), 1u);
}

TEST(CheckedStore, SecondRecoverCallDoesNotReplayAgain)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    {
        MetricsStore store(wal);
        ASSERT_TRUE(store.startupRecover().ok);
        ASSERT_EQ(store.ingest(Make(0, "lamp.on", 1, 1.0)).status, IngestStatus::Stored);
    }
    MetricsStore store(wal);
    ASSERT_TRUE(store.startupRecover().ok);
    ASSERT_TRUE(store.startupRecover().ok);
    EXPECT_EQ(store.readings().size(), 1u);
    EXPECT_EQ(store.query("lamp.on", 0).size(), 1u);
}

TEST(CheckedStore, SecondWriterIsLockedOut)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    MetricsStore first(wal);
    ASSERT_TRUE(first.startupRecover().ok);
    MetricsStore second(wal);
    const StoreResult opened = second.startupRecover();
    EXPECT_FALSE(opened.ok);
    EXPECT_NE(opened.message.find("in use"), std::string::npos) << opened.message;
}

TEST(CheckedStore, RejectsNonFiniteValuesAndBadNames)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    MetricsStore store(wal);
    ASSERT_TRUE(store.startupRecover().ok);
    EXPECT_EQ(store.ingest(Make(0, "lamp.on", 1, std::nan(""))).status, IngestStatus::Invalid);
    EXPECT_EQ(store.ingest(Make(1, "lamp.on", 1, INFINITY)).status, IngestStatus::Invalid);
    EXPECT_EQ(store.ingest(Make(2, "lamp on", 1, 1.0)).status, IngestStatus::Invalid);
    Reading badRun = Make(3, "lamp.on", 1, 1.0);
    badRun.id->runId = "-";
    EXPECT_EQ(store.ingest(badRun).status, IngestStatus::Invalid);
    store.shutdown();
    EXPECT_EQ(RecordCount(wal), 0u);
}

TEST(CheckedStore, ReadingsWithoutIdentityAreNotDeduplicated)
{
    TempDir dir;
    const std::string wal = dir.file("store.wal");
    MetricsStore store(wal);
    ASSERT_TRUE(store.startupRecover().ok);
    EXPECT_EQ(store.ingest("cpu", 100, 0.5).status, IngestStatus::Stored);
    EXPECT_EQ(store.ingest("cpu", 100, 0.5).status, IngestStatus::Stored);
    EXPECT_EQ(store.query("cpu", 0).size(), 2u);
}
