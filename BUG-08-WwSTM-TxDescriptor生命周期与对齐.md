# BUG-08：WwSTM TxDescriptor 生命周期与 over-aligned 分配

状态：已修复

修复提交：

- `ba1e308 fix(wwstm): align transaction descriptors correctly`
- `0247b1b fix(wwstm): reclaim transaction descriptors through ebr`
- `c5973c2 test(wwstm): cover descriptor abort cleanup churn`

验证日期：2026-09-03

本文件记录 single-head Locator 已经稳定之后，独立发现的 Descriptor 生命周期和
`alignas(64)` 分配问题。single-head、prepare/commit 协议本身仍记录在
[BUG-07](BUG-07-WwSTM-single-head-Locator协议设计审计.md)；本文件不扩展 WwSTM
协议语义。

## 结论

`TxDescriptor` 不是可以在 owner 函数返回时立即 `delete` 的普通临时对象。已发布的
`WriteRecord` 通过裸指针保存 `owner`，reader、冲突处理线程和 helper 可能在 Record
从 `TMVar::head_` 移除后仍持有该 Record。安全顺序必须是：

```text
事务进入终态
    -> 所有已发布 Record 完成 Record -> Node
       （成功 CAS 的线程负责退休 Record/被替换 Node）
    -> 退休 TxDescriptor
    -> 等待 EBR grace period
    -> 析构并释放 TxDescriptor
```

本阶段已经把 mode=0 下原先故意泄漏的动态 Descriptor 纳入 EBR 回收，并修复其
over-aligned `new/delete` 配对。VERIFY 模式仍保留 Descriptor，是因为该模式不执行
真实 EBR 物理回收，白盒测试可能保留已发布对象的 raw token；这不是 mode=0 的回收
协议。

## 原问题

### 1. Descriptor 曾被有意泄漏

`TxContext` 通过 `my_desc_ = new TxDescriptor(start_ts_)` 创建自己的 Descriptor，
但旧版 `cleanupResources()` 只把 `my_desc_` 置空，没有回收对象。这样暂时绕开了
悬空 `WriteRecord::owner`，却留下了每个事务一个 Descriptor 的长期泄漏，也没有验证
“Record 读者离开前 Descriptor 不能回收”这一真实约束。

### 2. `alignas(64)` 与旧分配函数不匹配

`TxDescriptor` 声明为 `alignas(kCacheLineSize)`，但旧的类内
`operator new(std::size_t)` 直接转发到普通 `::operator new(size)`。对象声明要求
的对齐和实际分配接口不一致，UBSan 曾报告过 misaligned access；普通运行可能只因
当前分配器碰巧返回足够大的地址而不暴露。

### 3. 不能简单改成 `delete my_desc_`

以下路径都可能访问 `TxDescriptor`：

- `TMVar::selectVisibleNode_()` / `readSnapshot()`：从 Record 读 `owner->status`，决定
  读 `old_node` 还是 `new_node`；
- `TMVar::tryWriteAndGetRecord()`：检查同 owner、读取冲突 owner 的状态并返回
  `conflict_tx`；
- `validateForCommit()`、`prepareCommit()`、`helpComplete()` 和
  `abortRestoreData()`：验证 Record 所属关系、准备状态或帮助终态 Record；
- `TxContext::resolveConflict()`：读取冲突 Descriptor 的 `status` 和 `start_ts`，
  执行 Wound-Wait；
- owner 的 commit/abort cleanup，以及只用于测试的 head/helper 诊断接口。

其中 `WriteRecord::owner`、`TxContext::write_set_` 中的 Record token 和
`conflict_tx` 都是非 owning 指针。head 移除只表示共享根改变，不表示已经没有 EBR
读者。

## 所有权图

```text
TxContext
  └─ my_desc_  -- owns while transaction is alive
       ├─ status / start_ts / write_gate / prepare_started
       └─ (after cleanup) EBR garbage list owns deferred reclamation

TMVar::head_
  └─ tagged WriteRecord
       ├─ owner    -- non-owning TxDescriptor*
       ├─ old_node -- generation retained by Record
       └─ new_node -- generation retained by Record

TxContext::write_set_
  └─ raw Record token -- non-owning; may be stale after helper wins CAS

conflict_tx / reader / helper
  └─ transient raw Descriptor/Record pointers -- protected by caller's EBR slot
```

