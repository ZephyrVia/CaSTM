# CaSTM 性能实验报告

整理日期：2026-11-04。本报告归档已有实验，重新从原始 CSV 核对统计结果；
本轮没有重跑 432 次主矩阵或 60 次分配器对照。短时运行检查单列于
[测试记录](../testing.md)，不混入以下数据。

新增的 OccSTM 失败分类、108 次退避对照和原始逐线程计数见
[诊断与退避报告](occ-retry-diagnostics.md)，与本页历史数据分开保存。

## 数据与来源

| 实验 | 原目录时间戳（UTC） | 源码提交 | 原始运行 | 汇总配置 |
| --- | --- | --- | ---: | ---: |
| 四后端矩阵 | 20260903-130419 | 4c47011a7cfd6a978772d8cfa80f273dd6ca4981 | 432 | 144 |
| allocator ablation | 20260904-061709 | 6998448493cc4b4593a01fdabc1246a52b223856 | 60 | 12 |

仓库内可直接检查以下快照：

- [主矩阵 raw.csv](data/stm-raw.csv)、[summary.csv](data/stm-summary.csv)
- [分配器 raw.csv](data/allocator-raw.csv)、[summary.csv](data/allocator-summary.csv)
- [来源、原参数和 SHA-256](data/provenance.json)

四个 CSV 按字节复制自已有本地结果，保留无效运行，没有重新筛选原始数据。
provenance 保留原实验 metadata，仅省略宿主绝对 binary/result 路径。
主矩阵与分配器实验使用不同提交和不同 allocator 配置，不能混拼为同一次运行。

## 环境与 workload

原实验环境为 Intel Core i5-14600KF、20 logical CPUs、GCC 11.4.0、
WSL2 / Linux 6.18.33.2-microsoft-standard-WSL2。构建为 Release、-O3、NDEBUG，
WwSTM VERIFY=0、TEST_HOOKS=0，未启用 -march=native。报告只对该环境下的测量负责。

| 参数 | 主矩阵 |
| --- | --- |
| 后端 | std::mutex、std::shared_mutex、OccSTM、WwSTM |
| 线程 | 1、2、4、8、16、20 |
| 每逻辑事务操作数 | 8 |
| 读写比例 | 90R/10W、50R/50W、10R/90W |
| key 数 | low：65,536；high：16 |
| 预热 / 测量 | 1 秒 / 5 秒 |
| 重复 | 每配置 3 个独立进程 |
| 随机种子 | 3394748143（0xCA57BEEF） |
| 外层 retry guard | 每逻辑事务最多 1,000,000 次尝试 |

key 均匀采样；操作批次由 run、worker、logical_id 确定，失败重试重放同一批次。
写操作是 read-modify-write：读取整数后写入 value + 1。它是短事务内存微基准，
不包含网络、磁盘或业务计算；TPS 表示已提交逻辑事务数，不等于请求 QPS 或单次操作数。

mutex 在整个逻辑事务期间持有全局排他锁。shared_mutex 仅对全读批次使用共享锁，
只要批次含写就使用排他锁；90% 的操作为读不意味着 90% 的事务使用共享锁。
STM 后端使用各自的事务接口和重试循环。

主矩阵保留原生分配路径：OccSTM VersionNode 使用 TierAlloc，WwSTM
VersionNode/WriteRecord 使用系统堆。因此它比较的是完整后端实现，不能将全部差异
仅归因于 OCC 与 Wound-Wait 算法。后续 allocator ablation 才固定后端切换分配路径。

## 指标与有效性

- throughput：已提交逻辑事务数 / 实际测量秒数；结束时等待在途逻辑事务完成。
- abort rate：失败尝试 / 尝试总数；CSV 中是 0～1 的比例，表格转换为百分比。
- latency：包含一个成功逻辑事务内部的重试，排除批次构造；每线程每 256 次成功提交
  抽样一次。单次运行合并样本后计算 p50/p99，summary 再汇总各运行的分位数。
- checksum：最终所有 key 的和应等于成功提交的写操作数，包含预热阶段。
  read_checksum 用于保留读操作的可观察结果，不作为后端间必须相等的值。
