# TierAlloc 中等对象与线程堆复用收尾验收（2026-11-17）

> 验证对象：主线候选代码（尚未覆盖 `v0.2.0-prototype` 标签）。
> 这是明确场景下的稳定性与内存验证，不构成无界内存增长、生产就绪或全仓库无数据竞争的证明。

## 改造内容

- **MediumArena**：对 >256 KiB、≤2044 KiB 的对象，在独立的进程级 Arena 中通过 2 MiB 对齐 Chunk、4 KiB Page-Run 与 Bitmap 实现共享，避免中等对象独占 2 MiB；大对象通过完整 2 MiB 映射长度对齐并记录实际映射大小。
- **ThreadHeapDepot + ThreadHeapLease**：ThreadHeap/SizeClassPool 的存储由进程级 Depot 持续持有，TLS 只持有独占 Lease。线程退出时将整个 ThreadHeap 放入空闲 LIFO，新线程再认领；`Slab::owner_` 地址不变，已有活跃对象不被覆盖。
- 不在 TLS 退出时向 CentralHeap 归还任何可能含活对象或正接受远程释放的 Slab，避免复现 [BUG-01](../../BUG-01-ThreadHeap线程退出归还活跃Chunk.md)。
- Depot 的锁只用于认领和归还，不在已经认领的 ThreadHeap 小对象快速路径中新增锁。

[MediumArena PR #1](https://github.com/ZephyrVia/CaSTM/pull/1) /
[ThreadHeapDepot PR #2](https://github.com/ZephyrVia/CaSTM/pull/2) /
[所有权转移架构](../architecture/threadheap-depot.md)

## 同环境 90 秒 A/B

来源：[GitHub Actions A/B CI](https://github.com/ZephyrVia/CaSTM/actions/runs/44046838877) /
[原始数据 artifact](https://github.com/ZephyrVia/CaSTM/actions/runs/44046838877/artifacts/14312176252)。

两组分别使用相同的 OccSTM/EBR 测试程序、相同 20 并发 worker、60 批次、共 1,200 次线程创建退出。每组完成 2,400,000 笔事务、1,200,000 次写入，1,199,840 次版本退休与回收，checksum 和对象析构计数均匹配。基线是 MediumArena 改造后、Depot 改造前的提交 `31c8902`；候选运行时代码是 `0f256d6`。

| 指标 | 无 Depot | 有 Depot |
|---|---:|---:|
| VmSize 首次样本 (MiB) | 1424.01 | 1424.00 |
| VmSize 最末样本 (MiB) | 3784.01 | 1424.00 |
| VmSize 增量 (MiB) | +2360 | **0** |
| RSS 首次样本 (MiB) | 70.70 | 70.83 |
| RSS 最末样本 (MiB) | 122.59 | 75.02 |
| Heap 创建数（含主线程） | 未采集 | **21** |
| Heap 重新认领次数 | 不适用 | **1180** |

## 10 分钟真实事务 / EBR churn 验收

来源：[10 分钟 CI 运行](https://github.com/ZephyrVia/CaSTM/actions/runs/44048564666) /
[原始 CSV/JSON artifact](https://github.com/ZephyrVia/CaSTM/actions/runs/44048564666/artifacts/14313416288)。

- 实验仓库提交：`2e321a77362f22d8d8af73bafc4435e2d59cb7d2`（只增添测试仪表化与 CI，不修改 Depot、STM、EBR 的运行时代码）；候选运行时代码 `0f256d6`。
- GitHub-hosted Ubuntu 22.04 / Release / native TierAlloc；20 worker 并发、每 5 秒创建退出一批、每 worker 2,000 次事务、120 批次，总计 2,400 worker。
- 每批线程退出后做 quiescent checksum 与版本回收对账；每秒采集 `/proc/self/status` 中的 VmSize/RSS；同时采集 Depot 创建、借出、闲置及复用计数。
- 运行 `600.000574` 秒，**严格校验全部通过**。

| 验证项 | 结果 |
|---|---:|
| 事务总数 | **4,800,000** |
| 成功写入 = checksum | **2,400,000 = 2,400,000** |
| 退休版本 = 回收版本 | **2,399,840 = 2,399,840** |
| 创建对象 = 析构对象 | **2,400,020 = 2,400,020** |
| Depot ThreadHeap 创建数（首批 → 末批） | **21 → 21** |
| 累计重新认领次数 | **2,380** |
| 最终已借出 / 闲置 Heap | **1 / 20** |
| VmSize 首批 → 末批 (MiB) | **1423.996 → 1423.996** |
| RSS 首批 → 末批 (MiB) | **70.94 → 77.53** |
| RSS 采样峰值 (MiB) | **78.55** |

结果说明：在该负载与时长下，ThreadHeap 创建总量按并发高水位稳定，而不是随累计线程创建次数线性增长；所有事务和版本回收断言通过。实验期间 VmSize 没有增长，但 RSS 仍存在少量增长，不意味着物理内存完全归还操作系统。

## 正确性与局限

- 功能分支与对应 PR 已通过 Release、VERIFY、ASan+UBSan、现有限定范围 OccSTM/EBR TSan，以及新增 Depot 专项 TSan（重复 20 轮）；汇总见 [PR #2](https://github.com/ZephyrVia/CaSTM/pull/2)。
- 这里没有提供 10 分钟 **旧版**与新版的同环境 A/B；旧版 2026-11-04 WSL2 10 分钟长跑的 VmSize 增长也不是同硬件直接对照。本报告的同环境 A/B 是 90 秒。
- Depot 故意保留 peak-concurrency 数量的 ThreadHeap 与 Slab；没有实现无认领者时自动清理空 Slab 的安全协议。
- `ThreadHeapLease` 与其他具有析构逻辑的 TLS 对象之间的销毁顺序仍需要具体业务场景审计。
- 没有完整 LSan、无限时长验证、所有分配器路径的全量 TSan 或本次改造后的统一性能矩阵。旧版 `v0.2.0-prototype` 的简历和性能结论不自动更新。
