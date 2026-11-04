#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Diagnostics.hpp"
#include "EBRManager/guard.hpp"
#include "OccSTM/Transaction.hpp"
#include "OccSTM/RetryBackoff.hpp"
#include "WwSTM/TxContext.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
using stm_bench::AbortCause;
using stm_bench::LogicalOutcome;
using stm_bench::PhaseStats;

constexpr int kDefaultOpsPerTransaction = 8;
constexpr int kMaxOpsPerTransaction = 64;
constexpr int kDefaultLowContentionKeys = 65536;
constexpr int kDefaultHighContentionKeys = 16;
constexpr uint64_t kDefaultSeed = 0xCA57BEEFULL;
constexpr uint64_t kLatencySampleMask = 255;

struct Config {
    std::string backend = "mutex";
    std::string contention = "low";
    int requested_threads = 1;
    int threads = 1;
    int read_ratio = 90;
    int key_count = kDefaultLowContentionKeys;
    int ops_per_tx = kDefaultOpsPerTransaction;
    int warmup_ms = 1000;
    int measure_ms = 5000;
    int run = 1;
    uint64_t seed = kDefaultSeed;
    uint64_t max_attempts = 1000000;
    std::string diagnostics_json;
    std::string backoff = "none";
    STM::Occ::BackoffPolicy backoff_policy = STM::Occ::BackoffPolicy::None;
    bool csv = false;
    bool key_count_overridden = false;
};

struct Operation {
    uint32_t key = 0;
    bool write = false;
};

struct OperationBatch {
    std::array<Operation, kMaxOpsPerTransaction> operations{};
    int count = 0;
    uint64_t writes = 0;
};

struct alignas(64) WorkerStats {
    PhaseStats warmup;
    PhaseStats measured;
    uint64_t read_checksum = 0;
    std::string failure_reason = "none";
    std::string failure_phase = "none";
    std::string error;
    std::vector<uint64_t> latency_ns;

    WorkerStats() { latency_ns.reserve(4096); }
};

class Barrier {
public:
    explicit Barrier(int participants)
        : participants_(participants) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const int generation = generation_;
        if (++arrived_ == participants_) {
            arrived_ = 0;
            ++generation_;
            condition_.notify_all();
            return;
        }

        condition_.wait(lock, [this, generation] {
            return generation_ != generation;
        });
    }

private:
    const int participants_;
    int arrived_ = 0;
    int generation_ = 0;
    std::mutex mutex_;
    std::condition_variable condition_;
};

struct RunControl {
    explicit RunControl(int threads)
        : ready_barrier(threads + 1)
        , measure_barrier(threads + 1) {}

    std::atomic<bool> launched{false};
    std::atomic<int> phase{0};
    std::atomic<bool> measure_go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> invalid{false};
    Barrier ready_barrier;
    Barrier measure_barrier;
};

uint64_t splitmix64(uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

OperationBatch make_batch(const Config& config,
                          int worker_id,
                          uint64_t logical_id) {
    OperationBatch batch;
    batch.count = config.ops_per_tx;

    uint64_t state = config.seed;
    state ^= static_cast<uint64_t>(config.run + 1) * 0xD6E8FEB86659FD93ULL;
    state ^= static_cast<uint64_t>(worker_id + 1) * 0xA0761D6478BD642FULL;
    state ^= (logical_id + 1) * 0xE7037ED1A0B428DBULL;

    for (int i = 0; i < batch.count; ++i) {
        const uint64_t key_draw = splitmix64(state);
        const uint64_t mode_draw = splitmix64(state);
        batch.operations[i].key = static_cast<uint32_t>(key_draw %
                                                         static_cast<uint64_t>(config.key_count));
        batch.operations[i].write = (mode_draw % 100ULL) >=
                                    static_cast<uint64_t>(config.read_ratio);
        if (batch.operations[i].write) {
            ++batch.writes;
        }
    }
    return batch;
}

void consume_read(uint64_t& sink, int64_t value, uint32_t key) noexcept {
    sink ^= static_cast<uint64_t>(value) +
            0x9E3779B97F4A7C15ULL +
            (static_cast<uint64_t>(key) << 32);
    sink = (sink << 7) | (sink >> 57);
}

class MutexBackend {
public:
    struct ThreadContext {
        uint64_t read_sink = 0;
    };

    explicit MutexBackend(int key_count)
        : values_(static_cast<size_t>(key_count), 0) {}

    LogicalOutcome run_logical(ThreadContext& context,
                               const OperationBatch& batch,
                               uint64_t, STM::Occ::RetryBackoff&) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < batch.count; ++i) {
            const Operation& operation = batch.operations[i];
            const int64_t value = values_[operation.key];
            consume_read(context.read_sink, value, operation.key);
            if (operation.write) {
                values_[operation.key] = value + 1;
            }
        }
        return LogicalOutcome{true, true, 1, 0, batch.writes};
    }

    int64_t checksum() const {
        int64_t result = 0;
        for (int64_t value : values_) {
            result += value;
        }
        return result;
    }

