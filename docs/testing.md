# 测试范围与验证记录

本页对应 2026-11-04 文档收尾所使用的源码基线
`da9634b47bd1e37e0700725e4aab845393d5213b`。本轮不修改算法、测试代码、
启用列表或 CI 配置。当前入口是 [tests/CMakeLists.txt](../tests/CMakeLists.txt)
和 [CI 配置](../.github/workflows/ci.yml)。

## 本轮验证记录

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

## 默认纳入构建的源文件

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

## 未纳入默认构建的历史源文件

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
