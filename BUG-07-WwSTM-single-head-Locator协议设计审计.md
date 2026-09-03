# BUG-07：WwSTM single-head Locator 协议与实现

状态：Commit B 已实现（待提交）

审计基线：`68ed73f docs: record single-head Commit A result`

Commit A：`72ec885 refactor(wwstm): introduce single-head locator state`

本文件保留前期设计审计，并记录 Commit A 到 Commit B 的协议差异、实际实现与验收结果。

## 结论

single-head 方向可行，并且能够从结构上消除当前 `data_ptr_ + record_ptr_` 的双重
authority 问题。但它不是只把两个成员合并成一个 tagged pointer：读快照、写集校验、
commit/abort 清理和测试 API 都必须同步调整。

此前阶段完成了协议审计；当前已按本文件的边界完成 Commit A 与 Commit B。两次提交保持
独立，便于分别回滚和审查：

1. Commit A：single-head、读快照和 helping 基础；
2. Commit B：prepare-before-COMMITTED、唯一提交线性化点和多变量原子性。

Commit A 曾暂时保留旧版 `TxContext::commit()` 的提交兼容路径；Commit B 已将 prepare、
partial commit 和多变量原子提交纳入同一个事务级状态协议。

## Commit A 前实现与目标的差距（历史审计）

### 1. 共享状态存在两个入口

`TMVar` 当前同时维护：

```cpp
std::atomic<NodeT*>   data_ptr_;
std::atomic<RecordT*> record_ptr_;
```

写路径先观察两个指针，再分别进行稳定性检查和发布；读路径也分别读取 record 和
stable node。这使得某一时刻的 value 与 version 可能来自不同对象，BUG-06 只能把
当前读值统一导向 stable node，不能消除结构上的双重权威。

目标应改为唯一入口：

```text
head -> VersionNode
head -> WriteRecord
```

`TaggedPtr.hpp` 已有低位标记辅助函数，可以作为实现起点，但需要补充对齐保证、原子
head 封装和无效值检查，不能直接把现有辅助函数原样当作完整协议。

Commit A 的实际状态是 `std::atomic<uintptr_t> head_`：低位 0 表示 `VersionNode`，
低位 1 表示 `WriteRecord`；两种 Ww 对象均使用系统堆，并通过静态断言保证至少有一位
可用于 tagging。

### 2. 当前 WriteRecord 不是 immutable Locator

当前 re-entrant write 会分配新的 `VersionNode`，然后直接修改已发布 record 的
`new_node` 指针。single-head 之后，已发布 record 的 `owner/old_node/new_node` 必须
保持不变；否则 reader/helping 无法把一次 head 读取稳定地解释为一个 generation。

对现有 API，推荐把 immutable 限定为 Locator 元数据：

```text
published Record 的 owner/old_node/new_node 指针不可变
owner 在 COMMITTED 之前可以修改其私有 draft node 的 payload
COMMITTED 之后 draft node 不再修改
```

这样可以保留同一事务重复写同一变量的语义，并避免 Record→Record。若要求
`VersionNode` 内容也完全不可变，则必须新增事务私有 draft/value cell；不能简单地在
现有 `tryWriteAndGetRecord()` 上把新 record 换进去。

### 3. 当前 read API 会主动拼接 value/version

`TxContext::read()` 当前先调用 `getDataVersion()`，再调用 `readProxy()`，最后再读一次
版本。single-head 后应由 `TMVar` 一次返回同一 `VersionNode` 产生的快照：

```cpp
ReadSnapshot<T> {
    T value;
    uint64_t version;
    // 可选：node identity，用于读集验证
};
```

读取流程必须是：

```text
进入 EBR
  -> h1 = head.load()
  -> 按 h1 解析出一个 VersionNode
  -> 从该 node 同时复制 value 和 version
  -> h2 = head.load()
  -> h1 != h2 则丢弃结果并重试
```

`TxContext` 的 `read_set_` 也应记录 snapshot 的 node identity/version，并通过新的
`validateSnapshot()` 接口校验，而不是继续依赖 `getDataVersion()` 与 `readProxy()` 的
分离组合。

所有 record/node 解引用都必须发生在 EBR critical section 内。当前测试中直接调用
`readProxy()` 的白盒用例需要改成显式持有 EBR，或改为只通过 `TxContext` 访问；否则会
绕过生产路径的生命周期前提。

## 目标状态机