private:
    std::vector<int64_t> values_;
    mutable std::mutex mutex_;
};

class SharedMutexBackend {
public:
    struct ThreadContext {
        uint64_t read_sink = 0;
    };

    explicit SharedMutexBackend(int key_count)
        : values_(static_cast<size_t>(key_count), 0) {}

    LogicalOutcome run_logical(ThreadContext& context,
                               const OperationBatch& batch,
                               uint64_t, STM::Occ::RetryBackoff&) {
        if (batch.writes == 0) {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            for (int i = 0; i < batch.count; ++i) {
                const Operation& operation = batch.operations[i];
                consume_read(context.read_sink,
                             values_[operation.key],
                             operation.key);
            }
        } else {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            for (int i = 0; i < batch.count; ++i) {
                const Operation& operation = batch.operations[i];
                const int64_t value = values_[operation.key];
                consume_read(context.read_sink, value, operation.key);
                if (operation.write) {
                    values_[operation.key] = value + 1;
                }
            }
        }
        return LogicalOutcome{true, true, 1, 0, batch.writes};
    }

    int64_t checksum() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        int64_t result = 0;
        for (int64_t value : values_) {
            result += value;
        }
        return result;
    }

private:
    std::vector<int64_t> values_;
    mutable std::shared_mutex mutex_;
};

class OccBackend {
public:
    using Var = STM::Occ::TMVar<int64_t>;

    struct ThreadContext {
        STM::Occ::TransactionDescriptor descriptor;
        STM::Occ::Transaction transaction;
        uint64_t read_sink = 0;

        ThreadContext()
            : descriptor()
            , transaction(&descriptor) {}
    };

    explicit OccBackend(int key_count) {
        variables_.reserve(static_cast<size_t>(key_count));
        for (int i = 0; i < key_count; ++i) {
            variables_.push_back(std::make_unique<Var>(0));
        }
    }

    LogicalOutcome run_logical(ThreadContext& context,
                               const OperationBatch& batch,
                               uint64_t max_attempts, STM::Occ::RetryBackoff& backoff) {
        ebr::Guard guard(*EBRManager::instance());
        auto outcome = stm_bench::runAttempts(max_attempts, [&] {
            context.transaction.begin();
            try {
                for (int i = 0; i < batch.count; ++i) {
                    const auto& operation = batch.operations[i];
                    const int64_t value = context.transaction.load(*variables_[operation.key]);
                    consume_read(context.read_sink, value, operation.key);
                    if (operation.write)
                        context.transaction.store(*variables_[operation.key], value + 1);
                }
            } catch (const STM::Occ::RetryException&) {
                return AbortCause::SnapshotUnavailable;
            }
            if (context.transaction.commit()) return AbortCause::None;
            switch (context.transaction.lastAbortReason()) {
            case STM::Occ::AbortReason::ReadLocked: return AbortCause::ReadLocked;
            case STM::Occ::AbortReason::ReadVersionChanged: return AbortCause::ReadChanged;
            default: throw std::logic_error("OccSTM commit failed without a reason");
            }
        }, [&](uint64_t failures) noexcept { backoff.wait(failures); });
        if (outcome.committed) outcome.writes = batch.writes;
        return outcome;
    }

