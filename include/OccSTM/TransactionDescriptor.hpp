#pragma once

#include "ThreadHeap/ThreadHeap.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace STM {
namespace Occ{

struct ReadLogEntry {
    const void* tmvar_addr;
    const void* expected_head;

    using Validator = bool (*)(const void* tmvar_addr, const void* expected_head, uint64_t rv);
    Validator validator;
};


struct WriteLogEntry {
    void* tmvar_addr;   // 锁对象地址（TMVar的指针）
    void* new_node;     // 擦除掉类型信息的新节点

    using Committer = void (*)(void* tmvar, void* node, uint64_t write_ts);
    Committer committer;

    using Deleter = void (*)(void* node);
    Deleter deleter;
};

class TransactionDescriptor {
public:
    enum class State {
        Active,
        Committed,
        Aborted
    };

    static constexpr size_t kDefaultCapacity = 16;

    TransactionDescriptor() {
        read_set_.reserve(kDefaultCapacity * 4);
        write_set_.reserve(kDefaultCapacity);
        lock_set_.reserve(kDefaultCapacity); 
    }

    ~TransactionDescriptor() {
        reset(); 
    }

    void reset() {
        for (void* ptr : allocated_ptrs_) {
            ThreadHeap::deallocate(ptr);
        }
        allocated_ptrs_.clear();

        state_ = State::Active;
        read_version_ = 0;

        read_set_.clear();
        lock_set_.clear(); 
        clearWriteSet_();
    }

    void setReadVersion(uint64_t rv) { read_version_ = rv; }
    uint64_t getReadVersion() const { return read_version_; }

    void addToReadSet(const void* addr, const void* head, ReadLogEntry::Validator v) {
        read_set_.push_back({addr, head, v});
    }

    void addToWriteSet(void* addr, void* new_node, WriteLogEntry::Committer c, WriteLogEntry::Deleter d) {
        write_set_.push_back({addr, new_node, c, d});
    }

    void recordAllocation(void* ptr) {
        allocated_ptrs_.push_back(ptr);
    }

    void commitAllocations() {
        allocated_ptrs_.clear();
    }

    const std::vector<ReadLogEntry>& readSet() const { return read_set_; }
    std::vector<WriteLogEntry>& writeSet() { return write_set_; }
    std::vector<void*>& lockSet() { return lock_set_; }

private:
    void clearWriteSet_() {
        for(WriteLogEntry& entry : write_set_) {
            if (entry.deleter && entry.new_node) {
                entry.deleter(entry.new_node);
            }
        }
        write_set_.clear();
    }

private:
    State state_{State::Active};
    uint64_t read_version_{0};
    std::vector<ReadLogEntry> read_set_;
    std::vector<WriteLogEntry> write_set_;
    std::vector<void*> lock_set_; 

    std::vector<void*> allocated_ptrs_; 
};

} // namespace Occ
} // namespace STM