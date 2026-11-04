# OccSTM 高冲突诊断与退避对照

记录日期：2026-11-04，分支 `codex/perf-diagnostics`，基线标签
`v0.1.0-prototype`。本轮先补失败分类和重试统计，再比较三种重试策略。
结果支持：在本机热点 workload 中，立即重试会放大读锁冲突；有界随机退避显著
降低重试量并改善完成情况，同时保留了单线程的少量吞吐下降。

本报告是一次新的 OccSTM 实验，与[历史四后端及分配器矩阵](README.md)分开。
新结果不追溯证明历史 11 次无效运行的具体原因，也不用于更新历史 CSV。

## 先修复了什么统计缺口

旧 benchmark 在检查到失败后直接退出逻辑事务循环，未将该逻辑事务的
attempts/aborts 累计到结果；预热阶段没有失败细节，异常也只记为 invalid。
这会漏掉最需要解释的最后一组重试。

当前实现将计数放在失败检查之前，分开保存 warmup/measure 和每个 worker。
OccSTM 的首次检测失败分为：

| 原因 | 检测位置 | 含义 |
| --- | --- | --- |
| `snapshot_unavailable` | `Transaction::load` | 保留的版本链中没有满足读时间戳的版本 |
| `read_locked` | 读集验证的锁检查 | 读条带被其他事务持有，且不在本事务已持有的写锁集合中 |
| `read_changed` | 读集 head identity 验证 | 当前 head 与读时记录的节点不同 |

终止原因区分 `retry_limit`、捕获到的分配/其他异常、checksum mismatch，runner
另行区分 timeout、signal、非零退出、启动失败和 CSV 解析失败。计数只记录首次
检测到的失败条件，不能据此断言同次尝试没有其他冲突，也没有测量锁等待时长。
`read_locked` 针对条带，不能单靠此计数排除不同 key 映射到同一条带的影响。