    int64_t checksum() const {
        int64_t result = 0;
        for (const auto& variable : variables_) {
            result += variable->loadHead()->payload;
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<Var>> variables_;
};

class WwBackend {
public:
    using Var = STM::Ww::TMVar<int64_t>;

    struct ThreadContext {
        STM::Ww::TxContext transaction;
        uint64_t read_sink = 0;
    };

    explicit WwBackend(int key_count) {
        variables_.reserve(static_cast<size_t>(key_count));
        for (int i = 0; i < key_count; ++i) {
            variables_.push_back(std::make_unique<Var>(0));
        }
    }

    LogicalOutcome run_logical(ThreadContext& context,
                               const OperationBatch& batch,
                               uint64_t max_attempts, STM::Occ::RetryBackoff& backoff) {
        auto outcome = stm_bench::runAttempts(max_attempts, [&] {
            context.transaction.begin();
            for (int i = 0; i < batch.count; ++i) {
                const auto& operation = batch.operations[i];
                const int64_t value = context.transaction.read(variables_[operation.key].get());
                consume_read(context.read_sink, value, operation.key);
                if (!context.transaction.isActive()) return AbortCause::WwInactive;
                if (operation.write) {
                    context.transaction.write(variables_[operation.key].get(), value + 1);
                    if (!context.transaction.isActive()) return AbortCause::WwInactive;
                }
            }
            return context.transaction.commit() ? AbortCause::None : AbortCause::WwCommit;
        });
        if (outcome.committed) outcome.writes = batch.writes;
        return outcome;
    }

    int64_t checksum() {
        int64_t result = 0;
        for (const auto& variable : variables_) {
            result += variable->readSnapshot(nullptr).value;
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<Var>> variables_;
};

struct BenchmarkResult {
    std::string backend;
    std::string backoff = "none";
    int requested_threads = 0;
    int threads = 0;
    int read_ratio = 0;
    std::string contention;
    int run = 0;
    double throughput = 0.0;
    double abort_rate = 0.0;
    double p50_us = 0.0;
    double p99_us = 0.0;
    uint64_t attempts = 0;
    uint64_t committed = 0;
    uint64_t read_checksum = 0;
    int64_t final_checksum = 0;
    uint64_t expected_checksum = 0;
    double elapsed_s = 0.0;
    bool valid = false;
    PhaseStats warmup;
    PhaseStats measured;
    std::string failure_reason = "none";
    std::string failure_phase = "none";
    std::string error;
    int failure_worker = -1;
    std::vector<WorkerStats> workers;
};

template<typename Backend>
BenchmarkResult run_benchmark(const Config& config) {
    Backend backend(config.key_count);
    RunControl control(config.threads);
    std::vector<WorkerStats> worker_stats(static_cast<size_t>(config.threads));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(config.threads));

    try {
        for (int worker_id = 0; worker_id < config.threads; ++worker_id) {
            workers.emplace_back([&, worker_id] {
                // Allow thread creation failures to cancel startup before barriers.
                while (!control.launched.load(std::memory_order_acquire)) {
                    if (control.stop.load(std::memory_order_acquire)) return;
                    std::this_thread::yield();
                }
                using ThreadContext = typename Backend::ThreadContext;
                std::unique_ptr<ThreadContext> context;
                WorkerStats& stats = worker_stats[static_cast<size_t>(worker_id)];
                uint64_t logical_id = 0;
                STM::Occ::RetryBackoff backoff(config.backoff_policy,
                    config.seed ^ (uint64_t(worker_id + 1) * 0xD6E8FEB86659FD93ULL) ^ uint64_t(config.run));
                const char* phase = "warmup";
                auto fail = [&](const char* reason, const std::string& error) {
                    stats.failure_reason = reason;
                    stats.failure_phase = phase;
                    stats.error = error;
                    control.invalid.store(true, std::memory_order_release);
                    control.stop.store(true, std::memory_order_release);
                };
                auto run_phase = [&](bool measured) {
                    while (!control.stop.load(std::memory_order_acquire) &&
                           (measured || control.phase.load(std::memory_order_acquire) == 0)) {
                        const auto batch = make_batch(config, worker_id, logical_id++);
                        const auto start = measured ? Clock::now() : Clock::time_point{};
                        const auto outcome = backend.run_logical(*context, batch, config.max_attempts, backoff);
                        const auto end = measured ? Clock::now() : Clock::time_point{};
                        auto& counts = measured ? stats.measured : stats.warmup;
                        // Record BEFORE the failure branch, including terminal attempts.
                        counts.record(outcome);
                        if (!outcome.valid || !outcome.committed) {
                            fail(stm_bench::failureName(outcome.failure), outcome.error);
                            break;
                        }
                        if (measured && (counts.committed & kLatencySampleMask) == 0)
                            stats.latency_ns.push_back(static_cast<uint64_t>(
                                std::chrono::duration_cast<Nanoseconds>(end - start).count()));
                    }
                };
                auto catch_failure = [&] {
                    try { throw; }
                    catch (const std::bad_alloc& error) { fail("allocation_failure", error.what()); }
                    catch (const std::exception& error) { fail("exception", error.what()); }
                    catch (...) { fail("unknown_exception", "non-standard exception"); }
                };
                control.ready_barrier.wait();
                try {
                    context = std::make_unique<ThreadContext>();
                    run_phase(false);
                } catch (...) { catch_failure(); }

                control.measure_barrier.wait();
                while (!control.measure_go.load(std::memory_order_acquire) &&
                       !control.stop.load(std::memory_order_acquire))
                    std::this_thread::yield();
                phase = "measure";
                if (context) {
                    try { run_phase(true); }
                    catch (...) { catch_failure(); }
                    stats.read_checksum = context->read_sink;
                }
            });
        }
    } catch (...) {
        control.stop.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
        throw;
    }
    control.launched.store(true, std::memory_order_release);

    control.ready_barrier.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(config.warmup_ms));

    control.phase.store(1, std::memory_order_release);
    control.measure_barrier.wait();
    const auto measure_begin = Clock::now();
    control.measure_go.store(true, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::milliseconds(config.measure_ms));
    control.stop.store(true, std::memory_order_release);

    for (auto& worker : workers) {
        worker.join();
    }
    const auto measure_end = Clock::now();

    std::vector<uint64_t> all_latencies;
    PhaseStats warmup;
    PhaseStats measured;
    uint64_t read_checksum = 0;
    for (WorkerStats& stats : worker_stats) {
        warmup.merge(stats.warmup);
        measured.merge(stats.measured);
        const size_t worker_id = static_cast<size_t>(&stats - worker_stats.data());
        read_checksum ^= stats.read_checksum +
                         0x9E3779B97F4A7C15ULL +
                         (static_cast<uint64_t>(worker_id) << 6);
        all_latencies.insert(all_latencies.end(),
                             stats.latency_ns.begin(),
                             stats.latency_ns.end());
    }

    std::sort(all_latencies.begin(), all_latencies.end());
    auto percentile_us = [&all_latencies](double percentile) {
        if (all_latencies.empty()) return 0.0;
        const double scaled = percentile * static_cast<double>(all_latencies.size());
        size_t index = static_cast<size_t>(scaled);
        if (index == 0) index = 1;
        --index;
        if (index >= all_latencies.size()) index = all_latencies.size() - 1;
        return static_cast<double>(all_latencies[index]) / 1000.0;
    };

    const double elapsed_s = std::chrono::duration<double>(measure_end - measure_begin).count();
    const int64_t final_checksum = backend.checksum();

    BenchmarkResult result;
    result.backend = config.backend;
    result.backoff = config.backoff;
    result.requested_threads = config.requested_threads;
    result.threads = config.threads;
    result.read_ratio = config.read_ratio;
    result.contention = config.contention;
    result.run = config.run;
    result.throughput = elapsed_s > 0.0
        ? static_cast<double>(measured.committed) / elapsed_s
        : 0.0;
    result.abort_rate = measured.attempts > 0
        ? static_cast<double>(measured.aborts) / static_cast<double>(measured.attempts)
        : 0.0;
    result.p50_us = percentile_us(0.50);
    result.p99_us = percentile_us(0.99);
    result.attempts = measured.attempts;
    result.committed = measured.committed;
    result.read_checksum = read_checksum;
    result.final_checksum = final_checksum;
    result.expected_checksum = warmup.writes + measured.writes;
    result.elapsed_s = elapsed_s;
    result.valid = !control.invalid.load(std::memory_order_acquire) &&
                   final_checksum == static_cast<int64_t>(result.expected_checksum);
    result.warmup = warmup;
    result.measured = measured;
    for (size_t i = 0; i < worker_stats.size(); ++i) {
        if (worker_stats[i].failure_reason != "none") {
            result.failure_reason = worker_stats[i].failure_reason;
            result.failure_phase = worker_stats[i].failure_phase;
            result.failure_worker = static_cast<int>(i);
            result.error = worker_stats[i].error;
            break; // Representative failure; JSON retains every worker's failure.
        }
    }
    if (result.failure_reason == "none" && !result.valid) {
        result.failure_reason = "checksum_mismatch";
        result.failure_phase = "validation";
    }
    result.workers = std::move(worker_stats);
    return result;
}

void print_csv_header() {
    std::cout << "backend,requested_threads,threads,read_ratio,contention,run,"
                 "throughput_tps,abort_rate,p50_us,p99_us,attempts,committed,"
                 "read_checksum,final_checksum,expected_checksum,elapsed_s,valid,"
                 "aborts,failed_logical,retries_p50_upper,retries_p99_upper,max_retries,"
                 "warmup_attempts,warmup_aborts,warmup_committed,warmup_failed_logical,"
                 "failure_reason,failure_phase,failure_worker,error,backoff";
    for (size_t i = 1; i < stm_bench::kAbortNames.size(); ++i)
        std::cout << ",abort_" << stm_bench::kAbortNames[i];
    std::cout << '\n';
}

std::string csv_quote(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) { if (ch == '"') out += '"'; out += ch; }
    return out + '"';
}

void print_csv_row(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(6)
              << result.backend << ','
              << result.requested_threads << ','
              << result.threads << ','
              << result.read_ratio << ','
              << result.contention << ','
              << result.run << ','
              << result.throughput << ','
              << result.abort_rate << ','
              << result.p50_us << ','
              << result.p99_us << ','
              << result.attempts << ','
              << result.committed << ','
              << result.read_checksum << ','
              << result.final_checksum << ','
              << result.expected_checksum << ','
              << result.elapsed_s << ','
              << (result.valid ? 1 : 0) << ','
              << result.measured.aborts << ',' << result.measured.failed << ','
              << result.measured.retryPercentileUpper(50) << ','
              << result.measured.retryPercentileUpper(99) << ',' << result.measured.max_retries << ','
              << result.warmup.attempts << ',' << result.warmup.aborts << ','
              << result.warmup.committed << ',' << result.warmup.failed << ','
              << result.failure_reason << ',' << result.failure_phase << ','
              << result.failure_worker << ',' << csv_quote(result.error) << ',' << result.backoff;
    for (size_t i = 1; i < stm_bench::kAbortNames.size(); ++i)
        std::cout << ',' << result.measured.causes[i];
    std::cout << '\n';
}

void print_human(const BenchmarkResult& result) {
    std::cout << "backend=" << result.backend
              << " backoff=" << result.backoff
              << " threads=" << result.threads
              << " read_ratio=" << result.read_ratio
              << " contention=" << result.contention
              << " run=" << result.run << '\n'
              << std::fixed << std::setprecision(3)
              << "throughput_tps=" << result.throughput
              << " abort_rate=" << result.abort_rate
              << " p50_us=" << result.p50_us
              << " p99_us=" << result.p99_us << '\n'
              << "attempts=" << result.attempts
              << " committed=" << result.committed
              << " read_checksum=" << result.read_checksum
              << " checksum=" << result.final_checksum
              << "/" << result.expected_checksum
              << " elapsed_s=" << result.elapsed_s
              << " valid=" << (result.valid ? "true" : "false") << '\n'
              << "failure_reason=" << result.failure_reason
              << " failure_phase=" << result.failure_phase
              << " max_retries=" << result.measured.max_retries
              << " error=" << result.error << '\n';
}

std::string json_quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch);
            else out << ch;
        }
    }
    out << '"';
    return out.str();
}