- summary 只对 valid=1 的运行计算均值与样本标准差；无有效样本时字段留空。
  无效运行保留在 raw 中，图表用 N/A，不插值、不用零替代。

原始 CSV 将 TPS/时间及 abort rate 输出到有限小数位；报告按保存的数据重新汇总，
不假设还有更高精度原始样本。checksum 匹配验证该 workload 的总和不变量，
不等于证明所有并发历史均满足某种隔离级别。

## 主矩阵结果

| 后端 | 有效 / 总运行 | 无效 | 最终 checksum mismatch |
| --- | ---: | ---: | ---: |
| std::mutex | 108 / 108 | 0 | 0 |
| std::shared_mutex | 108 / 108 | 0 | 0 |
| OccSTM | 97 / 108 | 11 | 0 |
| WwSTM | 108 / 108 | 0 | 0 |
| 合计 | 421 / 432 | 11 | 0 |

主矩阵有 144 个配置，每个配置 3 次运行。108/108 指运行次数，不能称为
108 种不同配置，也不是无饥饿保证。

### 高冲突代表点

以下均为 20 线程、16 keys、8 操作/事务。TPS 单位为千事务/秒，p99 单位为微秒；
数值是有效运行的均值，逐项标准差见原始 summary。

| 读写比例 | 后端 | TPS（k） | abort rate | p99（µs） | 有效 / 重复 |
| --- | --- | ---: | ---: | ---: | ---: |
| 90R/10W | mutex | 4,236.5 | 0% | 44.85 | 3 / 3 |
| 90R/10W | shared_mutex | 1,828.1 | 0% | 316.61 | 3 / 3 |
| 90R/10W | OccSTM | 24.5 | 99.9% | 12,222.49 | 3 / 3 |
| 90R/10W | WwSTM | 1,215.3 | 12.5% | 249.63 | 3 / 3 |
| 50R/50W | mutex | 3,731.6 | 0% | 49.04 | 3 / 3 |
| 50R/50W | shared_mutex | 3,236.1 | 0% | 62.68 | 3 / 3 |
| 50R/50W | OccSTM | N/A | N/A | N/A | 0 / 3 |
| 50R/50W | WwSTM | 336.5 | 70.4% | 649.36 | 3 / 3 |
| 10R/90W | mutex | 4,077.7 | 0% | 46.28 | 3 / 3 |
| 10R/90W | shared_mutex | 3,776.3 | 0% | 45.18 | 3 / 3 |
| 10R/90W | OccSTM | N/A | N/A | N/A | 0 / 3 |
| 10R/90W | WwSTM | 206.5 | 80.7% | 882.73 | 3 / 3 |

WwSTM 在这些高冲突配置中比 OccSTM 更稳定地完成，但仍有明显的 abort 与尾延迟成本。
锁基线在表中保持更高吞吐；不能把 WwSTM 对 OccSTM 某个压力点的优势推广为对锁的优势。

### 低冲突扩展性

以 50R/50W、65,536 keys 为例，吞吐单位为千事务/秒：

| 后端 | 1T | 8T | 20T |
| --- | ---: | ---: | ---: |
| mutex | 8,629.1 | 3,800.3 | 2,698.9 |
| shared_mutex | 8,576.2 | 3,687.4 | 2,458.4 |
| OccSTM | 709.0 | 1,202.8 | 1,214.3 |
| WwSTM | 1,131.8 | 497.4 | 659.2 |

这组实验没有展示近线性扩展；WwSTM 多线程吞吐低于自身单线程值。
额外的版本对象、验证、冲突处理和回收路径可能贡献开销，但尚无事件级 profiling
证明各项占比，也不能把 WSL2 或线程调度直接定为唯一原因。

### 无效运行

主矩阵的 11 次无效运行均属于 OccSTM：

| 线程 | 读写比例 | 冲突 | 有效 / 重复 |
| --- | --- | --- | ---: |
| 16 | 50R/50W | high | 0 / 3 |
| 16 | 10R/90W | high | 1 / 3 |
| 20 | 50R/50W | high | 0 / 3 |
| 20 | 10R/90W | high | 0 / 3 |

