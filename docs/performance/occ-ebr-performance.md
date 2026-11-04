# OccSTM 完成 EBR 修复后的性能复测

记录日期：2026-11-04，分支 `codex/occ-reclamation-audit`。
本报告测量最终源码提交 `19d43f6`，包含原子链边、退休桶跨代复用和线程槽位复用
三处并发修复。先确认四项远端 CI 全部通过，再运行同配置的 108 次策略对照。
共 **102/108 次有效**，其中 jitter **36/36**；所有运行最终 checksum 匹配。
20T、50R/50W、16 keys 的 jitter 为 **906,055 ± 21,193 TPS**，
p99 均值 **407.48 μs**。以下保留无效运行、低冲突结果及统计限制。

## 版本与验证依据

- 源码：[`19d43f6fb84753daea43f7d9bac795d3f395eeb3`](https://github.com/ZephyrVia/CaSTM/commit/19d43f6fb84753daea43f7d9bac795d3f395eeb3)。
- [远端 CI #11](https://github.com/ZephyrVia/CaSTM/actions/runs/41772192425)：mode=0、VERIFY、ASan/UBSan、OccSTM/EBR TSan 四项均成功；
  [核验记录](../validation/slot-reuse-2026-11-04/ci.json)保存精确提交和各 job 链接。
- 本地 Release / ASan+UBSan 各 64 个用例通过；VERIFY 57 个通过、7 个跳过；
  原有 1 个用例 disabled；Python 9 项通过。
- TSan 使用 system allocator，覆盖 20 个 OccSTM 用例与 2 个 EBR 用例；本地普通
  调度和双核调度各 100 轮（合计 4,400 次），CI 为 50 轮（1,100 次）。ASan 关闭泄漏检测。
- 构建、测量时工作区干净，归档前核对 metadata 中 95 个源码/构建/脚本文件的
  哈希与上述提交一致。二进制 SHA-256：`a792a0d7045b6a09023de1cef91d52661b0c7ef0962787edf8d884670cf42c8e`。

此次收尾曾出现两次 TSan CI 失败，本地分别复现了节点释放竞争和槽位 next 读写
竞争，见 [BUG-10](../../BUG-10-EBR纪元推进与退休桶复用竞争.md) 与
[BUG-11](../../BUG-11-EBR线程槽位复用链边竞争.md)。失败及修复日志保留，没有删除
失败记录或屏蔽 sanitizer 检查。远端旧失败的完整栈未取得，不声称已还原其每个交错。

## 实验方法

| 参数 | 设置 |
| --- | --- |
| 环境 | i5-14600KF，20 logical CPUs，GCC 11.4.0，WSL2 / Linux 6.18.40.1 |
| 构建 | Release -O3，VERIFY=0，TEST_HOOKS=0；无 sanitizer、无 -march=native |
| 后端 / 分配器 | OccSTM / native，VersionNode 使用 TierAlloc |
| 策略 | none、yield、jitter，同一二进制 |
| 线程 / 读写比 | 1、20 / 90R/10W、50R/50W、10R/90W |
| key / 操作 | low 65,536、high 16；每事务 8 个操作 |
| 预热 / 测量 / 重复 | 300 ms / 1,500 ms / 每配置 3 个独立进程 |
| 顺序 / seed | 同一 workload/repetition 内轮换策略；3394748143，重试重放同一批次 |
| 尝试上限 / timeout | 每逻辑事务 1,000,000 次尝试 / 每进程 40 秒 |

沿用前两轮实验参数，未提高 retry guard。性能矩阵期间没有并行构建或测试。
性能运行未绑定 CPU、未固定频率，也没有调度追踪；双核限制只用于上面的 TSan
回归，不用于本性能矩阵。TPS 是已提交逻辑事务/实际测量秒数，等待在途事务结束。

## 有效性与失败原因

| 策略 | 有效 / 总运行 | retry_limit | checksum mismatch |
| --- | ---: | ---: | ---: |
| none | 30/36 | 6 | 0 |
| yield | 36/36 | 0 | 0 |
| jitter | 36/36 | 0 | 0 |
| 合计 | 102/108 | 6 | 0 |

所有 6 次失败均为 retry_limit，没有 timeout、signal 或异常失败。分布为：

| 线程 | workload | 冲突 | 策略 | 失败阶段 | 次数 |
| --- | --- | --- | --- | --- | ---: |
| 20 | 10R/90W | high | none | measure | 2 |
| 20 | 10R/90W | high | none | warmup | 1 |
| 20 | 50R/50W | high | none | measure | 1 |
| 20 | 50R/50W | high | none | warmup | 2 |

每次失败所在阶段都记录到一个逻辑事务的 999,999 次重试，达到原定尝试上限。
20T、50R/50W、16 keys 的 none 在三次运行的预热与测量阶段合计，read_locked
占全部 abort 的 **99.940%**。原因只记录首次检测失败，不等于锁等待时间或全部冲突条件。
失败运行也参与原因统计；不将失败阶段没有测量提交解释为没有重试。

## 热点性能与重试

以下均为 20 线程、16 keys。TPS 为 3 次有效运行的均值 ± 样本标准差，abort rate
和 p99 为各运行指标的均值。未满 3 次有效的配置标为 N/A，原始幸存样本仍在 CSV
中；不据这些样本计算正常速度比。延迟每 worker 每 256 次成功提交采样，包含
内部重试，不包含失败逻辑事务。

| workload | 策略 | 有效 | TPS | abort rate | p99（μs） |
| --- | --- | ---: | ---: | ---: | ---: |
| 90R/10W | none | 3/3 | 40,709 ± 15,321 | 99.823% | 5,103.83 |
| 90R/10W | yield | 3/3 | 510,438 ± 25,498 | 97.620% | 546.89 |
| 90R/10W | jitter | 3/3 | 3,466,956 ± 74,374 | 26.504% | 157.42 |
| 50R/50W | none | 0/3 | N/A | N/A | N/A |
| 50R/50W | yield | 3/3 | 25,236 ± 2,574 | 99.802% | 16,352.60 |
| 50R/50W | jitter | 3/3 | 906,055 ± 21,193 | 39.721% | 407.48 |
| 10R/90W | none | 0/3 | N/A | N/A | N/A |
| 10R/90W | yield | 3/3 | 119,454 ± 38,686 | 97.586% | 1,204.62 |
| 10R/90W | jitter | 3/3 | 711,284 ± 58,495 | 33.477% | 631.14 |

20T、50R/50W、16 keys 的 jitter 三次测量中，单逻辑事务最大重试为 **116、75、63**；
yield 为 **213055、340482、235612**。jitter 每个 worker 每次均有提交，跨三次运行的 worker
提交数范围为 **63,441～73,931**。这些是有限窗口的进展证据，不是无饥饿保证。

## 低冲突与单线程

以下均为 65,536 keys，none/jitter 各 3/3 次有效；变化为本轮 jitter 相对 none。
全部单线程运行在预热和测量中均无 abort，差异不能归因于实际退避等待。

| 线程 | workload | none 平均 TPS | jitter 平均 TPS | 变化 |
| --- | --- | ---: | ---: | ---: |
| 1 | 90R/10W | 1,821,399 | 1,845,290 | +1.31% |
| 1 | 50R/50W | 624,692 | 627,751 | +0.49% |
| 1 | 10R/90W | 451,096 | 439,754 | -2.51% |
| 20 | 90R/10W | 5,200,688 | 5,067,285 | -2.57% |
| 20 | 50R/50W | 1,178,666 | 1,184,004 | +0.45% |
| 20 | 10R/90W | 690,225 | 668,254 | -3.18% |

逐配置标准差保存在 summary。三次短时样本不能证明小幅差异显著或性能等价，
低冲突收益与退化均应连同 workload 展示，不能将热点收益推广到所有负载。

## 历史结果的使用边界

[第一轮退避实验](occ-retry-diagnostics.md)和[原子链边修复后的中间版本](occ-postfix-performance.md)
各自保留完整原始数据。中间版本 `9bccd33` 的代表点为约 93.1 万 TPS，但其代码
仍有后来确认的 EBR 竞争，不能继续用作最终版本的性能数字。
本轮不把含已知竞争的旧实现作为语义等价的安全基线，也没有交替重跑两个版本；
因此不报告“EBR 修复开销百分比”。策略比较只使用本轮同一修复二进制的结果。

## 数据与复现

- [raw.csv](data/occ-ebr-2026-11-04/raw.csv)：108 行，保留全部失败。
- [summary.csv](data/occ-ebr-2026-11-04/summary.csv)：36 组，仅对有效运行计算均值和样本标准差。
- [metadata.txt](data/occ-ebr-2026-11-04/metadata.txt)、[provenance.json](data/occ-ebr-2026-11-04/provenance.json)：环境、源码、二进制、CI、参数与文件哈希。
- [逐线程诊断](data/occ-ebr-2026-11-04/diagnostics.jsonl.gz)：每行 `{path, data}`，path 对应 raw 的 diagnostics_file。
- [runner 日志](data/occ-ebr-2026-11-04/runner.txt)：顺序、有效性和失败原因。

在测量提交的独立 checkout 中，从仓库根目录执行：

~~~bash
cmake -S . -B /tmp/castm-occ-ebr-bench \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0"
cmake --build /tmp/castm-occ-ebr-bench --target stm_benchmark -j2

python3 scripts/run_retry_ablation.py \
  --binary /tmp/castm-occ-ebr-bench/benchmarks/stm_benchmark \
  --output-dir bench/results/occ-ebr-ablation \
  --threads 1,20 --policies none,yield,jitter --repetitions 3 \
  --warmup-ms 300 --measure-ms 1500 --max-attempts 1000000 --timeout-seconds 40
~~~

输出目录必须尚不存在。包含无效运行时 runner 返回 1，但仍保留结果；本轮退出
码 1 对应上述 6 次 retry_limit，失败次数和阶段可能随调度变化。在含报告的 checkout
中离线复核：

~~~bash
python3 scripts/verify_retry_report.py docs/performance/data/occ-ebr-2026-11-04
~~~

本快照结果为 `runs=108`、`valid=102`、`retry_limit=6`。复核包括文件哈希、
汇总重算、CSV/JSON 对应、逐线程/阶段计数、直方图、checksum 与失败 retry guard。

## 尚未覆盖

本轮是 benchmark 手动事务循环，未单独测量公共 `STM::atomically` 的端到端吞吐。
benchmark 默认仍为 none，通过 `--backoff jitter` 选择随机退避。没有重跑四后端
或 allocator 矩阵，不据此声称优于 mutex/WwSTM、业务 QPS 或磁盘存储性能。

测试与 checksum 只覆盖明确断言的行为，不证明完整隔离级别、全部 EBR 内存模型
或无泄漏。退避仍持有 EBR guard；长时 RSS、慢读者积压和长期线程 churn 仍待验证。