物理回收的 owning 关系只有两处：事务活跃期间 `TxContext::my_desc_` 持有
Descriptor；对象进入 EBR 退休队列后由对应的 `GarbageNode` 延迟持有。Record 不再由
owner 直接 `delete`，而由成功的 Record→Node CAS 的线程退休。

## 生命周期审计与安全证明

### EBR 是所有 `owner` 解引用的前置条件

mode=0 下，生产入口 `TxContext` 构造时进入 EBR，直到 commit/abort 的最终
`cleanupResources()` 才 leave。因而以下访问处于 EBR critical section：

1. 读路径读取 `record->owner` 与 `owner->status`；
2. 写冲突路径读取冲突 Descriptor；
3. validate/prepare/help/abort 清理读取 Record 的 owner 和 generation；
4. Wound-Wait 读取冲突 Descriptor 的年龄字段和状态；
5. owner 清理写集 token，或 helper 读取已发布 Record。

直接使用底层 `TMVar` 白盒 API 的测试必须自行 `enter()`/`leave()`；该底层 API 不会
替调用者自动建立 EBR 保护。新增的跨线程测试明确把 raw Record 的读取放在 EBR
临界区内。

### commit 路径

`TxContext::commit()` 在 Descriptor 状态 CAS 成功后调用 `finishCommitted_()`：

1. 逐个调用 `helpComplete(record_token)`；
2. 只有成功把当前 head 从 Record CAS 为 `new_node` 的线程才退休 Record 和旧 Node；
3. helper 先完成时，owner 后续 cleanup 的 head identity 检查失败，既不解引用 stale
   token，也不再次退休；
4. 所有 Record cleanup 完成后，`cleanupResources()` 才把 Descriptor 从
   `my_desc_` 移出并调用 `EBRManager::retire(descriptor)`；
5. 最后才 `leaveEpoch()`，因此 owner 自己在退休瞬间仍受 EBR 保护，其他已经加载
   Record 的 reader/helper 也会阻止同一对象过早回收。

`write_gate` 是 prepare 阶段的门，不是 Descriptor 的拥有者。commit 在进入任何
可能退休 Descriptor 的 cleanup 前显式 unlock；否则 Descriptor 被 EBR 回收后，
函数栈上的 `unique_lock` 仍可能在析构时访问已经析构的 `write_gate`。

### abort 路径

ABORTED 事务先以逆序调用 `abortRestoreData()`。该函数先比较
`head_ == packRecord_(token)`，只有仍拥有当前 head 的线程才执行 Record→old Node
并退休 Record/new Node；helper 已经完成时，owner 的 token cleanup 直接返回。
之后才进入 Descriptor cleanup。

如果 late cleanup 观察到 COMMITTED，不能把它当作普通 abort 直接清空写集；当前实现
统一走 `finishCommitted_()`，再次帮助剩余 Record 后才退休 Descriptor。这补上了
“状态已 COMMITTED 但 Record 尚未物理展平”的边界路径。

### 为什么 grace period 足够

假设某 reader/helper 已经从 head 加载 Record：

1. 它在本次 Record/Descriptor 解引用前已经宣布进入 EBR；
2. owner/helper 可以把 Record 从 head 移除并将 Record、Detached Node 入退休队列，
   但当前 reader 的 slot 仍阻止对应 epoch 收集；
3. owner 只能在所有自己可见的 Record cleanup 完成后退休 Descriptor；
4. reader 离开 EBR 后，Descriptor 与 Record 才可能在后续 epoch 被收集；
5. 因而 reader 在离开前执行 `record->owner->status` 时，Record 和 owner 都仍然
   存活。

这不是“head 没了所以对象没人用”的推理，而是“所有可能持有旧指针的访问者都在
EBR 内，且 Descriptor 退休晚于 Record 退休”的推理。

## 实现改动

### `TxDescriptor` 对齐和调试基建

文件：[include/WwSTM/TxDescriptor.hpp](include/WwSTM/TxDescriptor.hpp)

- 保留 `alignas(kCacheLineSize)`；
- `operator new(std::size_t)` 使用 C++17 aligned global new；
- 提供 aligned new/delete 以及 sized/unsized aligned delete 重载，保证分配和释放
  使用同一对齐契约；
- 在 `STM_WW_TEST_HOOKS` 下增加 `debug_tx_id`、allocation/reclaim/destructor 计数器；
- `debug_tx_id` 只用于日志和测试定位，不改变 Wound-Wait 年龄策略，也不参与生产
  决策。

### `TxContext` 回收顺序

