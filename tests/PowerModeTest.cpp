/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#include "Power.h"
#include "hint-data.h"
#include "utils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

using aidl::android::hardware::power::Mode;
using aidl::android::hardware::power::CpuHeadroomParams;
using aidl::android::hardware::power::CpuHeadroomResult;
using aidl::android::hardware::power::GpuHeadroomParams;
using aidl::android::hardware::power::GpuHeadroomResult;
using aidl::android::hardware::power::impl::Power;

namespace {
std::mutex perfMutex;
std::map<int, int> perfRequests;
int nextHandle = 1;
int acquired = 0;
int released = 0;
int feedbackHeadroom = -1;

size_t activeRequests() {
    std::lock_guard<std::mutex> lock(perfMutex);
    return perfRequests.size();
}

class PowerModeTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::lock_guard<std::mutex> lock(perfMutex);
        perfRequests.clear();
        acquired = released = 0;
    }
};
} // namespace

// Observe requests from the actual generic power_hint backend.
extern "C" int perf_hint_enable(int hint, int) {
    if (hint != AOSP_DELTA + POWER_HINT_SUSTAINED_PERFORMANCE) return -1;
    std::lock_guard<std::mutex> lock(perfMutex);
    const int handle = nextHandle++;
    perfRequests.emplace(handle, hint);
    ++acquired;
    return handle;
}

extern "C" void release_request(int handle) {
    std::lock_guard<std::mutex> lock(perfMutex);
    EXPECT_EQ(perfRequests.erase(handle), 1u);
    ++released;
}

// Observe the feedback result returned to the actual Power implementation.
extern "C" void interaction(int, int, int[]) {}
extern "C" int interaction_with_handle(int, int, int, int[]) { return -1; }
extern "C" int send_perf_get_feedback_extn(int, const char*, int, int[]) {
    return feedbackHeadroom;
}
extern "C" PropVal perf_get_property(const char*, const char* defaultValue) {
    PropVal value{};
    std::strncpy(value.value, defaultValue, sizeof(value.value) - 1);
    return value;
}

TEST_F(PowerModeTest, SustainedEnableAcquiresAndDisableReleases) {
    Power power;
    ASSERT_TRUE(power.setMode(Mode::SUSTAINED_PERFORMANCE, true).isOk());
    ASSERT_EQ(activeRequests(), 1u);
    EXPECT_EQ(perfRequests.begin()->second, AOSP_DELTA + POWER_HINT_SUSTAINED_PERFORMANCE);
    EXPECT_EQ(acquired, 1);
    ASSERT_TRUE(power.setMode(Mode::SUSTAINED_PERFORMANCE, false).isOk());
    EXPECT_EQ(activeRequests(), 0u);
    EXPECT_EQ(released, 1);
}

TEST_F(PowerModeTest, FixedEnableAcquiresAndDisableReleases) {
    Power power;
    ASSERT_TRUE(power.setMode(Mode::FIXED_PERFORMANCE, true).isOk());
    ASSERT_EQ(activeRequests(), 1u);
    EXPECT_EQ(perfRequests.begin()->second, AOSP_DELTA + POWER_HINT_SUSTAINED_PERFORMANCE);
    ASSERT_TRUE(power.setMode(Mode::FIXED_PERFORMANCE, false).isOk());
    EXPECT_EQ(activeRequests(), 0u);
    EXPECT_EQ(acquired, 1);
    EXPECT_EQ(released, 1);
}

TEST_F(PowerModeTest, RepeatedStatesDoNotLeakRequests) {
    for (const auto mode : {Mode::SUSTAINED_PERFORMANCE, Mode::FIXED_PERFORMANCE}) {
        Power power;
        ASSERT_TRUE(power.setMode(mode, false).isOk());
        ASSERT_TRUE(power.setMode(mode, true).isOk());
        ASSERT_TRUE(power.setMode(mode, true).isOk());
        ASSERT_EQ(activeRequests(), 1u);
        ASSERT_TRUE(power.setMode(mode, false).isOk());
        ASSERT_TRUE(power.setMode(mode, false).isOk());
        EXPECT_EQ(activeRequests(), 0u);
    }
    EXPECT_EQ(acquired, 2);
    EXPECT_EQ(released, 2);
}

TEST_F(PowerModeTest, EitherModeRetainsSharedRequestUntilLastDisable) {
    for (const auto first : {Mode::SUSTAINED_PERFORMANCE, Mode::FIXED_PERFORMANCE}) {
        const auto second = first == Mode::SUSTAINED_PERFORMANCE
                ? Mode::FIXED_PERFORMANCE : Mode::SUSTAINED_PERFORMANCE;
        Power power;
        ASSERT_TRUE(power.setMode(first, true).isOk());
        ASSERT_TRUE(power.setMode(second, true).isOk());
        ASSERT_EQ(activeRequests(), 1u);
        ASSERT_TRUE(power.setMode(first, false).isOk());
        ASSERT_TRUE(power.setMode(first, false).isOk());
        ASSERT_EQ(activeRequests(), 1u);
        ASSERT_TRUE(power.setMode(second, false).isOk());
        EXPECT_EQ(activeRequests(), 0u);
    }
    EXPECT_EQ(acquired, 2);
    EXPECT_EQ(released, 2);
}

