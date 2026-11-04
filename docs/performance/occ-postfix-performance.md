# OccSTM 并发剪枝修复后的性能复测

> 中间版本记录：本报告测量 `9bccd33`。后续重复验证发现该版本仍有
> [EBR 退休桶复用竞争](../../BUG-10-EBR纪元推进与退休桶复用竞争.md)。本批次原始
> 数据与当时 CI 结果保留，但不能作为后续 EBR 修复的性能或安全性结论。

记录日期：2026-11-04，分支 `codex/occ-reclamation-audit`。
本轮先推送修复并确认四项 CI 通过，再在干净的修复提交上运行 108 次策略对照。
`yield` 和 `jitter` 各 36/36 次有效；立即重试为 31/36，其余 5 次触发 retry guard。
20T、50R/50W、16 keys 下，jitter 为 **930,577 ± 21,732 TPS**，3/3 次有效。
退避仍能改善本轮热点负载的完成情况；部分配置低于修复前记录，具体差异保留在下文。

## 版本、CI 与测量条件

测量提交为 [`9bccd33aa23ad965fa0b4a82230f73c32dc67f5f`](https://github.com/ZephyrVia/CaSTM/commit/9bccd33aa23ad965fa0b4a82230f73c32dc67f5f)。
构建和启动矩阵时工作区均干净；测量后逐项核对 metadata 中的 95 个源码/构建/脚本文件，
哈希与该提交一致。二进制 SHA-256：
`c23a5103e5e1b709db1686098c08b0010894ff034fdb79816169808a28da838b`。
本报告随后以文档提交归档，测量版本始终以上述源码提交为准。

[远端 CI #8](https://github.com/ZephyrVia/CaSTM/actions/runs/41768629427) 的四个 job 均已完成并成功：

| job | 结果 |
| --- | --- |
| Build and test (WwSTM mode=0) | success |
| Build and test (VERIFY) | success |
| ASan and UBSan | success |
| OccSTM history and reclamation (TSan) | success |

[CI 核验记录](../validation/occ-reclamation-2026-11-04/ci.json)保存精确提交与各 job 链接。
TSan 仅覆盖 system allocator 下的 20 个 OccSTM 用例，重复 10 轮；ASan 关闭泄漏检测。
本地测试数量、跳过及禁用范围仍见[测试说明](../testing.md)，不由性能运行补充推断。

| 参数 | 本轮设置 |
| --- | --- |
| 环境 | i5-14600KF，20 logical CPUs，GCC 11.4.0，WSL2 / Linux 6.18.40.1 |
| 构建 | Release，-O3，VERIFY=0，TEST_HOOKS=0，无 sanitizer、无 -march=native |
| 后端与分配器 | OccSTM；native，VersionNode 使用 TierAlloc |
| 策略 | none / yield / jitter，同一二进制 |
| 线程 | 1 / 20 |
| 操作读写比 | 90R/10W、50R/50W、10R/90W |
| key 数 / 操作数 | low 65,536 / high 16；每事务 8 个操作 |
| 预热 / 测量 | 每进程 300 ms / 1,500 ms |
| 重复与顺序 | 每配置 3 个独立进程，同一 workload/repetition 内轮换策略顺序 |
| seed | 3394748143；重试重放同一操作批次 |
| 尝试上限 / timeout | 每逻辑事务 1,000,000 次尝试 / 每进程 40 秒 |

本轮沿用[修复前诊断实验](occ-retry-diagnostics.md)的配置，没有提高重试上限。
测量期间未并行执行本地构建或测试；未固定 CPU 亲和性或频率，未记录调度轨迹。
三种策略共用同一套诊断计数，策略间比较均来自本轮修复后的同一二进制。

## 完成情况与失败证据

| 策略 | 有效 / 总运行 | retry_limit | checksum mismatch |
| --- | ---: | ---: | ---: |
| none | 31/36 | 5 | 0 |
| yield | 36/36 | 0 | 0 |
| jitter | 36/36 | 0 | 0 |
| 合计 | 103/108 | 5 | 0 |

5 次失败全部来自 20 线程、16 keys 的立即重试：

| workload | 有效 | 失败阶段 | 读锁冲突 / 全部 abort |
| --- | ---: | --- | ---: |
| 90R/10W | 3/3 | 无 | 99.917% |
| 50R/50W | 0/3 | 预热 1 次、测量 2 次 | 99.949% |
| 10R/90W | 1/3 | 测量 2 次 | 97.659% |

原因比例合并同组 3 次运行的预热与测量计数，包括失败进程。
50R/50W 共记录 36,893,354 次 read_locked、18,753 次 read_changed 和 18 次
snapshot_unavailable。5 次失败所在阶段均记录到单逻辑事务 999,999 次重试，
对应原定 1,000,000 次尝试；没有 timeout、signal 或异常失败。

链边修复解决已复现的数据竞争，立即重试的进展问题仍然存在。10R/90W 的一次成功
也保留在 raw/summary 中，不用该幸存样本计算正常吞吐或速度比。所有运行的最终
checksum 匹配，包括失败进程；这只验证 workload 的最终总和不变量。

## 修复后的热点性能

以下均为 20 线程、16 keys。TPS 为 3 次有效运行的均值 ± 样本标准差，abort rate
和 p99 为各运行指标的均值。未满 3 次有效的配置标为 N/A，全部样本仍保留在 CSV。
延迟每 worker 每 256 次成功提交采样一次，包含内部重试，不包含失败逻辑事务。

| workload | 策略 | 有效 | TPS | abort rate | p99（μs） |
| --- | --- | ---: | ---: | ---: | ---: |
| 90R/10W | none | 3/3 | 38,961 ± 1,637 | 99.827% | 5,039.08 |
| 90R/10W | yield | 3/3 | 703,801 ± 101,477 | 96.249% | 439.38 |
| 90R/10W | jitter | 3/3 | 2,928,609 ± 31,257 | 26.428% | 154.48 |
| 50R/50W | none | 0/3 | N/A | N/A | N/A |
| 50R/50W | yield | 3/3 | 28,175 ± 7,733 | 99.765% | 8,499.31 |
| 50R/50W | jitter | 3/3 | 930,577 ± 21,732 | 39.015% | 446.65 |
| 10R/90W | none | 1/3 | N/A | N/A | N/A |
| 10R/90W | yield | 3/3 | 144,882 ± 10,536 | 96.660% | 2,018.14 |
| 10R/90W | jitter | 3/3 | 740,034 ± 22,790 | 32.574% | 682.54 |

20T、50R/50W、16 keys 下，jitter 三次测量的单逻辑事务最大重试为 **67、87、83**；
yield 为 **386,486、182,731、205,315**。jitter 的每个 worker 在每次测量中均有
提交，跨三次运行的 worker 提交数范围为 **64,312～75,877**。这些是有限窗口的
进展证据，不是无饥饿保证。首次失败原因计数也不表示锁等待时间或全部冲突条件。

## 低冲突与单线程

以下均为 65,536 keys，none/jitter 各 3/3 次有效。变化为本轮 jitter 相对 none；
各配置标准差见 summary。所有单线程运行在预热及测量阶段均无 abort，因此其中
的吞吐差异不能归因于实际执行退避等待。

| 线程 | workload | none 平均 TPS | jitter 平均 TPS | 变化 |
| --- | --- | ---: | ---: | ---: |
| 1 | 90R/10W | 1,469,473 | 1,404,730 | -4.41% |
| 1 | 50R/50W | 408,116 | 416,212 | +1.98% |
| 1 | 10R/90W | 397,479 | 411,184 | +3.45% |
| 20 | 90R/10W | 2,439,879 | 2,429,595 | -0.42% |
| 20 | 50R/50W | 1,104,541 | 1,088,659 | -1.44% |
| 20 | 10R/90W | 637,246 | 612,458 | -3.89% |

本轮 20 线程低冲突的三个点均未显示 jitter 吞吐收益，不能沿用上一轮的正收益
范围描述当前版本。三次短时样本也不足以证明小幅差异显著或策略性能等价。

## 与修复前记录的对照

下面完整列出 jitter 的 12 个配置，均为各自批次 3/3 次有效的 TPS 均值。
旧批次在修复前单独运行，包含后来确认的普通 prev 指针数据竞争；本轮没有交替
重跑两个版本。以下只展示历史数值差异，**不是原子链边修复成本的受控 A/B**。

| 线程 | workload | 冲突 | 修复前 TPS | 本轮 TPS | 差异 |
| --- | --- | --- | ---: | ---: | ---: |
| 1 | 90R/10W | low | 1,306,861 | 1,404,730 | +7.49% |
| 1 | 50R/50W | low | 547,807 | 416,212 | -24.02% |
| 1 | 10R/90W | low | 363,011 | 411,184 | +13.27% |
| 1 | 90R/10W | high | 5,219,693 | 5,019,506 | -3.84% |
| 1 | 50R/50W | high | 2,248,224 | 2,114,818 | -5.93% |
| 1 | 10R/90W | high | 1,782,158 | 1,670,962 | -6.24% |
| 20 | 90R/10W | low | 2,325,673 | 2,429,595 | +4.47% |
| 20 | 50R/50W | low | 1,164,042 | 1,088,659 | -6.48% |
| 20 | 10R/90W | low | 691,660 | 612,458 | -11.45% |
| 20 | 90R/10W | high | 3,010,872 | 2,928,609 | -2.73% |
| 20 | 50R/50W | high | 973,364 | 930,577 | -4.40% |
| 20 | 10R/90W | high | 829,395 | 740,034 | -10.77% |

其中 1T、50R/50W、low 的下降需要保留为后续性能排查项：本轮 jitter 为
416,212 ± 23,547 TPS，旧批次为 547,807 ± 10,422 TPS。不能没有进一步证据就将
差异归为调度噪声，也不能全部归因于 atomic。后续应固定运行条件、增加重复并
分析热点；本轮不为追求数字而更改已通过 CI 的修复。

## 原始证据与复现

- [raw.csv](data/occ-postfix-2026-11-04/raw.csv)：全部 108 行，保留 5 次失败。
- [summary.csv](data/occ-postfix-2026-11-04/summary.csv)：36 组汇总，仅对有效运行计算统计量。
- [metadata.txt](data/occ-postfix-2026-11-04/metadata.txt)、[provenance.json](data/occ-postfix-2026-11-04/provenance.json)：环境、参数、提交、CI、二进制与源码哈希。
- [逐线程诊断](data/occ-postfix-2026-11-04/diagnostics.jsonl.gz)：每行 `{path, data}`，path 对应 raw.csv 的 diagnostics_file。
- [runner 日志](data/occ-postfix-2026-11-04/runner.txt)：运行顺序、有效性和终止原因。

在该测量提交的独立 checkout 中，从仓库根目录执行：

~~~bash
cmake -S . -B /tmp/castm-occ-postfix-bench \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0"
cmake --build /tmp/castm-occ-postfix-bench --target stm_benchmark -j2

python3 scripts/run_retry_ablation.py \
  --binary /tmp/castm-occ-postfix-bench/benchmarks/stm_benchmark \
  --output-dir bench/results/occ-postfix-ablation \
  --threads 1,20 --policies none,yield,jitter --repetitions 3 \
  --warmup-ms 300 --measure-ms 1500 --max-attempts 1000000 --timeout-seconds 40
~~~

每次选择尚不存在的结果目录。runner 遇到无效运行仍保存结果，并返回 1；本轮
退出码 1 对应上述 5 次 retry_limit。失败位置与数量可能随线程调度改变。
在包含本报告的 checkout 中可离线复核归档：

~~~bash
python3 scripts/verify_retry_report.py docs/performance/data/occ-postfix-2026-11-04
~~~

本快照输出 `runs=108`、`valid=103`、`retry_limit=5`。已检查文件哈希、汇总重算、
CSV/JSON 对应、阶段/逐线程计数、重试直方图、checksum 和失败时的 retry guard。

## 结论边界

本轮测量的是 benchmark 的手动事务循环；`STM::atomically` 共享退避实现，未单独
量化其端到端吞吐。benchmark 默认策略仍是 none，需要显式选择 jitter。
没有重新运行四后端或 allocator 矩阵，不据此声称优于 mutex/WwSTM 或存储服务。

短时 checksum、CI 和有限交错测试不能证明完整隔离级别、所有内存模型下的 EBR
正确性或无泄漏。退避时仍持有 EBR guard，慢读者导致的退休积压、长时 RSS 和
线程 churn 尚未测量，仍是下一阶段的验证重点。