void write_phase(std::ostream& out, const PhaseStats& stats) {
    out << "{\"committed\":" << stats.committed << ",\"failed\":" << stats.failed
        << ",\"attempts\":" << stats.attempts << ",\"aborts\":" << stats.aborts
        << ",\"writes\":" << stats.writes << ",\"max_retries\":" << stats.max_retries
        << ",\"retries_p50_upper\":" << stats.retryPercentileUpper(50)
        << ",\"retries_p99_upper\":" << stats.retryPercentileUpper(99)
        << ",\"abort_causes\":{";
    for (size_t i = 1; i < stm_bench::kAbortNames.size(); ++i) {
        if (i > 1) out << ',';
        out << json_quote(stm_bench::kAbortNames[i]) << ':' << stats.causes[i];
    }
    out << "},\"retry_histogram\":[";
    for (size_t i = 0; i < stats.retry_histogram.size(); ++i) {
        if (i) out << ',';
        out << stats.retry_histogram[i];
    }
    out << "]}";
}

void write_diagnostics(const Config& config, const BenchmarkResult& result) {
    if (config.diagnostics_json.empty()) return;
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(config.diagnostics_json);
    out << "{\"schema_version\":1,\"backend\":" << json_quote(config.backend)
        << ",\"backoff\":" << json_quote(config.backoff)
        << ",\"threads\":" << config.threads << ",\"read_ratio\":" << config.read_ratio
        << ",\"contention\":" << json_quote(config.contention)
        << ",\"key_count\":" << config.key_count << ",\"ops_per_tx\":" << config.ops_per_tx
        << ",\"warmup_ms\":" << config.warmup_ms << ",\"measure_ms\":" << config.measure_ms
        << ",\"max_attempts\":" << config.max_attempts << ",\"seed\":" << config.seed
        << ",\"run\":" << config.run << ",\"valid\":" << (result.valid ? "true" : "false")
        << ",\"failure_reason\":" << json_quote(result.failure_reason)
        << ",\"failure_phase\":" << json_quote(result.failure_phase)
        << ",\"failure_worker\":" << result.failure_worker
        << ",\"error\":" << json_quote(result.error)
        << ",\"warmup\":";
    write_phase(out, result.warmup);
    out << ",\"measure\":";
    write_phase(out, result.measured);
    out << ",\"workers\":[";
    for (size_t i = 0; i < result.workers.size(); ++i) {
        if (i) out << ',';
        const auto& worker = result.workers[i];
        out << "{\"worker\":" << i << ",\"failure_reason\":" << json_quote(worker.failure_reason)
            << ",\"failure_phase\":" << json_quote(worker.failure_phase)
            << ",\"error\":" << json_quote(worker.error) << ",\"warmup\":";
        write_phase(out, worker.warmup);
        out << ",\"measure\":";
        write_phase(out, worker.measured);
        out << '}';
    }
    out << "]}\n";
    out.close();
}

