# BUG-07：WwSTM single-head Locator 协议与实现

状态：Commit A 已实现；Commit B 未开始

审计基线：`f8d9c46 fix(wwstm): retire published write records after rollback`

Commit A：`72ec885 refactor(wwstm): introduce single-head locator state`

本文件保留前期设计审计，并在末尾记录 Commit A 的实际实现与验收结果。

## 结论

single-head 方向可行，并且能够从结构上消除当前 `data_ptr_ + record_ptr_` 的双重
authority 问题。但它不是只把两个成员合并成一个 tagged pointer：读快照、写集校验、
commit/abort 清理和测试 API 都必须同步调整。

此前阶段完成了协议审计；当前已按本文件的边界完成 Commit A。实施仍分成两个提交：

1. Commit A：single-head、读快照和 helping 基础；
2. Commit B：prepare-before-COMMITTED、唯一提交线性化点和多变量原子性。

Commit A 暂时保留旧版 `TxContext::commit()` 的提交兼容路径，因此不宣称已经解决
Commit B 的 prepare、partial commit 或多变量原子提交问题。

## 当前实现与目标的差距

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

## commit/abort 必须同步重构（Commit B）

当前 `TxContext::commit()` 在 `ACTIVE -> COMMITTED` 后，才逐个调用
`commitReleaseRecord()`；而后者仍可能返回失败，当前代码甚至会让已经 COMMITTED 的
事务返回 `false`。这与目标协议冲突。

Commit A 保留了这条旧兼容路径，只把 `commitReleaseRecord()` 的物理清理改为
`Record -> new_node`；它不再把 cleanup CAS 失败当作内存所有权失败，也不改变旧提交
顺序的协议缺陷。`VersionNode::write_ts` 在 Commit A 中改为 atomic，以承受这条临时路径
中“COMMITTED 后写入最终时间戳”的并发访问；Commit B 应把时间戳写入移到 prepare 阶段，
届时可恢复为更严格的不可变发布语义。

Commit B 应改为：

### Phase 1：validate

- 验证 read set 的 snapshot；
- 验证所有 write Record 仍由本事务占有；
- 确认 status 仍为 ACTIVE。

验证失败只允许发生在 COMMITTED 之前。

### Phase 2：prepare

- 获取一个 commit timestamp；
- 在 status 仍为 ACTIVE 时，准备所有 draft `new_node->write_ts`；
- 确认 COMMITTED 之后不再需要任何可能失败的工作。

### Phase 3：唯一提交点

执行一次：

```cpp
ACTIVE -> COMMITTED
```

该 CAS 是事务的逻辑提交线性化点。CAS 成功后 `commit()` 必须最终返回成功。

### Phase 4：物理 cleanup/help

逐个执行：

```text
Record(COMMITTED) -> new_node
```

cleanup CAS 失败只表示已经被 helper 完成，或 head 已进入下一代合法状态，不能再把
事务判定为失败。`TMVarBase` 的 bool 接口需要拆成“prepare 是否成功”和“cleanup/help
是否完成或已被别人完成”，避免把物理清理结果当作逻辑提交结果。

Abort 则先执行 `ACTIVE -> ABORTED`，随后由 owner 或 helper 完成：

```text
Record(ABORTED) -> old_node
```

同样只有成功 CAS 的一方负责 retire；其他方 CAS 失败后只结束清理，不重复处理 record。

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
4. 高竞争写入、读集校验和 Wound-Wait 路径。

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

结论是“可以实施，但需按上述接口边界实施”，不是直接接受当前 `TMVar.hpp` 的大块
替换。尤其在确定 re-entrant write 的 draft 语义、快照验证接口和 commit cleanup 返回
语义之前，不应开始 Commit A 的代码修改。上述前置条件已经在 Commit A 中落实，下面
记录实际结果。

## Commit A 实现与验收

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

## Commit B 余项

- prepare 阶段写入最终 commit timestamp；
- 将 `ACTIVE -> COMMITTED` 变为唯一逻辑提交点，清理失败不得改变提交结果；
- 处理多变量事务的 partial commit/原子性；
- 为 owner/helper 重复清理建立更完整的接口语义，并补齐故障注入测试。

本次未改 EBR、allocator、Wound-Wait 年龄策略、TxDescriptor 回收/alignment 或 OccSTM。
