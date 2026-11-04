# 测试范围与验证记录

版本链剪枝审计分支 `codex/occ-reclamation-audit` 在已通过远端 CI 的性能诊断提交
`8cd38e0` 上补充版本链剪枝审计。入口仍是
[tests/CMakeLists.txt](../tests/CMakeLists.txt) 和 [CI 配置](../.github/workflows/ci.yml)。

## 长时内存与回收验证

`codex/occ-memory-soak` 在未更改运行时源码的情况下，对 OccSTM 的 native 和 system
分配路径各运行 30 分钟，覆盖稳定线程、慢读者和线程反复创建退出。六个阶段的
checksum、探针回收与最终析构对账通过；默认 TierAlloc 在线程 churn 下的虚拟内存
持续增长。[报告、曲线和原始数据](performance/occ-memory-soak.md)说明范围与限制。

## 线程槽位复用修复的追加验证

`52c39dd` 的 TSan 仍在远端失败。本地普通调度下多次通过后，限制为两个逻辑 CPU
复现 `LockFreeReuseStack` 的 next 读写竞争，详见 [BUG-11](../BUG-11-EBR线程槽位复用链边竞争.md)。
最新修复新增槽位独占所有权回归，总计登记 65 个 C++ 用例：Release、ASan/UBSan
各 64 个通过；VERIFY 57 个通过、7 个跳过；均保留 1 个禁用用例。Python 9 个方法通过。
TSan 范围为 system allocator 下 20 个 OccSTM 用例与 2 个 EBR 用例，CI 重复 50 轮。
本地普通调度和双核调度各重复 100 轮，共 4,400 次执行通过，独立日志及哈希见
[验证记录](validation/slot-reuse-2026-11-04/result.json)。各阶段的失败与通过均分别保留。
最终源码 `19d43f6fb84753daea43f7d9bac795d3f395eeb3` 的
[CI #11](https://github.com/ZephyrVia/CaSTM/actions/runs/41772192425) 四项 job 均成功，
见 [CI 核验记录](validation/slot-reuse-2026-11-04/ci.json)。四项通过后执行的 108 次
性能复测有 102 次有效、6 次 retry_limit，108 次 checksum 均匹配，见
[最终性能报告](performance/occ-ebr-performance.md)。

## EBR 桶复用修复的追加验证

`89578c1` 的 [CI #9](https://github.com/ZephyrVia/CaSTM/actions/runs/41769838034) 中
TSan 失败，其他三项成功。本地对原 `9bccd33` 二进制增加重复次数后，第 10 轮
报告读者访问与版本节点释放的数据竞争。完整证据与修复见
[BUG-10](../BUG-10-EBR纪元推进与退休桶复用竞争.md)。这说明下文历史 10 轮通过有其覆盖限制。

桶复用修复阶段新增 EBR 并发回归，当时总计登记 64 个 C++ 用例：Release 和 ASan/UBSan
各 63 个通过；VERIFY 为 56 个通过、7 个跳过；均保留原有 1 个禁用用例。Python
9 个方法通过。TSan 对 system allocator 下的 20 个 OccSTM 用例及新增 EBR 用例
重复 100 轮，2,100 次执行通过；CI 使用同一过滤范围重复 50 轮（1,050 次）。
没有全仓库/TierAlloc 的 TSan 或完整泄漏检测结论。
[验证记录](validation/ebr-reclamation-2026-11-04/result.json)保留本地日志与哈希。

下文为此前原子链边修复及更早阶段的记录，测试数量、CI 和性能不能直接挪用为
这次 EBR 修复的结果。

## 并发剪枝审计的验证

新增 `OccSTM/test_Reclamation.cpp` 的 5 个用例，总计登记 63 个 C++ 用例：

| 构建 | 通过 | 跳过 | 禁用 | 失败 |
| --- | ---: | ---: | ---: | ---: |
| Release / mode=0 | 62 | 0 | 1 | 0 |
| Debug / VERIFY | 55 | 7 | 1 | 0 |
| Debug / ASan+UBSan / mode=0 | 62 | 0 | 1 | 0 |

Python 的 9 个 unittest 方法通过。额外使用 system allocator 对 20 个 OccSTM
用例做 TSan 验证，重复 10 轮、200 次执行通过；这不是全仓库或 TierAlloc 的 TSan
通过结论。修复前的 `c64e62b` 在同样 TSan 设置下复现了已发布 prev 链边的数据竞争。
测试覆盖持有旧尾指针、看到剪断边、重复退休及真实事务的并发遍历/剪枝。

本机 TSan 需要 `setarch x86_64 -R` 调整单个进程的地址布局；没有全局关闭 ASLR。
ASan 仍关闭泄漏检测，1 个 WwSTM 高冲突用例仍保持 disabled。
[BUG-09](../BUG-09-OccSTM并发剪枝链边数据竞争.md) 说明根因、内存访问顺序和复现命令；
[验证记录与原始日志](validation/occ-reclamation-2026-11-04/result.json) 保存哈希及完整范围。

性能诊断提交的 [远端 CI #7](https://github.com/ZephyrVia/CaSTM/actions/runs/41765892054)
已确认三项 job 成功。审计修复提交 `9bccd33aa23ad965fa0b4a82230f73c32dc67f5f`
推送后，[远端 CI #8](https://github.com/ZephyrVia/CaSTM/actions/runs/41768629427)
的 mode=0、VERIFY、ASan/UBSan、OccSTM TSan 四项 job 全部成功；精确提交与 job
链接见 [CI 核验记录](validation/occ-reclamation-2026-11-04/ci.json)。上述测试数量
来自保留的本地日志，远端状态单独记录。原 result.json 是推送前的本地验证快照。

四项 CI 通过后，已对该干净提交执行 108 次性能复测，103 次有效，5 次立即重试
触发 retry_limit，108 次最终 checksum 均匹配。性能失败与单元测试结果分别记录，
见[修复后性能报告](performance/occ-postfix-performance.md)。下文保留先前两个阶段的验证记录。

## 诊断与退避分支的新增覆盖

性能诊断阶段新增登记 21 个 C++ 用例，总计 58 个，其中原有 1 个高冲突 WwSTM 用例
仍保持 disabled。本地结果为：

| 构建 | 通过 | 跳过 | 禁用 | 失败 |
| --- | ---: | ---: | ---: | ---: |
| Release / mode=0 | 57 | 0 | 1 | 0 |
| Debug / VERIFY | 50 | 7 | 1 | 0 |
| Debug / ASan+UBSan / mode=0 | 57 | 0 | 1 | 0 |

完整本地结果见[本分支验证记录](validation/2026-11-04-perf-diagnostics.json)。

| 新纳入的测试源文件 | 用例数 | 主要覆盖 |
| --- | ---: | --- |
| OccSTM/test_TMVar.cpp | 4 | 初始化、复杂值、版本链和分配接口 |
| OccSTM/test_Transaction.cpp | 11 | 读自己的写、保留的历史读取、三类失败、失败提交不发布且释放锁、重新 begin、受限并发累加、atomically 返回值和异常 |
| benchmarks/test_Diagnostics.cpp | 6 | 失败尝试计数、异常分类、直方图汇总、退避触发边界与随机延迟上限 |

另有 9 个 Python unittest 方法，覆盖 CSV 异常文本保留、非零退出、信号和超时分类、
策略分组、旧归档汇总不变，以及四种后端的短时 CSV/JSON/线程计数对账和报告写入失败。
`STM_BENCHMARK_BIN` 未设置时，其中 2 个二进制集成检查会跳过；Release CI 已配置
该环境变量并构建 benchmark。没有把本地结果当成远端 CI 通过记录。

~~~bash
STM_BENCHMARK_BIN=/tmp/castm-package-release/benchmarks/stm_benchmark \
  python3 -m unittest discover -s tests/scripts -v
~~~

新启用的 OccSTM 测试不代表全部历史 OccSTM 测试已启用。`test_STM.cpp`、
`test_STM_Tree.cpp` 仍在默认构建之外。测试限定在已断言的行为，不将单变量保留
历史的成功读取表述为完整快照隔离证明。下文保存的是原型标签发布前的历史验证，
其中“本轮”和用例计数均指当时的文档收尾。

## v0.1.0-prototype 发布前的验证记录

在独立的干净构建目录中验证，环境为 GCC 11.4.0、CMake 3.22.1、
Python 3.10.12、WSL2 / Linux 6.18.40.1-microsoft-standard-WSL2。
日期按 Asia/Shanghai 记录。

| 构建 | 登记用例 | 实际通过 | 按模式跳过 | 禁用 | 失败 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Release，mode=0 | 37 | 36 | 0 | 1 | 0 |
| Debug，VERIFY=1 | 37 | 29 | 7 | 1 | 0 |
| Debug，ASan + UBSan，mode=0 | 37 | 36 | 0 | 1 | 0 |

每种构建各运行一次当前 CTest 集合，单例 timeout 为 60 秒。这是本轮构建与回归
检查，不替代长期压力测试。ASan 使用 `detect_leaks=0:halt_on_error=1`，
UBSan 使用 `halt_on_error=1:print_stacktrace=1`；本轮没有 sanitizer 错误报告。
环境、用例清单和短时 benchmark 检查见[机器可读记录](validation/2026-11-04.json)。

CTest 在 VERIFY 下可能输出“0 tests failed out of 36”，其中包含 7 个 Skipped，
不能据此写成“36 个实际执行通过”。禁用用例不进入本轮通过数。
此处是本地运行结果，CI 配置列在下文；没有将远端 CI 状态推断为已通过。

## 原型标签默认纳入构建的源文件

| 测试源文件 | 登记数 | mode=0 通过 | VERIFY 通过 | VERIFY 跳过 | 禁用 |
| --- | ---: | ---: | ---: | ---: | ---: |
| [EBRManager/test_EBRManager.cpp](../tests/EBRManager/test_EBRManager.cpp) | 6 | 6 | 6 | 0 | 0 |
| [WwSTM/test_STM_Tree.cpp](../tests/WwSTM/test_STM_Tree.cpp) | 2 | 1 | 1 | 0 | 1 |
| [WwSTM/test_stm_math.cpp](../tests/WwSTM/test_stm_math.cpp) | 2 | 2 | 2 | 0 | 0 |
| [WwSTM/test_tmvar.cpp](../tests/WwSTM/test_tmvar.cpp) | 21 | 21 | 17 | 4 | 0 |
| [WwSTM/test_txdescriptor.cpp](../tests/WwSTM/test_txdescriptor.cpp) | 4 | 4 | 1 | 3 | 0 |
| [WwSTM/test_stm_linkedlist.cpp](../tests/WwSTM/test_stm_linkedlist.cpp) | 2 | 2 | 2 | 0 | 0 |
| 合计 | 37 | 36 | 29 | 7 | 1 |

这些用例覆盖 EBR 的 epoch 回归、WwSTM 基础读写、多变量提交与 helper 交错、
已发布 Record/descriptor 生命周期、对齐，以及树路径更新、并发累加和链表追加。
涉及 TierAlloc 的集成测试不等于覆盖其全部独立接口。

## 按模式跳过的 7 个用例

以下用例在 VERIFY 下调用 GTEST_SKIP；它们在本轮 mode=0 和 ASan+UBSan 下均执行通过：

- TMVarTest.HeadIdentityChangeRetriesSnapshot
- TMVarTest.PublishedWriteRecordRetiresAfterHelper
- TMVarTest.PublishedWriteRecordRetiredExactlyOnce
- TMVarTest.OwnerCleanupAfterHelperUsesTokenOnly
- TxDescriptorTest.ReclamationWaitsForReaderHoldingPublishedRecord
- TxDescriptorTest.HelperCanReadTerminalOwnerAfterOwnerCleanup
- TxDescriptorTest.TransactionDescriptorChurnReclaimsThroughEbr

前者需要 mode=0 的确定性 head 交错 hook，其余用例需要真实 EBR 物理回收。
VERIFY 为逻辑测试保留部分已发布对象，不能用于验证这些回收条件。
EBRManager 独立用例仍会在 VERIFY 构建中运行。

## 禁用的高冲突用例

`WwSTMTreeTest.DISABLED_HighContentionProgressAndConsistency` 位于
[树测试](../tests/WwSTM/test_STM_Tree.cpp)。它配置 8 个线程、每线程 3,000 次提交、
每次逻辑提交最多 100,000 次重试，并检查最终根节点与路径计数。

它在历史提交 `01178fe` 引入时即带 DISABLED 前缀。提交说明提到稳定性重构，
但没有单独记录这个用例的禁用理由，不能将具体原因补写成已确认结论。
本轮保持原状，未单独启用执行；当前是否稳定通过仍需另行评估。
已有 benchmark 的高冲突完成记录不等于该用例已通过。

## 原型标签未纳入默认构建的历史源文件

| 范围 | 文件与状态 |
| --- | --- |
| TierAlloc 独立测试 | test_central_heap.cpp、test_AtomicFreeList.cpp、test_slab.cpp、test_SizeClassPool.cpp、test_SizeClassConfig.cpp、test_ThreadHeap.cpp；在 CMake 中被注释 |
| OccSTM 独立测试 | test_TMVar.cpp、test_Transaction.cpp、test_STM.cpp、test_STM_Tree.cpp；在 CMake 中被注释 |
| WwSTM 历史上下文测试 | test_TxContextSingleThread.cpp、test_TxContextMultiThread.cpp；在 CMake 中被注释 |
| WwSTM 其他历史文件 | WwSTM_test.cpp；未列入 run_tests 目标 |

本轮未编译或运行这些历史测试，不对其当前兼容性、通过情况或禁用动机作推断。
OccSTM 在 benchmark 中运行的整数读改写 workload 只能提供该 workload 的完成与
checksum 证据，不能替代独立单元测试覆盖。

## 复现命令

从仓库根目录运行。使用独立目录，避免 VERIFY、性能和 sanitizer 参数混用。

### Release / mode=0

~~~bash
cmake -S . -B /tmp/castm-package-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0"
cmake --build /tmp/castm-package-release --target run_tests stm_benchmark -j2
ctest --test-dir /tmp/castm-package-release --output-on-failure --timeout 60
~~~

### Debug / VERIFY

~~~bash
cmake -S . -B /tmp/castm-package-verify \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=1"
cmake --build /tmp/castm-package-verify --target run_tests -j2
ctest --test-dir /tmp/castm-package-verify --output-on-failure --timeout 60
~~~

### ASan + UBSan / mode=0

~~~bash
cmake -S . -B /tmp/castm-package-asan-ubsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -DSTM_WW_VERIFY_LOGIC_MODE=0" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  cmake --build /tmp/castm-package-asan-ubsan --target run_tests -j2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /tmp/castm-package-asan-ubsan --output-on-failure --timeout 60
~~~

构建阶段也传 sanitizer 环境变量，因为 GoogleTest 的发现步骤会启动测试程序。

### 查看用例与指定回归

~~~bash
ctest --test-dir /tmp/castm-package-release -N
ctest --test-dir /tmp/castm-package-release --show-only=json-v1
/tmp/castm-package-release/tests/run_tests \
  --gtest_filter='TMVarTest.MultiVariableAtomicityStress:TxDescriptorTest.*'
~~~

### 短时 benchmark 运行检查

~~~bash
python3 scripts/run_stm_benchmark.py \
  --binary /tmp/castm-package-release/benchmarks/stm_benchmark \
  --smoke --threads 1,4 --warmup-ms 100 --measure-ms 300 --repetitions 1
~~~

这组参数为四个后端 × 两个线程点 × 三个 workload，共 24 次进程运行。
本轮 24/24 次有效，最终 checksum 均匹配；README 中 WwSTM 的单点命令也通过。
结果用于检查 runner、有效性和 checksum，不用于替代[历史性能矩阵](performance/README.md)。

## CI 与验证边界

现有 CI 在 push / pull_request 时配置 mode=0、VERIFY 和 ASan+UBSan 三个 job。
它们使用同一 tests/CMakeLists.txt，不自动启用上述历史源文件或 DISABLED 用例，
也不运行完整性能矩阵。

- TSan 本轮未运行；[历史 BUG-08](../BUG-08-WwSTM-TxDescriptor生命周期与对齐.md)
  记录过 unexpected memory mapping 环境错误，不将其解释为代码通过或永久不可运行。
- LSan 未完整验证：当前 ASan 关闭 leak detection，VERIFY 还会有意保留部分对象。
- [SizeClassPool](../src/TierAlloc/ThreadHeap/SizeClassPool.cpp) 在线程退出时保留部分
  chunk；进程长期运行、线程 churn 和慢读者下的内存有界性尚未得到本轮验证。
- 有限回归测试和最终 checksum 不构成全执行历史的隔离级别、无饥饿或无锁进展证明。
- BUG 文档中的“全量 ×N”是各修复阶段的已启用集合，不能与本页用例数相加。