[[noreturn]] void fail_usage(const std::string& message) {
    std::cerr << "error: " << message << "\n\n";
    std::cerr << "usage: stm_benchmark --backend <mutex|shared_mutex|occ|ww> "
                 "--threads N --read-ratio P --contention <low|high> [options]\n"
                 "  --key-count N       override contention key count\n"
                 "  --ops-per-tx N      operations per logical transaction (1..64)\n"
                 "  --warmup-ms N       warmup duration (default 1000)\n"
                 "  --measure-ms N      measurement duration (default 5000)\n"
                 "  --run N             repetition number\n"
                 "  --seed N            deterministic workload seed\n"
                 "  --max-attempts N    retry cap per logical transaction\n"
                 "  --backoff POLICY    OccSTM retry policy: none (default), yield, jitter\n"
                 "  --diagnostics-json P write per-worker counters and retry histograms\n"
                 "  --csv               print one machine-readable CSV row\n";
    std::exit(2);
}

std::string next_value(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        fail_usage(std::string("missing value for ") + argv[index]);
    }
    return argv[++index];
}

int parse_int(const std::string& value, const std::string& option) {
    try {
        size_t consumed = 0;
        const int result = std::stoi(value, &consumed, 10);
        if (consumed != value.size()) throw std::invalid_argument("trailing");
        return result;
    } catch (...) {
        fail_usage("invalid integer for " + option + ": " + value);
    }
}