```text
              ACTIVE
Node -> Record       -> old Node   (ABORTED)
                 \-> new Node      (COMMITTED)
```

发布和帮助都只允许 Node→Record、Record→Node：

| 当前 head | 条件 | 动作 |
|---|---|---|
| Node | 新写者安装 | CAS(Node, Record) |
| Record | owner ACTIVE | 报告冲突，执行 Wound-Wait |
| Record | owner ABORTED | CAS(Record, old_node)，退休 Record/new_node |
| Record | owner COMMITTED | CAS(Record, new_node)，退休 Record/old_node |

禁止直接执行 `Record A -> Record B`。遇到 ABORTED record 时，先帮助恢复为
`old_node`，再重新读取 head，最后尝试 Node→Record。

## 生命周期与 EBR 约束

single-head 下 head 是唯一根引用：

- `head -> Record` 时，Record 同时保持 `old_node` 和 `new_node` 可达；
- `Record -> old_node` 成功时，只退休 Record 和 new node；old node 成为 head；
- `Record -> new_node` 成功时，只退休 Record 和 old node；new node 成为 head；
- 只有成功 CAS 的线程拥有这次物理清理权；CAS 失败方不得再次 retire 或解引用失去
  所有权的 record。

`retire()` 仍然只是延迟入队，不是立即析构。事务 owner、reader 和 helper 都必须在
EBR 保护期内使用其已经加载的裸指针；离开 EBR 后不能保存旧指针继续使用。

TxDescriptor 当前有意不回收，因此 Record 的 owner 指针暂时仍有稳定生命周期。后续
若处理 descriptor 回收，必须把 descriptor 引用纳入新的生命周期协议，本轮不应顺手
解决。

事务写集可能在 Record 已被 helper 展平后仍短暂保存该 Record 的原始 token，直到事务
清理写集。Commit A 的提交/回滚兼容入口会先用 `head_` 比较 token；若当前 head 已不是
该 Record，则不再解引用 token，也不重复 retire。这是对“helper 先于 owner cleanup”
交错的必要保护，不能把写集中的裸指针误当作仍然拥有 head 根引用。

## 读协议审计

解析 head 后的语义应为：

| head 形态 | 读取的 node |
|---|---|
| VersionNode | 该 node |
| 自己的 ACTIVE Record | new node（Read-Your-Own-Write） |
| 别人的 ACTIVE Record | old node |
| ABORTED Record | old node |
| COMMITTED Record | new node |

需要额外注意：status 可能在两次 head load 之间变化。只要选出的 node 与一次合法的
status 观察相对应，并且 head identity 复核通过，就可以把读取线性化在该观察点；若
head 已发生变化则必须重试。

跨多个变量的 reader 如果恰好跨过事务的提交 CAS，可能在事务内部暂时读到不同阶段的
结果；读集验证必须让这种事务最终 abort，成功提交的 reader 不得提交 mixed snapshot。
因此“多变量原子性”测试应验证成功事务的观察结果和最终提交结果，不能只把任意一次
中途读取当作已经提交的观察。

## 写与 helping 审计

写入流程应变成：

1. head 是 Node：以该 Node 作为 immutable `old_node`，构造 Record，CAS 安装；失败则
   重新加载 head；
2. head 是同一事务的 Record：更新其私有 draft payload，不能改 Record 指针字段；
3. head 是别人的 ACTIVE Record：返回冲突，交给现有 Wound-Wait；
4. head 是 ABORTED/COMMITTED Record：先执行对应的 Record→Node helping，再重试。

Wound-Wait 的年龄规则不应改变。但当前 bug 分支仍有按 `TxDescriptor*` 地址处理同一
时间戳平局的代码；若该分支尚未移植 BUG-05 的创建序号，应在 v2 中使用稳定的逻辑
tie-breaker，不能把堆地址继续当作年龄依据。

## commit/abort 协议审计（Commit A 基线）

Commit A 的实际提交路径仍是“先完成事务状态转换，再逐个清理变量”：

1. `TxContext::commit()` 验证读集和写集；
2. 空写集直接清理并返回；
3. 执行 `ACTIVE -> COMMITTED`；
4. 调用 `commitReleaseRecord(record, commit_ts)`，在每个变量上写入最终时间戳并做
   `Record -> new_node`；
5. 任意清理接口仍可能返回 `false`，因此曾存在“descriptor 已 COMMITTED 但
   `commit()` 返回失败”的协议缺口。

