#pragma once

#include "TransactionDescriptor.hpp"
#include "Transaction.hpp"
#include "TMVar.hpp"
#include "RetryBackoff.hpp"
#include "EBRManager/guard.hpp"
#include <cstdint>
#include <type_traits>

namespace STM {
namespace Occ {

inline TransactionDescriptor& getLocalDescriptor() {
    static thread_local TransactionDescriptor desc;
    return desc;
}

inline Transaction& getLocalTransaction() {
    static thread_local Transaction tx(&getLocalDescriptor());
    return tx;
}

} // namespace Occ

template<typename T>
using Var = Occ::TMVar<T>;

// The logical transaction keeps its EBR guard across retries, as before.
// Backoff occurs only after commit() has released all acquired write locks.
// Callers may choose None/Yield for controlled comparisons; no progress bound
// or fairness guarantee is implied by the randomized scheduling policy.
template<typename F>
auto atomically(F&& func, Occ::BackoffPolicy policy = Occ::BackoffPolicy::Jitter) {
    ebr::Guard guard(*EBRManager::instance());
    Occ::Transaction& tx = Occ::getLocalTransaction();
    Occ::RetryBackoff backoff(policy, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tx)));
    uint64_t failures = 0;
    while (true) {
        try {
            tx.begin();
            if constexpr (std::is_void_v<std::invoke_result_t<F, Occ::Transaction&>>) {
                func(tx);
                if (tx.commit()) return;
            } else {
                auto result = func(tx);
                if (tx.commit()) return result;
            }
        } catch (const Occ::RetryException&) {
            // Missing retained history and validation failures share the same
            // retry scheduling path; other exceptions propagate through RAII.
        }
        if (failures != UINT64_MAX) ++failures;
        backoff.wait(failures);
    }
}

} // namespace STM