raw 中这些行的 error 都是 exit code 1，最终 checksum 均匹配。既有分析将其
解释为 1,000,000 次 retry guard 耗尽的进展失败。本次可直接复核的事实是
“运行无效且没有最终 checksum mismatch”；harness 也会把其他异常标为 invalid，
原 CSV 没有逐次失败原因计数或异常栈，因此不能只凭退出码排除所有其他失败路径。
不完整配置的有效样本均值仍可在 summary 中检查，但不用于宣称稳定的正常性能。

## 分配器对照

实验固定同一 workload、后端和编译选项，仅切换 benchmark 的 system/tier 分配路径。
切换对象主要是 OccSTM VersionNode 与 WwSTM VersionNode/WriteRecord；不包括
WwSTM TxDescriptor、EBR GarbageNode 元数据、std::vector 缓冲区等全部进程分配。

六个代表配置 × 两种 allocator × 五个独立进程 = 60 次运行；
57 次有效，3 次无效，最终 checksum mismatch 为 0。

| 配置 | system TPS（k） | tier TPS（k） | 吞吐变化 | p99 变化 | system / tier 有效次数 |
| --- | ---: | ---: | ---: | ---: | --- |
| OccSTM，1T，50R/50W，low | 541.3 | 639.9 | +18.2% | -12.7% | 5/5、5/5 |
| WwSTM，1T，50R/50W，low | 1,103.2 | 1,274.2 | +15.5% | -53.1% | 5/5、5/5 |
| OccSTM，8T，50R/50W，low | 916.9 | 1,108.5 | +20.9% | -13.3% | 5/5、5/5 |
| WwSTM，8T，50R/50W，low | 505.1 | 658.0 | +30.3% | -15.4% | 5/5、5/5 |
| OccSTM，8T，50R/50W，high | N/A | N/A | N/A | N/A | 2/5、5/5 |
| WwSTM，20T，10R/90W，high | 206.5 | 183.7 | -11.0% | +11.3% | 5/5、5/5 |

变化为 tier 相对 system 的变化，p99 下降表示该分位延迟降低。
比值仅在两侧 5/5 有效时展示。OccSTM 8T high 的原始有效样本仍保留，但
system 一侧不完整，不能以幸存样本均值计算可宣传的加速比。

TierAlloc 在四个低冲突配置有收益，高冲突 WwSTM 则出现退化。后者与冲突管理、
调度或分配改变交错有关是待验证解释，不能写成已经定位出的性能根因。

## 图表

以下图表来自同一历史主矩阵，命名中的 10R90W 表示 10% 读、90% 写：

![10R/90W low throughput](../images/benchmark/throughput-10r90w-low.png)

![50R/50W high throughput](../images/benchmark/throughput-50r50w-high.png)

![50R/50W high abort rate](../images/benchmark/abort-rate-50r50w-high.png)

![10R/90W high p99 latency](../images/benchmark/p99-10r90w-high.png)

## 复核与重新运行

从仓库根目录执行，使用现有汇总函数即可复核两组快照：

~~~bash
PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
import csv
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, "scripts")
import summarize_stm_benchmark as stm
import run_allocator_ablation as allocator

root = Path("docs/performance/data")
manifest = json.loads((root / "provenance.json").read_text())
for experiment in manifest["experiments"]:
    for name, info in experiment["files"].items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == info["sha256"]
for name, module in [("stm", stm), ("allocator", allocator)]:
    with (root / f"{name}-raw.csv").open(newline="") as f:
        raw = list(csv.DictReader(f))
    with (root / f"{name}-summary.csv").open(newline="") as f:
        saved = list(csv.DictReader(f))
    assert module.summarize_rows(raw) == saved
    assert all(r["final_checksum"] == r["expected_checksum"] for r in raw)
    print(name, "runs =", len(raw), "invalid =", sum(r["valid"] != "1" for r in raw))
PY
~~~

构建、完整 runner 和 allocator ablation 命令见
[benchmark 使用说明](../../benchmarks/README.md)。重新测量会产生独立结果目录；
应记录新提交和机器参数，不覆盖这里的历史快照，也不预设会复现相同吞吐数字。
