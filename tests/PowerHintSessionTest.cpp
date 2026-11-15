/*
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "PowerHintSession.h"

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <thread>
#include <vector>

using aidl::android::hardware::power::SessionHint;
using aidl::android::hardware::power::SessionMode;
using aidl::android::hardware::power::WorkDuration;

namespace {
std::mutex perfLock;
std::map<int, std::vector<int>> perfRequests;
int nextHandle = 1;
int legacyHintCalls = 0;
bool rejectLegacyHints = false;
bool rejectTaskVotes = false;

size_t activeRequests() {
    std::lock_guard<std::mutex> lock(perfLock);
    return perfRequests.size();
}

std::vector<int> onlyRequest() {
    std::lock_guard<std::mutex> lock(perfLock);
    EXPECT_EQ(perfRequests.size(), 1u);
    return perfRequests.size() == 1u ? perfRequests.begin()->second : std::vector<int>{};
}

class PowerHintSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::lock_guard<std::mutex> lock(perfLock);
        perfRequests.clear();
        legacyHintCalls = 0;
        rejectLegacyHints = false;
        rejectTaskVotes = false;
    }

    const std::vector<WorkDuration> lateWork{{.durationNanos = 20000000}};
};
} // namespace

// Observe the actual session implementation's perf requests without changing clocks.
extern "C" int perf_hint_enable(int hint, int boost) {
    std::lock_guard<std::mutex> lock(perfLock);
    ++legacyHintCalls;
    if (rejectLegacyHints) return -1;
    const int handle = nextHandle++;
    perfRequests[handle] = {hint, boost};
    return handle;
}

extern "C" int interaction_with_handle(int, int duration, int count, int args[]) {
    EXPECT_EQ(duration, 0);
    std::lock_guard<std::mutex> lock(perfLock);
    if (rejectTaskVotes) return -1;
    const int handle = nextHandle++;
    perfRequests[handle] = std::vector<int>(args, args + count);
    return handle;
}

extern "C" void release_request(int handle) {
    std::lock_guard<std::mutex> lock(perfLock);
    EXPECT_EQ(perfRequests.erase(handle), 1u);
}

TEST_F(PowerHintSessionTest, PauseReleasesAllBoosts) {
    PowerHintSessionImpl session(100, 100, {101, 102}, 16666666);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_UP).isOk());
    ASSERT_TRUE(session.reportActualWorkDuration(lateWork).isOk());
    ASSERT_EQ(activeRequests(), 2u);
    ASSERT_TRUE(session.pause().isOk());
    EXPECT_EQ(activeRequests(), 0u);
}

TEST_F(PowerHintSessionTest, CpuLoadResetReleasesFrameBoosts) {
    PowerHintSessionImpl session(100, 100, {101, 102}, 16666666);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_UP).isOk());
    ASSERT_TRUE(session.reportActualWorkDuration(lateWork).isOk());
    ASSERT_EQ(activeRequests(), 2u);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_RESET).isOk());
    EXPECT_EQ(activeRequests(), 0u);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_RESUME).isOk());
    EXPECT_EQ(activeRequests(), 0u);
}

TEST_F(PowerHintSessionTest, PausedReportsCannotAcquireBoosts) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    ASSERT_TRUE(session.pause().isOk());
    session.reportActualWorkDuration(lateWork);
    EXPECT_EQ(activeRequests(), 0u);
    ASSERT_TRUE(session.resume().isOk());
    ASSERT_TRUE(session.reportActualWorkDuration(lateWork).isOk());
    EXPECT_EQ(activeRequests(), 1u);
}

TEST_F(PowerHintSessionTest, ResumeRestoresExplicitLoadHint) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_UP).isOk());
    ASSERT_EQ(activeRequests(), 1u);
    ASSERT_TRUE(session.pause().isOk());
    ASSERT_EQ(activeRequests(), 0u);
    ASSERT_TRUE(session.resume().isOk());
    EXPECT_EQ(activeRequests(), 1u);
}

TEST_F(PowerHintSessionTest, ClosedSessionCannotAcquireBoosts) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    ASSERT_TRUE(session.close().isOk());
    session.reportActualWorkDuration(lateWork);
    EXPECT_EQ(activeRequests(), 0u);
    session.sendHint(SessionHint::CPU_LOAD_UP);
    session.resume();
    session.setThreads({201});
    session.setMode(SessionMode::GRAPHICS_PIPELINE, true);
    EXPECT_EQ(activeRequests(), 0u);
    EXPECT_FALSE(session.setThreads({201}).isOk());
    ASSERT_TRUE(session.close().isOk());
}

TEST_F(PowerHintSessionTest, ReplacingThreadsReleasesOldModesAndBoost) {
    PowerHintSessionImpl session(100, 100, {101, 102, 103}, 16666666);
    ASSERT_TRUE(session.setMode(SessionMode::GRAPHICS_PIPELINE, true).isOk());
    ASSERT_TRUE(session.setMode(SessionMode::POWER_EFFICIENCY, true).isOk());
    ASSERT_TRUE(session.reportActualWorkDuration(lateWork).isOk());
    ASSERT_EQ(activeRequests(), 1u);
    ASSERT_TRUE(session.setThreads({201}).isOk());
    ASSERT_EQ(activeRequests(), 0u);
    ASSERT_TRUE(session.reportActualWorkDuration(lateWork).isOk());
    {
        std::lock_guard<std::mutex> lock(perfLock);
        ASSERT_EQ(perfRequests.size(), 1u);
        const auto& request = perfRequests.begin()->second;
        ASSERT_EQ(request.size(), 2u);
        EXPECT_EQ(request[0], 0x43C04000);
        EXPECT_EQ(request[1] >> 8, 201);
    }
    EXPECT_FALSE(session.setThreads({}).isOk());
}

TEST_F(PowerHintSessionTest, ConcurrentLifecycleAndReportsReleaseBoosts) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    std::thread reporter([&] {
        for (int i = 0; i < 1000; ++i) {
            session.reportActualWorkDuration(lateWork);
        }
    });
    std::thread lifecycle([&] {
        for (int i = 0; i < 1000; ++i) {
            session.pause();
            session.setThreads({101});
            session.resume();
        }
    });
    reporter.join();
    lifecycle.join();
    ASSERT_TRUE(session.close().isOk());
    EXPECT_EQ(activeRequests(), 0u);
}

TEST_F(PowerHintSessionTest, ExplicitCpuHintsUseSignedPerThreadLoadVotes) {
    PowerHintSessionImpl session(100, 100, {101, 102}, 16666666);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_UP).isOk());
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 20,
                                             0x43C04000, (102 << 8) | 20}));
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_DOWN).isOk());
    EXPECT_EQ(activeRequests(), 0u);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_DOWN).isOk());
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 0x80 | 20,
                                             0x43C04000, (102 << 8) | 0x80 | 20}));
    ASSERT_TRUE(session.sendHint(SessionHint::POWER_EFFICIENCY).isOk());
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 0x80 | 40,
                                             0x43C04000, (102 << 8) | 0x80 | 40}));
}

TEST_F(PowerHintSessionTest, ExplicitLoadWorksWithoutLegacy104EHint) {
    rejectLegacyHints = true;
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    ASSERT_TRUE(session.sendHint(SessionHint::CPU_LOAD_UP).isOk());
    EXPECT_EQ(legacyHintCalls, 0);
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 20}));
}

TEST_F(PowerHintSessionTest, ExplicitLoadUsesExistingTaskBoostBounds) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    for (int i = 0; i < 20; ++i) session.sendHint(SessionHint::CPU_LOAD_UP);
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 80}));
    session.sendHint(SessionHint::CPU_LOAD_RESET);
    for (int i = 0; i < 20; ++i) session.sendHint(SessionHint::CPU_LOAD_DOWN);
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 0x80 | 80}));
}

TEST_F(PowerHintSessionTest, ReplacingThreadsRetargetsExplicitLoadVote) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    session.sendHint(SessionHint::CPU_LOAD_UP);
    ASSERT_TRUE(session.setThreads({201, 202}).isOk());
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (201 << 8) | 20,
                                             0x43C04000, (202 << 8) | 20}));
}

TEST_F(PowerHintSessionTest, FailedExplicitVoteRetainsPreviousVote) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    session.sendHint(SessionHint::CPU_LOAD_UP);
    rejectTaskVotes = true;
    session.sendHint(SessionHint::CPU_LOAD_UP);
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 20}));
    rejectTaskVotes = false;
    session.sendHint(SessionHint::CPU_LOAD_UP);
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (101 << 8) | 40}));
}

TEST_F(PowerHintSessionTest, PausedThreadReplacementWaitsForResume) {
    PowerHintSessionImpl session(100, 100, {101}, 16666666);
    session.sendHint(SessionHint::CPU_LOAD_UP);
    session.pause();
    ASSERT_TRUE(session.setThreads({201}).isOk());
    EXPECT_EQ(activeRequests(), 0u);
    session.resume();
    EXPECT_EQ(onlyRequest(), (std::vector<int>{0x43C04000, (201 << 8) | 20}));
}