TEST_F(PowerModeTest, DisablingInactiveModeDoesNotReleaseActiveMode) {
    Power power;
    ASSERT_TRUE(power.setMode(Mode::SUSTAINED_PERFORMANCE, true).isOk());
    ASSERT_TRUE(power.setMode(Mode::FIXED_PERFORMANCE, false).isOk());
    ASSERT_EQ(activeRequests(), 1u);
    EXPECT_EQ(released, 0);
    ASSERT_TRUE(power.setMode(Mode::SUSTAINED_PERFORMANCE, false).isOk());
    EXPECT_EQ(activeRequests(), 0u);
}

TEST_F(PowerModeTest, OtherModeDoesNotChangeSharedRequest) {
    Power power;
    ASSERT_TRUE(power.setMode(Mode::SUSTAINED_PERFORMANCE, true).isOk());
    ASSERT_TRUE(power.setMode(Mode::INTERACTIVE, true).isOk());
    ASSERT_TRUE(power.setMode(Mode::LOW_POWER, true).isOk());
    ASSERT_EQ(activeRequests(), 1u);
    ASSERT_TRUE(power.setMode(Mode::SUSTAINED_PERFORMANCE, false).isOk());
    EXPECT_EQ(activeRequests(), 0u);
}

TEST_F(PowerModeTest, ConcurrentModeChangesBalanceSharedRequest) {
    Power power;
    std::atomic<bool> start = false;
    auto toggle = [&](Mode mode) {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 100; ++i) {
            EXPECT_TRUE(power.setMode(mode, true).isOk());
            EXPECT_TRUE(power.setMode(mode, true).isOk());
            EXPECT_TRUE(power.setMode(mode, false).isOk());
        }
    };
    std::thread sustained(toggle, Mode::SUSTAINED_PERFORMANCE);
    std::thread fixed(toggle, Mode::FIXED_PERFORMANCE);
    start = true;
    sustained.join();
    fixed.join();
    EXPECT_EQ(activeRequests(), 0u);
    EXPECT_GT(acquired, 0);
    EXPECT_EQ(acquired, released);
}

TEST(PowerHeadroomTest, UnavailableCpuFeedbackDoesNotReportZeroHeadroom) {
    Power power;
    for (const int unavailable : {-1, -22, 101, std::numeric_limits<int>::max()}) {
        feedbackHeadroom = unavailable;
        CpuHeadroomResult result;
        ASSERT_TRUE(power.getCpuHeadroom(CpuHeadroomParams{}, &result).isOk());
        EXPECT_TRUE(std::isnan(result.get<CpuHeadroomResult::Tag::globalHeadroom>()))
                << "feedback=" << unavailable;
    }
}

TEST(PowerHeadroomTest, UnavailableGpuFeedbackDoesNotReportZeroHeadroom) {
    Power power;
    for (const int unavailable : {-1, -22, 101, std::numeric_limits<int>::max()}) {
        feedbackHeadroom = unavailable;
        GpuHeadroomResult result;
        ASSERT_TRUE(power.getGpuHeadroom(GpuHeadroomParams{}, &result).isOk());
        EXPECT_TRUE(std::isnan(result.get<GpuHeadroomResult::Tag::globalHeadroom>()))
                << "feedback=" << unavailable;
    }
}

TEST(PowerHeadroomTest, CpuFeedbackPreservesValidZeroAndCapacity) {
    Power power;
    for (const int headroom : {0, 1, 50, 100}) {
        feedbackHeadroom = headroom;
        CpuHeadroomResult result;
        ASSERT_TRUE(power.getCpuHeadroom(CpuHeadroomParams{}, &result).isOk());
        EXPECT_FLOAT_EQ(result.get<CpuHeadroomResult::Tag::globalHeadroom>(), headroom);
    }
}

TEST(PowerHeadroomTest, GpuFeedbackPreservesValidZeroAndCapacity) {
    Power power;
    for (const int headroom : {0, 1, 50, 100}) {
        feedbackHeadroom = headroom;
        GpuHeadroomResult result;
        ASSERT_TRUE(power.getGpuHeadroom(GpuHeadroomParams{}, &result).isOk());
        EXPECT_FLOAT_EQ(result.get<GpuHeadroomResult::Tag::globalHeadroom>(), headroom);
    }
}