Commit A 已把 cleanup CAS 失败视为“helper 已完成或 head 已进入下一代”，不再重复
retire；但它没有改变第 3 步早于第 4 步的顺序。`VersionNode::write_ts` 在该过渡期间
改为 atomic，以承受 COMMITTED 后写入最终时间戳的并发读写。

## Commit A → Commit B 差异审计与实际实现

这次先按 Commit A 的代码逐条对照，再落地 Commit B，差异如下：

| 维度 | Commit A 基线 | Commit B 实现 |
|---|---|---|
| owner 写入与提交 | owner 可直接修改 draft，提交路径没有统一冻结点 | `TxDescriptor::write_gate` 串行化 owner 写与 prepare；`prepare_started` 后拒绝新写 |
| 提交前检查 | validate 后立即做状态 CAS | validate read set、write ownership/token，再逐个 prepare |
| 最终时间戳 | `COMMITTED` 后由 cleanup 写入 | `ACTIVE` 期间由 prepare 写入全部 `new_node->write_ts` |
| 事务线性化 | 没有单独、稳定的多变量逻辑点 | 一次 `ACTIVE -> COMMITTED` CAS 是唯一逻辑提交点 |
| cleanup 结果 | `commitReleaseRecord()` 的 bool 可能污染 commit 结果 | `helpComplete()` 为 void；CAS 失败只表示别人完成，不能回滚或返回失败 |
| payload/Locator | Locator 已不可变，draft payload 可在提交前修改 | prepare 后 Record 与 draft 都冻结；COMMITTED 后只读 |
| abort 竞态 | owner/helper 共享旧兼容清理入口 | 先 `ACTIVE -> ABORTED`，再 `Record -> old_node`；COMMITTED 绝不进入回滚 |

当前 `TxContext::commit()` 的实际顺序是：

```text
lock write_gate
  -> prepare_started = true
  -> validate read set
  -> validate write ownership/token
  -> 获取 commit_ts
  -> prepare 所有 Record（写最终 timestamp，冻结 draft）
  -> ACTIVE -> COMMITTED         ← 唯一逻辑线性化点
  -> helpComplete 所有 Record    ← 可被 helper 抢先完成
  -> cleanupResources()
```

`TMVarBase` 对应拆成 `validateForCommit()`、`prepareCommit()` 和
`helpComplete()`。`prepareCommit()` 只在 head 仍是本事务保存的 Record 且 descriptor
仍为 ACTIVE 时成功；`helpComplete()` 不把 Record→Node 的 CAS 结果暴露为事务失败。
事务写集仍可能短暂保存已被 helper 展平的 raw token，但 owner 在 EBR 保护期内使用它，
且 cleanup 会先比较 head token，失去 head 身份后不再解引用或重复 retire。

因此，多变量事务在 COMMITTED 前只能被整体判为 ABORTED；一旦 descriptor CAS 成功，
所有变量都按同一 descriptor 状态解释为 new，即使各变量的物理 flatten 尚未完成，也
不会出现逻辑上的 partial commit。读者如果跨过该 CAS 观察到不同阶段，读集的 node
identity/version 校验会使它在提交时 abort，而不是提交 mixed snapshot。

状态转换仍是不可逆的：

```text
ACTIVE -> COMMITTED
ACTIVE -> ABORTED
```

`abortTransaction()` 在观察到 COMMITTED 时只做终态 cleanup；即便状态 CAS 失败是因为
另一条路径已经完成 COMMITTED，也不再恢复 old node。

## 必须新增或改写的测试

Commit A 至少覆盖：

1. snapshot 的 value/version 来自同一个 VersionNode；
2. ACTIVE Locator 读取 old node；
3. COMMITTED 但尚未 flatten 时读取 new node；
4. ABORTED 但尚未 flatten 时读取 old node；
5. 其他线程可以帮助完成两种 Record→Node 转换；
6. 同一事务重复写同一变量且不产生 Record→Record；
7. 所有白盒裸指针访问都在 EBR 保护区内。

Commit B 至少覆盖：

1. 多变量事务不存在可成功提交的 partial commit；
2. 某个 cleanup CAS 被人为失败时，事务仍保持 COMMITTED 且 commit 返回成功；
3. status 已终态时 owner/helper 重复清理不会重复 retire；
4. prepare 后 draft 冻结，重复写不能改变已准备值；
5. 高竞争写入、读集校验和 Wound-Wait 路径。

