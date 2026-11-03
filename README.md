[![CI](https://github.com/ZephyrVia/CaSTM/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ZephyrVia/CaSTM/actions/workflows/ci.yml)

# CaSTM

**C++17 软件事务内存与并发控制原型**

CaSTM 将进程内共享变量的读写组织成事务，实现了 OccSTM 和 WwSTM 两条并发控制
路线，并包含 EBR 延迟回收与 TierAlloc 分层分配器。项目重点是多变量提交、并发
对象生命周期，以及不同冲突与分配策略的实验比较。

## 阅读入口

| 内容 | 文档 |
| --- | --- |
| WwSTM 当前协议与实现位置 | [设计说明](OSTM_WW.md) |
| 构建选项与代码阅读顺序 | [构建指南](OSTM_WW%20系统构建指南.md) |
| 启用、跳过、禁用及未纳入构建的测试 | [测试范围与验证记录](docs/testing.md) |
| 性能结论、限制及可核验 CSV | [性能报告](docs/performance/README.md) |
| benchmark 构建、运行和汇总 | [实验使用说明](benchmarks/README.md) |
| 并发问题的取证与修复过程 | [BUG 索引](BUG-00-索引.md) |
| 项目展示 | [简历描述](docs/interview/resume-bullets.md)、[面试讲稿](docs/interview/project-story.md) |

## 实现概览

~~~text
Application
  ├── Occ::Transaction / STM::atomically
  │     └── Occ::TMVar → 版本链、读写集、条带锁
  └── Ww::TxContext
        └── Ww::TMVar → tagged head、WriteRecord、TxDescriptor
                    ↓
          EBR + system allocator / TierAlloc
~~~

- **OccSTM**：基于时间戳与版本链读取数据，写事务在提交时验证读集并使用条带锁，
  冲突时重试。`STM::atomically` 当前封装的是 OccSTM。
- **WwSTM**：使用 Wound-Wait 风格的主动中止与重试，通过 single-head Locator
  统一共享变量的状态入口。应用使用 `TxContext`，与 OccSTM 的接口尚未统一。
- **EBR**：保护并发 reader、owner、helper 使用的退休对象，等待 grace period
  后执行析构与释放。
- **TierAlloc**：提供 CentralHeap → ThreadHeap → SizeClassPool 分层分配及跨线程
  释放。默认 OccSTM 的版本节点走 TierAlloc，WwSTM 的 VersionNode/WriteRecord
  走系统堆；benchmark 可切换这些关键路径。

## WwSTM 的提交与回收

共享变量只有一个 tagged head：

~~~text
head → VersionNode | WriteRecord
                         ├── owner → TxDescriptor
                         ├── old_node
                         └── new_node
~~~

对其他事务，ACTIVE/ABORTED Record 的逻辑值来自 old_node，COMMITTED Record
来自 new_node；owner 可以读取自己的草稿。读操作从同一节点取得 value/version，
复核 head identity，并在事务验证阶段检查读集。

提交顺序是：

~~~text
validate → prepare all Records → ACTIVE --CAS--> COMMITTED → help / flatten
~~~

descriptor 的状态 CAS 是逻辑提交点。所有 Record 在这个点之前准备好，之后
Record → Node 的展平只负责物理清理。成功提交后不能再回滚为 ABORTED。
多变量读者仍需验证并成功提交，不能把任意事务中的中间读值直接当作一致快照。

已发布的 Record 和被替换节点通过 EBR 退休；descriptor 在对应 Record 清理完成后
退休。`retire()` 使用退休时读取的**全局 epoch**标记对象，避免落后的线程 epoch
使对象过早进入回收窗口。descriptor 使用满足对齐要求的系统分配；
GarbageNode 元数据独立使用系统堆，payload 由对应 deleter 释放。

主要修复记录包括 [value/version 偏斜](BUG-06-WwSTM版本值偏斜丢失更新.md)、
[single-head 提交协议](BUG-07-WwSTM-single-head-Locator协议设计审计.md)、
[EBR 提前回收](BUG-02-EBR-retire纪元标记提前回收.md)和
[descriptor 生命周期与对齐](BUG-08-WwSTM-TxDescriptor生命周期与对齐.md)。

## 验证范围

2026-11-04 在源码基线 `da9634b` 上复核；本轮收尾只修改文档与归档数据。

- Release / mode=0：36 个测试通过，1 个高冲突测试保持 disabled。
- Debug / VERIFY：29 个通过，7 个按设计跳过，1 个 disabled。
- ASan + UBSan / mode=0：36 个通过，1 个 disabled；命令见[验证记录](docs/testing.md#本轮验证记录)。
- 默认 CTest 未纳入 OccSTM 独立测试、TierAlloc 独立测试及部分历史 WwSTM 测试。
- TSan 未在本轮验证；ASan 使用 `detect_leaks=0`，没有完整的 LSan 通过结论。

VERIFY 用于逻辑验证，保留部分已发布对象，不验证 WwSTM 的真实 EBR 物理回收。
mode=0 包含真实退休路径，但 TierAlloc 在线程退出时仍保留部分 chunk 到进程结束，
长期线程 churn 的内存有界性尚未得到验证。

## 性能结果

已有实验使用同一 Shared KV/Array workload，每个事务 8 个操作，比较 mutex、
shared_mutex、OccSTM 和 WwSTM。以下是历史实验结果，本轮没有重跑完整矩阵：

| 后端 | 有效运行 / 总运行 |
| --- | ---: |
| std::mutex | 108 / 108 |
| std::shared_mutex | 108 / 108 |
| OccSTM | 97 / 108 |
| WwSTM | 108 / 108 |

432 次运行的最终 checksum 均匹配；11 次无效运行集中在 OccSTM 高冲突配置，
原 CSV 保留 `exit code 1`，不纳入性能均值。既有分析将其归为重试耗尽的进展问题，
原始记录的诊断粒度与结论限制见[性能报告](docs/performance/README.md)。

锁基线在这组短事务实验中通常更快。WwSTM 的结果支持该矩阵中的完成情况比较，
不代表普遍吞吐领先或无饥饿保证。

另有 60 次分配器对照：57 次有效，0 次 checksum mismatch。有效且两侧样本齐全的
低冲突代表配置中，TierAlloc 吞吐提升约 15.5%～30.3%；WwSTM 20T、
10R/90W、高冲突配置则下降约 11.0%。[报告与数据快照](docs/performance/README.md)
包含参数、版本、失败点及统计边界。

![50R/50W high throughput](docs/images/benchmark/throughput-50r50w-high.png)

## 快速构建与运行

已验证环境为 Linux/WSL2、GCC 11.4、CMake 3.22。代码使用 C++17，
CMake 项目最低版本为 3.14，GoogleTest 已随仓库提供；Python runner 使用 Python 3。
VERIFY 与 sanitizer 的独立构建命令见[测试说明](docs/testing.md)。

在仓库根目录运行：

~~~bash
cmake -S . -B /tmp/castm-package-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0"
cmake --build /tmp/castm-package-release --target run_tests stm_benchmark -j2
ctest --test-dir /tmp/castm-package-release --output-on-failure --timeout 60
~~~

单点运行检查：

~~~bash
/tmp/castm-package-release/benchmarks/stm_benchmark \
  --backend ww --threads 4 --read-ratio 50 --contention low \
  --warmup-ms 100 --measure-ms 300 --run 1 --csv
~~~

短时运行用于检查接入和 checksum；性能结论使用报告中的独立进程重复实验。

## 当前范围

这是单机进程内的内存事务原型，目前没有 WAL、崩溃恢复或网络服务。
代码同时使用原子操作和互斥锁；已有测试和实验不构成 lock-free、wait-free、
无饥饿或完整隔离级别的证明。测试和性能报告分别列出已验证行为与尚未验证的范围。

## 目录

~~~text
include/OccSTM      乐观事务、版本链与条带锁
include/WwSTM       Locator、descriptor、事务上下文与提交协议
include/EBRManager  线程槽位、退休队列与延迟回收
include/TierAlloc   中央堆、线程堆与 size class
src                非模板实现
tests              当前及历史 GoogleTest 源文件
benchmarks         统一事务 workload
scripts            实验 runner、汇总与绘图
docs/testing.md    测试范围与验证记录
docs/performance   历史实验报告、CSV 与来源记录
docs/interview     简历与面试材料
BUG-*.md           各修复阶段的历史取证记录
~~~
