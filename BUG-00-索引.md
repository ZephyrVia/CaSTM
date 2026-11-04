# CaSTM BUG 记录索引

本目录采用“一份文件对应一个根因/bug”的记录方式。索引只用于导航，
详细的证据链、修复和验证结果见对应文档。

这些文件保留各修复提交当时的调查与验收记录。“全量测试”指当时已启用的集合，
历史次数和用例数不能累加成当前覆盖率。当前协议见[设计说明](OSTM_WW.md)，
投递版的启用/跳过/禁用范围见[测试说明](docs/testing.md)，
实验结果见[性能报告](docs/performance/README.md)。

| 编号 | Bug 文档 | 状态 | 关键提交 |
|---|---|---|---|
| BUG-01 | [ThreadHeap 线程退出归还活跃 Chunk](BUG-01-ThreadHeap线程退出归还活跃Chunk.md) | 已修复 | `fbd86f1` |
| BUG-02 | [EBR retire 纪元标记导致提前回收](BUG-02-EBR-retire纪元标记提前回收.md) | 已修复 | `6f5bcc5` |
| BUG-03 | [EBR 默认 deleter 与堆分配不配对](BUG-03-EBR默认Deleter堆配对.md) | 已修复 | `a345e04` |
| BUG-04 | [WwSTM published WriteRecord 生命周期](BUG-04-WwSTM-WriteRecord生命周期.md) | 已修复 | `f8d9c46` |
| BUG-05 | [Wound-Wait 平局裁决顺序依赖](BUG-05-WoundWait平局裁决顺序依赖.md) | main 谱系已修复，bug 分支暂缓 | `8b3cdf5` |
| BUG-06 | [WwSTM 版本-值偏斜导致丢失更新](BUG-06-WwSTM版本值偏斜丢失更新.md) | 已修复 | `ffc7f14` |
| BUG-07 | [WwSTM single-head Locator 协议与实现](BUG-07-WwSTM-single-head-Locator协议设计审计.md) | Commit B 已实现 | `91c32c0` |
| BUG-08 | [WwSTM TxDescriptor 生命周期与对齐](BUG-08-WwSTM-TxDescriptor生命周期与对齐.md) | 已修复 | `ba1e308`, `0247b1b`, `c5973c2` |

| BUG-09 | [OccSTM 并发剪枝链边数据竞争](BUG-09-OccSTM并发剪枝链边数据竞争.md) | 审计分支已修复并本地验证 | 复现基线 `c64e62b` |

最后更新：2026-11-04（新增 OccSTM 并发剪枝审计；各历史记录独立保留）。
