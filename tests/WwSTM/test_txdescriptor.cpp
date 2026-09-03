#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "WwSTM/Config.hpp"
#include "WwSTM/TxDescriptor.hpp"

using namespace STM::Ww;

namespace {

} // namespace

TEST(TxDescriptorTest, OverAlignedAllocationUsesCacheLineAlignment) {
#if !STM_WW_TEST_HOOKS
    GTEST_SKIP() << "Descriptor lifetime counters require test hooks.";
#else
    const uint64_t allocated_before = TxDescriptor::debugAllocatedCount();
    const uint64_t reclaimed_before = TxDescriptor::debugReclaimedCount();
    const uint64_t destroyed_before = TxDescriptor::debugDestroyedCount();

    constexpr size_t kDescriptorCount = 4096;
    std::vector<TxDescriptor*> descriptors;
    descriptors.reserve(kDescriptorCount);

    for (size_t i = 0; i < kDescriptorCount; ++i) {
        auto* descriptor = new TxDescriptor(static_cast<uint64_t>(i));
        EXPECT_EQ(reinterpret_cast<uintptr_t>(descriptor) % alignof(TxDescriptor),
                  0u);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(descriptor) % 64u, 0u);
        EXPECT_NE(descriptor->debug_tx_id, 0u);
        descriptors.push_back(descriptor);
    }

    for (auto* descriptor : descriptors) {
        delete descriptor;
    }

    EXPECT_EQ(TxDescriptor::debugAllocatedCount() - allocated_before,
              kDescriptorCount);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before,
              kDescriptorCount);
    EXPECT_EQ(TxDescriptor::debugDestroyedCount() - destroyed_before,
              kDescriptorCount);
#endif
}
