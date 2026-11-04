// Validation-only driver. Payload lifetime counters do not modify STM or EBR.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "EBRManager/guard.hpp"
#include "OccSTM/Transaction.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using STM::Occ::Transaction;
using STM::Occ::TransactionDescriptor;

struct Options {
    int phase_seconds = 600, threads = 20, sample_ms = 1000;
    int rate = 1000, hold_ms = 5000, recovery_ms = 5000;
    int batch_ms = 5000, batch_transactions = 2000, settle_ms = 2000;
    uint64_t rss_limit_kib = 1024 * 1024, vms_limit_kib = 8ULL * 1024 * 1024;
    std::string samples, result;
};

struct Counters {
    std::atomic<uint64_t> created{0}, destroyed{0}, writes{0}, transactions{0};
    std::atomic<uint64_t> retired{0}, reclaimed{0}, teardown_destroyed{0}, unpublished{0};
    std::atomic<uint64_t> errors{0}, batches{0}, threads_started{0};
    std::atomic<uint64_t> slow_cycles{0}, protected_checks{0}, release_checks{0};
    std::atomic<bool> teardown{false}, stop{false};
};

struct Lifetime {
    Counters& counters;
    bool published = false;
    const bool initial;
    // Separate witness: retaining it does not retain the payload or version node.
    const std::shared_ptr<std::atomic<bool>> destroyed_flag;
    Lifetime(Counters& counters, bool initial, bool watched)
        : counters(counters), initial(initial),
          destroyed_flag(watched ? std::make_shared<std::atomic<bool>>(false) : nullptr) {
        counters.created.fetch_add(1, std::memory_order_relaxed);
    }
    ~Lifetime() {
        if (destroyed_flag) destroyed_flag->store(true, std::memory_order_release);
        if (!initial && !published) ++counters.unpublished;
        else if (counters.teardown.load(std::memory_order_relaxed)) ++counters.teardown_destroyed;
        else ++counters.reclaimed;
        ++counters.destroyed;
    }
};

struct Value {
    uint64_t value;
    uint64_t complement;
    std::shared_ptr<Lifetime> lifetime;
};
using Var = STM::Occ::TMVar<Value>;
using Variables = std::vector<std::unique_ptr<Var>>;

struct Memory {
    uint64_t rss = 0, hwm = 0, vms = 0, anon = 0, threads = 0;
};
Memory memory() {
    Memory m;
    std::ifstream stream("/proc/self/status");
    if (!stream) throw std::runtime_error("cannot read /proc/self/status");
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream in(line);
        std::string key;
        uint64_t value;
        if (!(in >> key >> value)) continue;
        if (key == "VmRSS:") m.rss = value;
        else if (key == "VmHWM:") m.hwm = value;
        else if (key == "VmSize:") m.vms = value;
        else if (key == "RssAnon:") m.anon = value;
        else if (key == "Threads:") m.threads = value;
    }
    if (!m.rss || !m.vms) throw std::runtime_error("missing Linux memory counters");
    return m;
}

std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else if (c < 32) out << '?';
        else out << c;
    }
    out << '"';
    return out.str();
}

class Recorder {
public:
    Recorder(const Options& options) : options_(options), stream_(options.samples) {
        if (!stream_) throw std::runtime_error("cannot open samples output");
        stream_ << "elapsed_s,phase,event,rss_kib,hwm_kib,vms_kib,anon_kib,os_threads,"
                   "created,destroyed,live,transactions,writes,retired,reclaimed,pending,"
                   "teardown_destroyed,unpublished,errors,batches,threads_started,"
                   "slow_cycles,protected_checks,release_checks,quiescent,checksum,"
                   "expected_checksum,retained,check_ok,recovery_ms\n";
    }
    void sample(const std::string& phase, const std::string& event, Counters& c,
                bool quiescent = false, uint64_t checksum = 0, uint64_t retained = 0,
                bool check_ok = true, double recovery_ms = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto m = memory();
        if (m.rss > options_.rss_limit_kib || m.vms > options_.vms_limit_kib) {
            ++c.errors;
            c.stop.store(true);
        }
        const uint64_t destroyed = c.destroyed.load(), created = c.created.load();
        const uint64_t reclaimed = c.reclaimed.load(), retired = c.retired.load();
        stream_ << std::fixed << std::setprecision(6)
                << std::chrono::duration<double>(Clock::now() - start_).count() << ','
                << phase << ',' << event << ',' << m.rss << ',' << m.hwm << ',' << m.vms
                << ',' << m.anon << ',' << m.threads << ',' << created << ',' << destroyed
                << ',' << (int64_t(created) - int64_t(destroyed)) << ',' << c.transactions
                << ',' << c.writes << ',' << retired << ',' << reclaimed
                << ',' << (int64_t(retired) - int64_t(reclaimed))
                << ',' << c.teardown_destroyed << ',' << c.unpublished << ',' << c.errors
                << ',' << c.batches << ',' << c.threads_started << ',' << c.slow_cycles
                << ',' << c.protected_checks << ',' << c.release_checks << ',' << quiescent << ',';
        if (quiescent) stream_ << checksum;
        stream_ << ',' << c.writes << ',';
        if (quiescent) stream_ << retained;
        stream_ << ',' << check_ok << ',';
        if (recovery_ms >= 0) stream_ << recovery_ms;
        stream_ << '\n';
        stream_.flush();
        if (!stream_) throw std::runtime_error("samples write failed");
    }
private:
    const Options& options_;
    std::ofstream stream_;
    const Clock::time_point start_ = Clock::now();
    std::mutex mutex_;
};