uint64_t parse_u64(const std::string& value, const std::string& option) {
    try {
        size_t consumed = 0;
        if (value.empty() || value.front() == '-') throw std::invalid_argument("negative");
        const uint64_t result = std::stoull(value, &consumed, 0);
        if (consumed != value.size()) throw std::invalid_argument("trailing");
        return result;
    } catch (...) {
        fail_usage("invalid unsigned integer for " + option + ": " + value);
    }
}

Config parse_config(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            fail_usage("help requested");
        } else if (option == "--backend") {
            config.backend = next_value(i, argc, argv);
        } else if (option == "--threads") {
            config.requested_threads = parse_int(next_value(i, argc, argv), option);
        } else if (option == "--read-ratio") {
            config.read_ratio = parse_int(next_value(i, argc, argv), option);
        } else if (option == "--contention") {
            config.contention = next_value(i, argc, argv);
        } else if (option == "--key-count") {
            config.key_count = parse_int(next_value(i, argc, argv), option);
            config.key_count_overridden = true;
        } else if (option == "--ops-per-tx") {
            config.ops_per_tx = parse_int(next_value(i, argc, argv), option);
        } else if (option == "--warmup-ms") {
            config.warmup_ms = parse_int(next_value(i, argc, argv), option);
        } else if (option == "--measure-ms") {
            config.measure_ms = parse_int(next_value(i, argc, argv), option);
        } else if (option == "--run") {
            config.run = parse_int(next_value(i, argc, argv), option);
        } else if (option == "--seed") {
            config.seed = parse_u64(next_value(i, argc, argv), option);
        } else if (option == "--max-attempts") {
            config.max_attempts = parse_u64(next_value(i, argc, argv), option);
        } else if (option == "--backoff") {
            config.backoff = next_value(i, argc, argv);
        } else if (option == "--diagnostics-json") {
            config.diagnostics_json = next_value(i, argc, argv);
        } else if (option == "--csv") {
            config.csv = true;
        } else {
            fail_usage("unknown option: " + option);
        }
    }

    if (config.backend == "OccSTM" || config.backend == "occstm") {
        config.backend = "occ";
    } else if (config.backend == "WwSTM" || config.backend == "wwstm") {
        config.backend = "ww";
    }
    if (config.backend != "mutex" && config.backend != "shared_mutex" &&
        config.backend != "occ" && config.backend != "ww") {
        fail_usage("unsupported backend: " + config.backend);
    }
    if (config.backoff == "yield") config.backoff_policy = STM::Occ::BackoffPolicy::Yield;
    else if (config.backoff == "jitter") config.backoff_policy = STM::Occ::BackoffPolicy::Jitter;
    else if (config.backoff != "none") fail_usage("backoff must be none, yield or jitter");
    if (config.backend != "occ" && config.backoff != "none")
        fail_usage("backoff currently applies only to OccSTM");
    if (config.contention != "low" && config.contention != "high") {
        fail_usage("contention must be low or high");
    }
    if (!config.key_count_overridden) {
        config.key_count = config.contention == "low"
            ? kDefaultLowContentionKeys
            : kDefaultHighContentionKeys;
    }
    if (config.requested_threads <= 0) fail_usage("threads must be positive");
    if (config.read_ratio < 0 || config.read_ratio > 100) {
        fail_usage("read-ratio must be between 0 and 100");
    }
    if (config.key_count <= 0) fail_usage("key-count must be positive");
    if (config.ops_per_tx <= 0 || config.ops_per_tx > kMaxOpsPerTransaction) {
        fail_usage("ops-per-tx must be between 1 and 64");
    }
    if (config.warmup_ms < 0 || config.measure_ms <= 0) {
        fail_usage("warmup-ms must be non-negative and measure-ms must be positive");
    }
    if (config.run <= 0) fail_usage("run must be positive");
    if (config.max_attempts == 0) fail_usage("max-attempts must be positive");

    const unsigned hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    config.threads = std::min(config.requested_threads,
                              static_cast<int>(hardware_threads));
    return config;
}