当前 `ActiveRecordReadIgnoresStaleOldNode` / `AbortedRecordReadUsesStableData` 通过
直接篡改 `old_node` 构造旧双指针病态；single-head 后应改为“head generation 与
snapshot 同源”的测试，不能继续依赖篡改 immutable Locator。

## 实施边界

本阶段不改：

- EBR 算法和 allocator；
- Wound-Wait 的年龄策略；
- OccSTM；
- TxDescriptor 回收；
- benchmark 和性能优化。

前置审计的结论是“可以实施，但需按上述接口边界实施”，不是直接接受当时
`TMVar.hpp` 的大块替换。re-entrant write 的 draft 语义、快照验证接口和 commit cleanup
返回语义已经在 Commit A/B 中落实，下面记录两次提交的实际结果。

## Commit A 历史实现与验收

实现内容：

1. `TMVar` 以 tagged `head_` 作为唯一共享状态；Node→Record 发布和 Record→Node
   helping/清理均使用 CAS，禁止 Record→Record 窃取。
2. 已发布 Locator 的 `owner/old_node/new_node` 不再修改；同一 ACTIVE owner 的重复写
   只更新私有 `new_node->payload`，并复用同一个 Record 与 draft Node。
3. `readSnapshot()` 从同一 VersionNode 复制 value/version，并以 h1/h2 复核 head；
   `TxContext` 读集记录 node identity 与 version，写前和提交时按同一代校验。
4. 新增 ACTIVE、ABORTED、COMMITTED、重入写、帮助、head 变化重试和 published Record
   EBR 生命周期回归；补上 helper 已退休但 owner 写集仍持 token 时的无解引用清理路径。

验证结果：

| 验证 | 结果 |
| --- | --- |
| 默认 VERIFY 全量 ×20 | 所有非 disabled CTest 条目无失败；4 个 mode=0/EBR 专属测试按设计跳过，另有 1 个 disabled 高竞争测试 |
| mode=0 全量 ×20 | 所有非 disabled CTest 条目全部通过 |
| mode=0 `ConcurrentPathIncrement` ×100 | 100/100 通过 |
| mode=0 高竞争 Ww disabled ×50 | 50/50 通过 |
| mode=0 EBR 6 项 ×20 | 全量回归通过 |
| mode=0 单头 TMVar 直接测试 | 16/16 通过 |
| ASan+UBSan `detect_leaks=0` 全量 | active 测试通过，无 UAF、double-free、invalid-free |
| ASan+UBSan 高竞争 Ww ×20 | 20/20 通过；无 ASan 内存错误 |

UBSan 仍会报告项目既有的 `TxDescriptor alignas(64)` 对齐问题；本 Commit A 按实施边界
未处理该独立问题。运行验收时关闭 UBSan halt，避免该已知诊断阻断 ASan 结果。

## Commit B 实现与验收

实现提交：`refactor(wwstm): make descriptor commit the transaction linearization point`

实现文件：

1. `TxDescriptor` 增加 owner 写门和 prepare 状态；
2. `WriteRecord` 增加 prepared 状态，`VersionNode` 的最终 timestamp 在 COMMITTED 前写入；
3. `TMVarBase` 拆分 validate/prepare/help 接口，保留 single-head tagged head 和 EBR
   退休规则；
4. `TxContext::commit()` 按 validate → prepare → descriptor CAS → infallible help 顺序
   执行，COMMITTED 后不再走 abort 回滚；
5. 增加 prepare 冻结、COMMITTED/ABORTED before flatten、多变量原子提交、helper 竞争、
   token-only cleanup 和多变量并发 snapshot 回归。

本次验收结果：

| 验证 | 结果 |
| --- | --- |
| VERIFY 全量 ×20 | 通过 |
| mode=0 全量 ×20 | 通过 |
| VERIFY/mode=0 高竞争 Ww ×100 | 各 100/100 通过 |
| VERIFY/mode=0/ASan 多变量原子性压力 ×100 | 各 100/100 通过 |
| ASan+UBSan（`detect_leaks=0`）全量 | 32 项通过；无 UAF、double-free、invalid-free |
| no-hooks（`STM_WW_TEST_HOOKS=0`）生产头文件编译 | 通过 |

已知范围外问题仍不变：TxDescriptor 回收/alignment、EBR/allocator 的后续性能与精确
回收、Wound-Wait 年龄策略、benchmark 和 OccSTM。本提交没有改动这些部分。