重试分布覆盖成功与最终失败的逻辑事务，重试数为 `attempts - 1`；直方图分位数
是区间上界。延迟沿用每 worker 每 256 次成功提交采样一次，包含内部重试，
不包含失败逻辑事务。字段及边界详见[benchmark 说明](../../benchmarks/README.md#retry-diagnostics-and-occstm-backoff)。

## 对照方法与来源

- 环境：Intel Core i5-14600KF，20 logical CPUs，GCC 11.4.0，WSL2，
  Linux 6.18.40.1-microsoft-standard-WSL2。
- 同一个 Release 二进制，`-O3`、VERIFY=0、TEST_HOOKS=0、无 `-march=native`；
  分配模式 native，OccSTM VersionNode 使用 TierAlloc。
- 1 / 20 线程；90R/10W、50R/50W、10R/90W；65,536 / 16 个 key；每事务 8 个操作。
- 每进程预热 300 ms、测量 1,500 ms；每配置 3 个独立进程。
- 三种策略共 108 次运行；同一 workload/repetition 内轮换策略执行顺序。
- seed 固定为 `3394748143`，逻辑重试重放同一操作批次。
- 重试上限始终为 1,000,000 次尝试，进程 timeout 为 40 秒。
- 性能矩阵期间没有并行构建或测试；没有固定 CPU 亲和性、频率或做调度追踪。

测量时工作区尚未提交，HEAD 是 `22d00aa13cf554896a0441578102010bf077d776`。
不能只用该 HEAD 复现修改后的实现；以下 metadata 保存了实际二进制 SHA-256、
dirty 标记和逐源码文件 SHA-256，归档 provenance 保存构建与运行命令。

- [raw.csv](data/occ-retry-2026-11-04/raw.csv)：保留全部 108 行，包括 6 行失败。
- [summary.csv](data/occ-retry-2026-11-04/summary.csv)：36 组策略/配置汇总。
- [metadata.txt](data/occ-retry-2026-11-04/metadata.txt)、[provenance.json](data/occ-retry-2026-11-04/provenance.json)。
- [完整逐线程诊断压缩包](data/occ-retry-2026-11-04/diagnostics.jsonl.gz)：每行一个
  `{path, data}`，path 对应 raw.csv 的 `diagnostics_file`，data 是原 JSON 对象。

## 高冲突失败的直接证据

`none` 共 30/36 次有效；`yield` 和 `jitter` 均为 36/36 次有效。
全部 108 次最终 checksum 均匹配，所有 6 次无效运行均明确为 `retry_limit`，
没有 timeout、signal 或异常记录：

| 立即重试场景 | 有效 | 失败阶段 | 读锁冲突占全部 abort 的比例 |
| --- | ---: | --- | ---: |
| 20T，90R/10W，16 keys | 3/3 | 无 | 99.915% |
| 20T，50R/50W，16 keys | 0/3 | 全部在预热 | 99.940% |
| 20T，10R/90W，16 keys | 0/3 | 全部在测量 | 96.380% |

比例将同组 3 次进程的预热及测量计数相加后计算，包含失败进程。
50R/50W 的测量计数为零，因为预热已经失败，不能把零解读为没有重试：其预热
合计 **13,100,874 次读锁冲突、7,815 次 head 变化、2 次快照不可用**。
三个失败运行都记录到一个逻辑事务的 1,000,000 次尝试，即 999,999 次重试。

代码路径是：按条带序获取写锁 → 验证读集 → 检测他人持有的读条带 → 释放写锁
并失败。旧 benchmark 随即进入下一次尝试。计数与下面的退避对照共同支持
“密集重试放大锁竞争”的解释；没有调度轨迹，因此不声称已经证明每个线程之间
的具体交错，也不将 lock-wait 或内存开销精确分摊到 CPU 时间。

## 退避实现与结果

`none` 保持立即重试；`yield` 在每次失败后让出调度；`jitter` 在前两次失败后
yield，之后从 0 到 1、2、4、8、16、32、64 微秒的窗口中随机取延迟，窗口上限
为 64 微秒，取到零时 yield。操作系统实际调度延迟可能超过请求值。

随机状态每线程独立，不使用全局 RNG；每个逻辑事务重新计算连续失败次数。
退避发生在写锁释放之后，仅在还有下一次尝试时执行，不提高重试上限。
benchmark 的默认仍为 `none`，通过 `--backoff jitter` 选择优化。
`STM::atomically` 默认使用同一 jitter 实现，可用第二参数选择其他策略。
两条路径都保留原有的跨重试 EBR guard。

以下均为 20 线程、16 keys。TPS 是 3 个有效进程的均值 ± 样本标准差；
abort rate 和 p99 是各有效进程指标的均值。`N/A` 不参与速度比计算。

| workload | 策略 | 有效 | TPS | abort rate | p99（μs） |
| --- | --- | ---: | ---: | ---: | ---: |
| 90R/10W | none | 3/3 | 45,463 ± 17,036 | 99.801% | 6,690.00 |
| 90R/10W | yield | 3/3 | 705,502 ± 33,943 | 96.037% | 379.53 |
| 90R/10W | jitter | 3/3 | 3,010,872 ± 110,323 | 25.960% | 144.58 |
| 50R/50W | none | 0/3 | N/A | N/A | N/A |
| 50R/50W | yield | 3/3 | 24,430 ± 3,459 | 99.795% | 8,306.15 |
| 50R/50W | jitter | 3/3 | 973,364 ± 40,372 | 38.077% | 417.62 |
| 10R/90W | none | 0/3 | N/A | N/A | N/A |
| 10R/90W | yield | 3/3 | 132,902 ± 28,642 | 96.947% | 2,241.74 |
| 10R/90W | jitter | 3/3 | 829,395 ± 35,188 | 31.723% | 563.28 |

20T、50R/50W、16 keys 中，jitter 的 3 次测量窗口内，单逻辑事务最大重试次数
分别为 **59、50、56**；yield 则为 **169,134、202,226、368,651**。
同一组 jitter 运行中，每个 worker 都有成功提交，跨 3 次运行的 worker 完成数
范围为 65,491～80,009；这是有限测量窗口的进展证据，不是无饥饿保证。

## 低冲突和单线程开销

以下均为 65,536 keys，none/jitter 各 3/3 次有效。退避只在失败后等待，单线程
没有 abort；单线程的少量差异不应直接归因于睡眠开销，也不能从 3 次样本认定
性能完全等价。

| 线程 | workload | none 平均 TPS | jitter 平均 TPS | 变化 |
| --- | --- | ---: | ---: | ---: |
| 1 | 90R/10W | 1,342,283 | 1,306,861 | -2.64% |
| 1 | 50R/50W | 556,206 | 547,807 | -1.51% |
| 1 | 10R/90W | 371,964 | 363,011 | -2.41% |
| 20 | 90R/10W | 2,313,919 | 2,325,673 | +0.51% |
| 20 | 50R/50W | 1,010,853 | 1,164,042 | +15.15% |
| 20 | 10R/90W | 652,979 | 691,660 | +5.92% |

本轮保留 jitter 作为 `atomically` 的默认重试策略，但仍提供 None/Yield，以便
应用按自己的冲突强度复测。本报告测量的是 benchmark 的手动事务循环；公共
`atomically` 共享退避实现并有功能回归，尚未单独量化其端到端吞吐。

## 验证与边界

- Release / mode=0：57 个通过、1 个 disabled。
- Debug / VERIFY：50 个通过、7 个 skipped、1 个 disabled。
- Debug / ASan+UBSan：57 个通过、1 个 disabled，leak detection 关闭。
- Python：9 个 unittest 方法通过，包括四后端计数/JSON/CSV 对账。
- 归档复核检查 SHA-256、汇总重算、逐线程和阶段计数、直方图、checksum，以及
  失败逻辑事务确实到达原定 retry guard；[验证记录](../validation/2026-11-04-perf-diagnostics.json)。

本轮没有修改提交协议、锁表、版本链剪枝或分配器。退避期间仍持有 EBR guard，
可能推迟回收；未测长期 RSS、线程 churn 或尾部最差延迟。OccSTM 历史链 `prev`
仍是普通指针，读者遍历与并发剪枝之间的访问需要单独审计；本轮没有 TSan 结论，
ASan/UBSan 通过不能替代该项验证，也不构成完整隔离级别的证明。

样本来自短事务、单机 WSL2、固定均匀热点分布，不能推广到业务服务 QPS、磁盘
存储性能或所有机器。没有重新测四后端矩阵，不能据此声称超过 mutex/WwSTM。
所有策略都有相同的诊断计数开销；不把新 none 的绝对 TPS 与历史未诊断版本
直接相减作为优化收益。

## 复现与离线复核

~~~bash
cmake -S . -B /tmp/castm-perf-diagnostics \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0"
cmake --build /tmp/castm-perf-diagnostics --target run_tests stm_benchmark -j2
ctest --test-dir /tmp/castm-perf-diagnostics --output-on-failure --timeout 60

python3 scripts/run_retry_ablation.py \
  --binary /tmp/castm-perf-diagnostics/benchmarks/stm_benchmark \
  --output-dir bench/results/occ-retry-ablation \
  --threads 1,20 --policies none,yield,jitter --repetitions 3 \
  --warmup-ms 300 --measure-ms 1500 --max-attempts 1000000 --timeout-seconds 40

python3 scripts/verify_retry_report.py docs/performance/data/occ-retry-2026-11-04
~~~

runner 在包含无效运行时返回 1，并仍写出完整结果。已存在的输出目录不会被覆盖。
离线复核本快照应输出 `runs=108`、`valid=102`、`retry_limit=6`。
