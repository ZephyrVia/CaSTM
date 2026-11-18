#pragma once

#include <mutex>

namespace STM {
namespace Ww {

// Serializes transaction validation and the logical commit transition.
// Read/write execution and post-commit helping remain concurrent.
class CommitCoordinator final {
public:
    static std::mutex& mutex() noexcept {
        static std::mutex commit_mutex;
        return commit_mutex;
    }
};

} // namespace Ww
} // namespace STM