文件：[include/WwSTM/TxContext.hpp](include/WwSTM/TxContext.hpp)

- 新增 `finishCommitted_()`，让 COMMITTED 的 owner/helper late cleanup 也先完成
  Record helping；
- commit 在 cleanup 前显式释放 `write_gate`；
- `cleanupResources()` 将动态 Descriptor 通过 `EBRManager::retire()` 延迟回收，
  不直接 `delete`；
- VERIFY 模式保留 Descriptor，以维持该模式的 white-box 保留语义。

本阶段没有修改 `TMVar` 的 single-head 状态机，也没有把 Record 回收和 Descriptor
回收混为一个 deleter。Record 的退休仍由原有 Record→Node 成功 CAS 负责，Descriptor
只由事务 cleanup 退休。

## 回归测试

新增文件：[tests/WwSTM/test_txdescriptor.cpp](tests/WwSTM/test_txdescriptor.cpp)

| 测试 | 覆盖点 | 结果 |
|---|---|---|
| `OverAlignedAllocationUsesCacheLineAlignment` | 4096 个 Descriptor 的地址满足 `alignof(TxDescriptor)`/64-byte 对齐，new/delete/destructor 计数一致 | 通过 |
| `ReclamationWaitsForReaderHoldingPublishedRecord` | reader 持有旧 Record；owner/helper 先移除并退休 Record，再退休 Descriptor；reader 随后读取 `owner->status` | 通过 |
| `HelperCanReadTerminalOwnerAfterOwnerCleanup` | helper 先加载 Record，owner `TxContext` 返回并退休 Record/Descriptor 后，helper 在 EBR 内读取终态 owner | 通过 |
| `TransactionDescriptorChurnReclaimsThroughEbr` | 2000 个真实空事务交替走 commit 与析构 abort，Descriptor allocation/reclaim/destructor 计数闭合 | 通过 |

确定性重复结果：

- `TxDescriptorTest.*` ×200：4 个测试每轮全部通过，共 800 轮；
- Mode=0 全量 ×20：每轮 36/36 通过（不含 1 个 disabled 测试）；
- VERIFY 全量 ×20：每轮 29 pass、7 个按设计 skip；对齐测试执行，真实 EBR 回收
  测试在 VERIFY 中 skip；
- Mode=0 高竞争 Ww ×100：100/100 通过；
- Mode=0 多变量原子性 ×100：100/100 通过；
- EBR/TMVar 既有回归：27/27 通过。

## Sanitizer 基线

| 配置 | 结果 |
|---|---|
| ASan，Mode=0，`detect_leaks=0` 全量 | 36/36 通过；无 UAF、double-free、invalid-free |
| ASan+UBSan，Mode=0，`detect_leaks=0` 全量 | 36/36 通过；无对齐、UAF、double-free、invalid-free |
| no-hooks（`STM_WW_TEST_HOOKS=0`）头文件语法实例化 | 通过 |
| TSan | 宿主限制：CMake discovery 和手动启动均报 `ThreadSanitizer: unexpected memory mapping`，退出码 66；没有得到可解释的代码 race 报告 |
| LSan | 当前 ptrace 执行环境直接报 `LeakSanitizer has encountered a fatal error`，未生成 leak 分类；不能据此判定代码 leak |

### Leak 口径

mode=0 的动态 `TxDescriptor` 不再按事务永久泄漏，新增计数测试证明在 EBR grace
period 后可以析构并释放。VERIFY 模式仍有意保留 Descriptor 和白盒对象；此外项目
的 EBR singleton、TLS slot、ThreadHeap/测试生命周期资源可能在进程退出时存活。
由于本环境 LSan 不可用，最终 leak 分类应在非 ptrace 的原生运行环境补做；本阶段
不把 `detect_leaks=0` 的 ASan 结果表述成“没有任何泄漏”。

## 不在本阶段范围内

- Wound-Wait 年龄/tie-breaker 策略；
- single-head Locator 状态机和 Commit B 语义扩展；
- EBR 算法性能、线程槽精确回收和 ThreadHeap 分配器优化；
- OccSTM、benchmark、README/CI 包装；
- 既有 disabled 测试是否默认纳入全量入口。

如果后续要减少 VERIFY 的测试保留对象或实现进程级无泄漏退出，需要单独定义
white-box 语义和 singleton/TLS 的 shutdown 顺序，不能通过提前 `delete` Descriptor
来替代 EBR 证明。