BenchmarkResult dispatch(const Config& config) {
    if (config.backend == "mutex") return run_benchmark<MutexBackend>(config);
    if (config.backend == "shared_mutex") return run_benchmark<SharedMutexBackend>(config);
    if (config.backend == "occ") return run_benchmark<OccBackend>(config);
    return run_benchmark<WwBackend>(config);
}

} // namespace

int main(int argc, char** argv) {
    const Config config = parse_config(argc, argv);
    BenchmarkResult result;
    result.backend = config.backend;
    result.backoff = config.backoff;
    result.requested_threads = config.requested_threads;
    result.threads = config.threads;
    result.read_ratio = config.read_ratio;
    result.contention = config.contention;
    result.run = config.run;
    result.failure_phase = "startup_or_aggregation";
    try { result = dispatch(config); }
    catch (const std::bad_alloc& error) {
        result.failure_reason = "allocation_failure";
        result.error = error.what();
    } catch (const std::exception& error) {
        result.failure_reason = "exception";
        result.error = error.what();
    } catch (...) {
        result.failure_reason = "unknown_exception";
        result.error = "non-standard exception";
    }
    try { write_diagnostics(config, result); }
    catch (const std::exception& error) {
        result.valid = false;
        result.failure_reason = "diagnostics_io";
        result.failure_phase = "report";
        result.error = error.what();
    }
    if (config.csv) {
        print_csv_header();
        print_csv_row(result);
    } else {
        print_human(result);
    }
    return result.valid ? 0 : 1;
}
