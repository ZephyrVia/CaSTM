# CaSTM BUG 记录索引

本目录采用“一份文件对应一个根因/bug”的记录方式。索引只用于导航，
详细的证据链、修复和验证结果见对应文档。

| 编号 | Bug 文档 | 状态 | 关键提交 |
|---|---|---|---|
| BUG-01 | [ThreadHeap 线程退出归还活跃 Chunk](BUG-01-ThreadHeap线程退出归还活跃Chunk.md) | 已修复 | `fbd86f1` |
| BUG-02 | [EBR retire 纪元标记导致提前回收](BUG-02-EBR-retire纪元标记提前回收.md) | 已修复 | `6f5bcc5` |
| BUG-03 | [EBR 默认 deleter 与堆分配不配对](BUG-03-EBR默认Deleter堆配对.md) | 已修复 | `a345e04` |
| BUG-04 | [WwSTM published WriteRecord 生命周期](BUG-04-WwSTM-WriteRecord生命周期.md) | 部分修复，仍有后续问题 | `7efba79` |
| BUG-05 | [Wound-Wait 平局裁决顺序依赖](BUG-05-WoundWait平局裁决顺序依赖.md) | main 谱系已修复，bug 分支暂缓 | `8b3cdf5` |
| BUG-06 | [WwSTM 版本-值偏斜导致丢失更新](BUG-06-WwSTM版本值偏斜丢失更新.md) | 已修复 | `ffc7f14` |

最后更新：2026-09