void drain() {
    for (int n = 0; n < 16; ++n) {
        ebr::Guard guard(*EBRManager::instance());
    }
}

Value makeValue(Counters& c, uint64_t value, bool initial, bool watched) {
    return {value, ~value, std::make_shared<Lifetime>(c, initial, watched)};
}

// One writer per key makes retirement accounting exact: each successful store
// past MAX_HISTORY detaches exactly one version. This is not a contention TPS test.
void worker(Var& var, int index, Counters& c, const Options& o,
            Clock::time_point deadline, bool paced, int transaction_limit = -1) {
    ++c.threads_started;
    try {
        TransactionDescriptor descriptor;
        Transaction transaction(&descriptor);
        auto tick = Clock::now();
        for (int n = 0; !c.stop.load(std::memory_order_relaxed)
             && Clock::now() < deadline && (transaction_limit < 0 || n < transaction_limit); ++n) {
            {
                ebr::Guard guard(*EBRManager::instance());
                transaction.begin();
                const auto previous = transaction.load(var);
                if (previous.complement != ~previous.value) throw std::runtime_error("payload corruption");
                if ((n & 1) == 0) {
                    auto next = makeValue(c, previous.value + 1, false, index == 0);
                    transaction.store(var, next);
                    if (!transaction.commit()) throw std::runtime_error("partitioned write aborted");
                    // Keep this local copy until classification; it never spans a sleep.
                    next.lifetime->published = true;
                    ++c.writes;
                    if (next.value > Var::MAX_HISTORY) ++c.retired;
                } else if (!transaction.commit()) {
                    throw std::runtime_error("partitioned read aborted");
                }
                ++c.transactions;
            }
            if (paced) {
                tick += std::chrono::microseconds(1000000 / o.rate);
                if (tick < Clock::now()) tick = Clock::now();
                std::this_thread::sleep_until(tick);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "worker " << index << ": " << error.what() << '\n';
        ++c.errors;
        c.stop.store(true);
    }
}

struct Checkpoint {
    uint64_t checksum = 0, retained = 0;
    bool ok = true;
};

Checkpoint checkpoint(Variables& vars, Counters& c) {
    drain();
    Checkpoint check;
    {
        ebr::Guard guard(*EBRManager::instance());
        for (const auto& var : vars) {
            const auto* node = var->loadHead();
            check.checksum += node->payload.value;
            if (node->payload.complement != ~node->payload.value) check.ok = false;
            check.retained += std::min<uint64_t>(node->payload.value + 1, Var::MAX_HISTORY + 1);
        }
    }
    drain();
    check.ok = check.ok && check.checksum == c.writes.load()
        && c.reclaimed.load() == c.retired.load()
        && c.created.load() - c.destroyed.load() == check.retained;
    if (!check.ok) ++c.errors;
    return check;
}

void waitUntil(Clock::time_point deadline, Counters& c) {
    while (!c.stop.load() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::min(50ms,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()) + 1ms));
}

void observeUntil(Clock::time_point deadline, const std::string& phase,
                  Counters& c, Recorder& recorder, const Options& o) {
    while (!c.stop.load() && Clock::now() < deadline) {
        recorder.sample(phase, "sample", c);
        waitUntil(std::min(deadline, Clock::now() + std::chrono::milliseconds(o.sample_ms)), c);
    }
}

void slowReader(Variables& vars, Counters& c, Recorder& recorder,
                const Options& o, Clock::time_point deadline) {
    ++c.threads_started;
    try {
        const auto hold = std::chrono::milliseconds(o.hold_ms);
        const auto recovery = std::chrono::milliseconds(o.recovery_ms);
        while (!c.stop.load() && Clock::now() + hold + recovery < deadline) {
            std::shared_ptr<std::atomic<bool>> destroyed;
            {
                ebr::Guard guard(*EBRManager::instance());
                const auto* cached = vars.front()->loadHead();
                const auto value = cached->payload.value;
                destroyed = cached->payload.lifetime->destroyed_flag;
                ++c.slow_cycles;
                recorder.sample("slow_reader", "slow_pinned", c);
                waitUntil(Clock::now() + hold, c);
                const bool alive = destroyed && !destroyed->load(std::memory_order_acquire);
                const bool detached = vars.front()->loadHead()->payload.value
                    >= value + Var::MAX_HISTORY + 1;
                const bool correct = alive && detached && cached->payload.value == value
                    && cached->payload.complement == ~value;
                if (correct) ++c.protected_checks;
                else ++c.errors;
                recorder.sample("slow_reader", "slow_held", c, false, 0, 0, correct);
            }
            const auto released = Clock::now();
            const auto recovery_end = std::min(deadline, released + recovery);
            recorder.sample("slow_reader", "slow_released", c);
            while (destroyed && !destroyed->load(std::memory_order_acquire)
                   && !c.stop.load() && Clock::now() < recovery_end) {
                drain();
                std::this_thread::sleep_for(1ms);
            }
            const bool reclaimed = destroyed && destroyed->load(std::memory_order_acquire);
            if (reclaimed) ++c.release_checks;
            else ++c.errors;
            recorder.sample("slow_reader", "slow_drained", c, false, 0, 0, reclaimed,
                std::chrono::duration<double, std::milli>(Clock::now() - released).count());
            waitUntil(recovery_end, c);
        }
    } catch (const std::exception& error) {
        std::cerr << "slow reader: " << error.what() << '\n';
        ++c.errors;
        c.stop.store(true);
    }
}

std::string runPhase(const std::string& phase, const Options& o, Recorder& recorder, bool& passed) {
    Counters c;
    Variables vars;
    for (int i = 0; i < o.threads; ++i)
        vars.push_back(std::make_unique<Var>(makeValue(c, 0, true, i == 0)));
    recorder.sample(phase, "phase_start", c);
    observeUntil(Clock::now() + std::chrono::milliseconds(o.settle_ms), phase, c, recorder, o);
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(o.phase_seconds);
    if (phase == "thread_churn") {
        while (!c.stop.load() && Clock::now() < deadline) {
            const auto next_batch = std::min(deadline, Clock::now() + std::chrono::milliseconds(o.batch_ms));
            std::vector<std::thread> workers;
            for (int i = 0; i < o.threads; ++i)
                workers.emplace_back(worker, std::ref(*vars[i]), i, std::ref(c), std::cref(o),
                                     next_batch, false, o.batch_transactions);
            for (auto& thread : workers) thread.join();
            ++c.batches;
            const auto check = checkpoint(vars, c);
            recorder.sample(phase, "batch_joined", c, true, check.checksum, check.retained, check.ok);
            observeUntil(next_batch, phase, c, recorder, o);
        }
    } else {
        std::vector<std::thread> workers;
        for (int i = 0; i < o.threads; ++i)
            workers.emplace_back(worker, std::ref(*vars[i]), i, std::ref(c), std::cref(o), deadline, true, -1);
        std::thread reader;
        if (phase == "slow_reader")
            reader = std::thread(slowReader, std::ref(vars), std::ref(c), std::ref(recorder), std::cref(o), deadline);
        observeUntil(deadline, phase, c, recorder, o);
        for (auto& thread : workers) thread.join();
        if (reader.joinable()) reader.join();
    }
    const double work_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const auto check = checkpoint(vars, c);
    recorder.sample(phase, "drained", c, true, check.checksum, check.retained, check.ok);
    if (c.writes == 0 || (phase == "thread_churn" && c.batches < 2)) ++c.errors;
    if (phase == "slow_reader" && (c.slow_cycles == 0 || c.protected_checks != c.slow_cycles
                                   || c.release_checks != c.slow_cycles)) ++c.errors;
    c.teardown.store(true);
    vars.clear();
    drain();
    const bool destroyed = c.created == c.destroyed && c.retired == c.reclaimed
        && c.teardown_destroyed == check.retained && c.unpublished == 0;
    if (!destroyed) ++c.errors;
    recorder.sample(phase, "destroyed", c, true, check.checksum, 0, destroyed);
    observeUntil(Clock::now() + std::chrono::milliseconds(o.settle_ms), phase, c, recorder, o);
    recorder.sample(phase, "phase_end", c, true, check.checksum, 0, destroyed && c.errors == 0);
    const bool ok = c.errors == 0 && work_seconds >= o.phase_seconds;
    passed = passed && ok;
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << "{\"phase\":" << quote(phase) << ",\"work_seconds\":" << work_seconds
        << ",\"transactions\":" << c.transactions << ",\"writes\":" << c.writes
        << ",\"created\":" << c.created << ",\"destroyed\":" << c.destroyed
        << ",\"retired\":" << c.retired << ",\"reclaimed\":" << c.reclaimed
        << ",\"teardown_destroyed\":" << c.teardown_destroyed
        << ",\"unpublished\":" << c.unpublished << ",\"errors\":" << c.errors
        << ",\"checksum\":" << check.checksum << ",\"expected_checksum\":" << c.writes
        << ",\"retained_before_teardown\":" << check.retained
        << ",\"batches\":" << c.batches << ",\"threads_started\":" << c.threads_started
        << ",\"slow_cycles\":" << c.slow_cycles << ",\"protected_checks\":" << c.protected_checks
        << ",\"release_checks\":" << c.release_checks << ",\"passed\":" << (ok ? "true" : "false") << '}';
    std::cout << phase << " complete: writes=" << c.writes << " errors=" << c.errors << std::endl;
    return out.str();
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (i + 1 == argc) throw std::runtime_error("missing option value");
        const std::string value = argv[++i];
        if (name == "--samples") o.samples = value;
        else if (name == "--result") o.result = value;
        else {
            size_t consumed;
            const auto number = std::stoll(value, &consumed);
            if (consumed != value.size() || number <= 0 || number > 1000000000)
                throw std::runtime_error("invalid positive integer");
            if (name == "--phase-seconds") o.phase_seconds = number;
            else if (name == "--threads") o.threads = number;
            else if (name == "--sample-ms") o.sample_ms = number;
            else if (name == "--rate-per-worker") o.rate = number;
            else if (name == "--hold-ms") o.hold_ms = number;
            else if (name == "--recovery-ms") o.recovery_ms = number;
            else if (name == "--batch-ms") o.batch_ms = number;
            else if (name == "--batch-transactions") o.batch_transactions = number;
            else if (name == "--settle-ms") o.settle_ms = number;
            else if (name == "--rss-limit-mib") o.rss_limit_kib = number * 1024;
            else if (name == "--vms-limit-mib") o.vms_limit_kib = number * 1024;
            else throw std::runtime_error("unknown option: " + name);
        }
    }
    if (o.samples.empty() || o.result.empty() || o.threads > 128 || o.rate > 1000000
        || o.phase_seconds * 1000LL <= o.hold_ms + o.recovery_ms || o.batch_transactions < 2)
        throw std::runtime_error("invalid options or phase too short for slow-reader cycle");
    return o;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto o = parse(argc, argv);
        Recorder recorder(o);
        bool passed = true;
        const auto start = Clock::now();
        std::vector<std::string> phases;
        for (const auto& name : {"steady", "slow_reader", "thread_churn"})
            phases.push_back(runPhase(name, o, recorder, passed));
        std::ofstream result(o.result);
        if (!result) throw std::runtime_error("cannot open result output");
        result << "{\"schema_version\":1,\"allocator\":" << quote(STM::Memory::configuredMode())
               << ",\"version_allocator\":" << quote(STM::Memory::occMode())
               << ",\"phase_seconds\":" << o.phase_seconds << ",\"threads\":" << o.threads
               << ",\"wall_seconds\":" << std::chrono::duration<double>(Clock::now() - start).count()
               << ",\"passed\":" << (passed ? "true" : "false") << ",\"phases\":[";
        for (size_t i = 0; i < phases.size(); ++i) result << (i ? "," : "") << phases[i];
        result << "]}\n";
        result.close();
        if (!result) throw std::runtime_error("result write failed");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
