# BUG-09：OccSTM 并发剪枝的版本链边数据竞争

记录日期：2026-11-04。工作分支：`codex/occ-reclamation-audit`。

性能诊断提交 `8cd38e005627a6c139e266abc574c603d0df4ee6` 已推送到
`origin/codex/perf-diagnostics`，并确认 [CI #7](https://github.com/ZephyrVia/CaSTM/actions/runs/41765892054)
的 Release、VERIFY、ASan/UBSan 三个 job 全部通过。审计分支在该结果确认后创建。
该远端 CI 结果对应性能诊断提交；本页修复的结果是下述本地验证。

## 问题与复现证据

`VersionNode::prev` 原来是普通指针。读者通过 `Transaction::load` 遍历版本链；
写者提交新版本后，会在保留窗口的末端执行 `curr->prev = nullptr`，把旧尾链
交给 EBR 退休。读者不持有写者的条带锁，因此双方可能并发读写同一个 `prev`。

EBR 延迟释放节点，并不把普通指针访问变成原子操作。这是链边的数据竞争，
与“某次运行是否发生过早释放”是两个不同条件。

复现测试单独保存在提交 `c64e62b142278942c8d5011bf6de9ad110b00ec8`：
该提交仅增加测试和 CMake 登记，生产代码仍使用普通指针。
`PublishedEdgeReadOverlapsPruning` 让读者在 EBR 临界区内保留已发布的父节点，
与写者对该链边的剪断并发执行。停止标志使用 relaxed，不为链边访问人为增加
happens-before；读者直接访问链边，是为固定冲突位置。另一个并发测试覆盖真实
`Transaction::load/commit` 路径。

修复前运行退出码为 **66**，TSan 报告：

~~~text
WARNING: ThreadSanitizer: data race
  Write of size 8 ... committer ... include/OccSTM/TMVar.hpp:120
  Previous read of size 8 ... previous ... tests/OccSTM/test_Reclamation.cpp:54
SUMMARY: ThreadSanitizer: data race ... TMVar.hpp:120 in committer
~~~

以上行号指复现提交；[完整修复前日志](docs/validation/occ-reclamation-2026-11-04/tsan-before.txt)
包含读写栈、节点分配栈及线程来源。普通 Release 测试通过不能排除这个竞争。

## 修复与内存访问约定

- `prev` 改成 `std::atomic<VersionNode*>`。
- 私有新节点先以 relaxed 设置 prev，再通过原有 head release store 发布。
- `Transaction::load` 与提交者的链遍历显式使用 acquire load。
- 剪枝通过 `exchange(nullptr, acq_rel)` 同时断开链边并取得旧尾链，只退休非空返回值。
- 析构与 EBR grace period 后的尾链清理使用 relaxed load；这两处要求独占节点。
- 写者之间仍由原有条带锁串行化；EBR guard 的范围及快照不可用时的重试规则保持原有语义。

读者已经取得旧尾指针时，由它持有的 EBR guard 延迟物理回收；读者看到空链边且
找不到符合读时间戳的版本时，抛出 `RetryException`，调用方用新快照重试。
原子链边和 EBR 分别负责指针访问与节点生命周期，两者都需要。

本轮保留原来的窗口大小：从新 head 向后走 `MAX_HISTORY=8` 条边后再剪断，
因此最多保留 **9 个节点（head 加 8 个历史节点）**。没有借修复改变这一行为。
`TMVar` 析构仍要求调用者先停止所有使用它的线程。

## 新增回归

| 用例 | 验证内容 |
| --- | --- |
| ReaderObservesCutEdgeAndRetriesWithFreshSnapshot | 写者剪断后读者看到空边；旧快照报告不可用，重新 begin 能读到最新值 |
| CachedTailSurvivesRepeatedPruningUntilReaderLeaves | 读者持有的裸节点跨越多次剪枝仍可读；读者退出前不析构，退出后被回收 |
| MultipleWritesInOneCommitRetireEachNodeOnce | 同一提交多次写同一变量触发多轮剪枝，旧对象只析构一次 |
| PublishedEdgeReadOverlapsPruning | 修复前由 TSan 捕获已发布链边的并发读写；修复后允许读到旧指针或空指针 |
| ConcurrentSnapshotTraversalAndPruning | 4 个读者共 16,000 次旧快照读取，与 4,000 次真实事务提交并发执行 |

生命周期测试不复制所观察节点中的 shared_ptr，不用引用计数替代 EBR 保护。
Probe 的析构计数还检查了退休对象和变量最终销毁时的释放次数；这些计数不等于
TierAlloc 的 chunk 已归还操作系统，也不是完整 LSan 结论。

## 验证结果

环境：GCC 11.4.0，WSL2 / Linux 6.18.40.1-microsoft-standard-WSL2。

| 检查 | 结果 |
| --- | --- |
| Release / mode=0，原生分配路径 | 62 passed、1 disabled |
| Debug / VERIFY，原生分配路径 | 55 passed、7 skipped、1 disabled |
| Debug / ASan+UBSan / mode=0，原生分配路径 | 62 passed、1 disabled |
| Python runner | 9 个 unittest 方法通过，包括四后端短时集成检查 |
| TSan / OccSTM / system allocator | 20 个用例重复 10 轮，200 次执行通过，无 TSan 报告 |

TSan 未使用 suppression。由于本机默认启动会在 main 之前报 `unexpected memory
mapping`，用 `setarch x86_64 -R` 仅关闭该测试进程的地址随机化；未修改系统全局
设置。测试发现设为 PRE_TEST，避免构建阶段提前启动 TSan 程序。

TSan 的 `STM_ALLOCATOR_MODE=0` 同时传给库和测试，用于单独检查 STM 与回收路径。
没有把这项结果扩大到 TierAlloc 或整个仓库。其余三个构建使用原生分配路径。
ASan 设置 `detect_leaks=0`；没有完整泄漏检测结论。

日志、源码与日志 SHA-256、运行次数见
[机器可读验证记录](docs/validation/occ-reclamation-2026-11-04/result.json)及
[修复后 TSan 日志](docs/validation/occ-reclamation-2026-11-04/tsan-after.txt)。
CI 配置新增 `occ-tsan` job，使用同一构建参数、OccSTM 范围和重复次数；本地验证
不能当作该修复分支已通过远端 CI 的记录。

## 复现命令

从需要验证的源码目录执行。修复前切到 `c64e62b` 的独立 checkout，修复后使用
当前分支；分别使用不同构建目录，不与性能或 ASan 构建混用。

~~~bash
cmake -S . -B /tmp/castm-occ-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=thread -DSTM_WW_VERIFY_LOGIC_MODE=0 -DSTM_ALLOCATOR_MODE=0" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
cmake --build /tmp/castm-occ-tsan --target run_tests -j2

# 修复前：预期报告已发布链边的数据竞争，退出码 66。
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  /tmp/castm-occ-tsan/tests/run_tests --gtest_filter='OccReclamationTest.*'

# 修复后：20 个 OccSTM 用例，重复 10 轮。
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  /tmp/castm-occ-tsan/tests/run_tests --gtest_filter='Occ*' --gtest_repeat=10
~~~

Release、VERIFY、ASan/UBSan 和 Python 的构建命令沿用[测试说明](docs/testing.md)。

## 尚未覆盖

本轮修复已复现的 `prev` 数据竞争，并验证了列出的剪枝/退休交错；没有证明整个
事务协议的隔离级别、EBR 在所有内存模型下的正确性、无饥饿或任意分配异常下的
恢复。长时 RSS、慢读者造成的退休积压及线程 churn 仍需单独实验。

历史 108 次退避对照属于 `codex/perf-diagnostics` 的实现。原始 CSV、参数和哈希
保持原样，本轮短时集成检查不用于声称原子链边修复的吞吐收益或开销。
