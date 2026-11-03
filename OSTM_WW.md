# WwSTM 当前设计说明

本文保留历史文件名 OSTM_WW，内容于 2026-11-04 按源码基线 `da9634b` 更新。
旧草案中的 LSA 快照扩展、提交后补写时间戳和全链路无锁描述已被当前实现取代。
修复过程见 [BUG 索引](BUG-00-索引.md)，实验范围见[测试说明](docs/testing.md)。

## 数据结构

| 结构 | 职责 | 实现 |
| --- | --- | --- |
| TxContext | 持有读写集，开始、验证、提交或中止事务 | [TxContext.hpp](include/WwSTM/TxContext.hpp) |
| TxDescriptor | 保存 start_ts、ACTIVE/ABORTED/COMMITTED 状态与 prepare gate | [TxDescriptor.hpp](include/WwSTM/TxDescriptor.hpp) |
| TMVar | 用一个 tagged head 发布 Node 或 Record | [TMVar.hpp](include/WwSTM/TMVar.hpp) |
| WriteRecord | 保存 owner、old_node、new_node 和 prepared 状态 | [WriteRecord.hpp](include/WwSTM/WriteRecord.hpp) |
| VersionNode | 保存 payload 与 write_ts | [VersionNode.hpp](include/WwSTM/VersionNode.hpp) |

WwSTM 的 VersionNode 没有 OccSTM 的 prev 版本链。它通过 Record 同时保留旧节点和
草稿节点，并结合读集验证处理并发修改。两个后端共享“版本化对象”思路，但不能
把它们描述成相同的历史快照实现。

## 读取与写入

~~~text
head → VersionNode | WriteRecord
~~~

对其他事务，Record 的逻辑视图由 owner 状态决定：

| owner 状态 | 可见节点 |
| --- | --- |
| ACTIVE | old_node |
| ABORTED | old_node |
| COMMITTED | new_node |

owner 可以读取自己的 new_node，实现 read-your-own-writes。
`readSnapshot()` 从同一 VersionNode 复制 value 和 version，并复核 head identity；
TxContext 记录 node identity/version，用于后续验证。

写操作将旧 Node 与新草稿关联到 Record，通过 head CAS 发布。事务对同一变量的
重复写复用自己的 Record/草稿；prepare 开始后冻结草稿。owner 写入与 prepare
使用同一个 descriptor `write_gate` 互斥锁。

读者可能观察到一个事务生效前后的不同变量，随后验证失败并重试。因此应用必须
以事务成功提交为读取结果的接受条件；当前没有对所有执行历史给出 serializability
或 opacity 的形式化证明。

## 冲突裁决与进展边界

当前实现按 start_ts 判定年龄，相同时间戳时用 descriptor 地址打破平局。
较老事务尝试将冲突方从 ACTIVE 改为 ABORTED；较年轻事务中止自身。
终态 Record 可由其他线程帮助展平。

写入重试包含 yield、周期性短暂 sleep 和次数上限，外层逻辑事务仍可能重试。
这种实现使用了 Wound-Wait 风格的主动中止，但不据此承诺无饥饿、wait-free
或 lock-free。新事务会重新获取时间戳；108/108 次 benchmark 完成仅是有限实验结果。

## 提交与中止

写事务的顺序为：

1. 获取 write_gate，检查 ACTIVE 并标记 prepare_started。
2. 验证读集和写集 ownership。
3. 获取 commit_ts，prepare 所有已发布 Record；最终 payload 与时间戳必须在此阶段就绪。
4. CAS：ACTIVE → COMMITTED，作为事务逻辑提交点。
5. 释放 write_gate，帮助各 Record 展平，清理事务资源。

COMMITTED 是不可逆终态。helper 先完成展平，不改变 owner 的提交结果。
提交 CAS 之前失败的事务进入 ABORTED，逻辑上保留旧值，再完成清理。

Record → Node 的成功 CAS 决定该代 Record 及废弃节点的退休责任，避免
owner/helper 重复退休。清理遍历写集，所以不把整个事务回滚描述为 O(1)。
old_node/new_node 是内存中的版本引用，没有磁盘 Undo/Redo 日志或故障恢复能力。

## 生命周期与分配

- mode=0 中，reader、owner 和 helper 对共享对象的访问受到 EBR critical section 保护。
- 已发布 Record 从 head 移除后通过 EBR 退休；仍可能有 reader 持有其指针。
- descriptor 等对应 Record 清理完成后再退休，且在退出 EBR 临界区之前登记退休。
- `EBRManager::retire()` 读取全局 epoch，为垃圾队列选择桶；不使用可能落后的线程 epoch。
- Record/VersionNode 默认使用系统堆，benchmark 的 allocator 选项可将其切换为 TierAlloc。
- TxDescriptor 使用满足 cache-line 对齐的系统分配，GarbageNode 元数据也使用系统堆；
  它们不参与 allocator ablation 切换。
- ThreadHeap 在线程退出时保留部分 chunk，避免活对象被复用覆盖；长期资源回收仍有边界。

VERIFY 为白盒逻辑测试保留部分已发布对象，并使用额外的 TMVar 互斥保护。
该模式不验证 WwSTM 的真实物理回收，不能用于性能比较或宣称无泄漏。

## 与证据对应

| 问题 | 回归入口或记录 |
| --- | --- |
| value/version 偏斜 | [BUG-06](BUG-06-WwSTM版本值偏斜丢失更新.md)、TMVarTest.HeadIdentityChangeRetriesSnapshot |
| 多变量逻辑提交与 helper 交错 | [BUG-07](BUG-07-WwSTM-single-head-Locator协议设计审计.md)、TMVarTest.MultiVariableCommitLinearizesAtDescriptorStatus / MultiVariableAtomicityStress |
| 已发布 Record 生命周期 | [BUG-04](BUG-04-WwSTM-WriteRecord生命周期.md)、TMVarTest.PublishedWriteRecordRetiresAfterHelper |
| descriptor 对齐与延迟回收 | [BUG-08](BUG-08-WwSTM-TxDescriptor生命周期与对齐.md)、TxDescriptorTest |
| EBR 退休 epoch | [BUG-02](BUG-02-EBR-retire纪元标记提前回收.md)、EBRManagerTest |

是否启用、模式跳过情况及本轮验证结果以[测试范围](docs/testing.md)为准。
