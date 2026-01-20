
# OSTM_WW 系统构建指南

本指南遵循 **Bottom-Up（自底向上）** 的原则，从最基础的原子状态定义开始，逐步构建到复杂的事务逻辑。

## 第一阶段：基础元数据定义 (Metadata & Status)

在触碰具体数据之前，必须先定义“状态”和“时间”。

### 1. 事务状态机 (Atomic Status Machine)
定义一个原子枚举，用于控制事务的生命周期。这是实现 **Active Abort** 的关键。
*   **设计要点**：
    *   使用 `enum class TxStatus : uint8_t`。
    *   状态包括：`ACTIVE`, `COMMITTED`, `ABORTED`。
    *   **关键机制**：该状态必须被包装在 `std::atomic` 中，且必须允许**非 Owner 线程**（即竞争者）进行 CAS 修改（将 ACTIVE 改为 ABORTED）。

### 2. 事务描述符 (Transaction Descriptor)
这是系统中流转的身份凭证。
*   **成员**：
    *   `status`：上述的原子状态。
    *   `start_ts`：事务开始时的全局时间戳（用于 Wound-Wait 比较）。
    *   `allocator_ctx`：指向当前线程分配器上下文的指针（用于快速释放）。
*   **内存策略**：描述符本身也应该通过 TLS 分配器分配，并在事务结束后通过 EBR 回收。

---

## 第二阶段：核心数据载体 (Data Structures)

利用 **Indirection（间接层）** 技术，我们需要两类对象：稳态的数据节点和暂态的锁节点。

### 1. 数据节点 (VersionNode)
这是用户定义的实际数据容器。
*   **成员**：
    *   `T data`：用户数据 payload。
    *   `write_ts`：写入该版本时的提交时间戳（用于 LSA 验证）。
*   **继承/接口**：必须继承自 `EbrBase` 或包含 EBR 的 hook，以便被安全回收。

### 2. 定位器 (Locator) —— 核心组件
`Locator` 是 OSTM_WW 的灵魂，它既是锁，也是 Undo Log，还是 Redo Log。
*   **成员**：
    *   `owner` (TxDescriptor*)：指向发起修改的事务。
    *   `old_version` (VersionNode*)：**Undo Log**。指向修改前的节点（快照读使用）。
    *   `new_version` (VersionNode*)：**Redo Log**。指向包含新值的节点（提交后生效）。
*   **构建逻辑**：每次写操作前，通过 Allocator 分配一个新的 Locator。

### 3. 标记指针 (Tagged Pointer) 抽象层
为了在 `std::atomic<uintptr_t>` 中区分上述两种指针，需要封装一套位运算工具。
*   **规则**：利用指针内存对齐的特性（通常最低位为 0）。
    *   `IsLocator(ptr)`: 检查 LSB 是否为 1。
    *   `IsNode(ptr)`: 检查 LSB 是否为 0。
    *   `SetLocatorTag(ptr)` / `RemoveLocatorTag(ptr)`: 设置或清除 LSB。

---

## 第三阶段：共享变量句柄 (TMVar Implementation)

这是用户持有的共享对象句柄。

### 1. 结构定义
内部持有一个 `std::atomic<uintptr_t> head`。

### 2. 辅助方法实现
实现内部私有的 `load_atomic()` 方法，它需要处理 Tagged Pointer 的解引用逻辑：
*   如果 `head` 是 Node -> 直接返回。
*   如果 `head` 是 Locator -> 读取 `Locator->owner->status`：
    *   若状态为 `COMMITTED` -> 返回 `new_version`。
    *   若状态为 `ABORTED` -> 返回 `old_version`。
    *   若状态为 `ACTIVE` -> 依然返回 `old_version` (MVCC 读不等待)。

---

## 第四阶段：事务上下文与 LSA 读逻辑

在线程本地（Thread Local）构建事务控制逻辑。

### 1. 事务上下文 (TxContext)
每个线程维护一个上下文对象：
*   `read_set`：记录读取过的对象及其版本（用于 LSA 验证）。
*   `write_set`：记录所有成功挂上去的 `Locator` 指针（用于提交后的清理）。
*   `local_epoch`：缓存当前的 EBR 纪元。

### 2. 读操作实现 (Load with LSA)
步骤如下：
1.  **原子读取**：读取 `TMVar.head`。
2.  **解析值**：利用第三阶段的逻辑解析出实际的 `VersionNode`。
3.  **时间戳检查 (LSA Check)**：
    *   检查 `Node.write_ts`。
    *   如果 `Node.write_ts > Tx.start_ts`：读到了未来的数据，**Self-Abort**（抛出重试异常）。
    *   (优化) 更新 LSA 的 `read_upper_bound`，用于延长事务的快照有效期。

---

## 第五阶段：写操作与 Wound-Wait 策略

这是最复杂的模块，实现了 **Non-blocking** 写。

### 1. 准备阶段
1.  使用 Allocator 分配 `NewNode`，拷贝旧值并应用修改。
2.  使用 Allocator 分配 `Locator`。
3.  设置 `Locator.new_version = NewNode`，`Locator.owner = SelfTx`。

### 2. 冲突检测循环 (CAS Loop)
在一个 `while(true)` 循环中尝试挂载 Locator：
1.  读取 `TMVar.head`。
2.  **分支 A：当前是 Locator (遭遇冲突)**
    *   获取对方 Owner 的 `start_ts` 和 `status`。
    *   **Wound-Wait 裁决**：
        *   若 `Self.ts < Enemy.ts` (我更老)：**Kill Enemy**。执行 `CAS(&Enemy.status, ACTIVE, ABORTED)`。无论成功失败（说明它可能刚提交），都重试循环。
        *   若 `Self.ts > Enemy.ts` (我更年轻)：**Suicide**。执行 Self-Abort，重置自身状态并重试。
    *   **清理**：如果发现对方已 Abort 或 Committed，协助将其 `TMVar.head` 替换回纯 Node（Help-along 协议），然后重试。
3.  **分支 B：当前是 Node (无冲突)**
    *   设置 `Locator.old_version = CurrentNode` (构建 Undo Log)。
    *   执行 `CAS(&TMVar.head, CurrentNode, Tagged(Locator))`。
    *   成功 -> 将 `TMVar` 加入 `write_set`，退出循环。
    *   失败 -> 重试循环。

---

## 第六阶段：提交与回滚协议 (Commit & Abort)

### 1. 提交协议 (Commit Protocol)
1.  **获取提交时间**：原子递增全局时钟，获得 `commit_ts`。
2.  **状态翻转 (The Point of No Return)**：
    *   执行 `CAS(&Self.status, ACTIVE, COMMITTED)`。
    *   **如果失败**：说明被更老的事务“杀”了（Active Abort），转入回滚流程。
3.  **后期处理 (Post-Commit)**：
    *   遍历 `write_set`。
    *   更新 `new_version->write_ts = commit_ts`。
    *   **去除间接层 (Flattening)**：尝试执行 `CAS(&TMVar.head, Locator, new_version)`。
        *   无论成功失败（可能被后续写操作覆盖了），都将 `Locator` 和 `old_version` 传入 **EBR Retire** 队列。
    *   清空读写集。

### 2. 回滚协议 (Abort Protocol)
1.  **状态标记**：设置 `status = ABORTED`。
2.  **资源清理**：
    *   遍历 `write_set`。
    *   **恢复指针**：尝试执行 `CAS(&TMVar.head, Locator, old_version)` (可选，但这能加速后续读取)。
    *   将 `Locator` 和 `new_version` (脏数据) 传入 **EBR Retire** 队列。
    *   **重要**：如果 `old_version` 是从其他事务继承来的，不要释放它；只释放自己分配的内存。

---

## 第七阶段：内存管理集成 (EBR & Allocator)

最后将所有内存操作对接到底层设施。

### 1. 分配 (Allocation)
*   所有 `new Locator()` 和 `new VersionNode()` 必须调用 `ThreadLocalAllocator::alloc()`。
*   避免系统调用 `malloc`，确保指针分配是纳秒级的。

### 2. 回收 (Reclamation)
*   **Retire 时机**：
    *   Commit 后：Retire `Locator` 和 `old_version`。
    *   Abort 后：Retire `Locator` 和 `new_version`。
*   **EBR 周期**：
    *   在每次 `Tx.Begin()` 时调用 `Ebr.Enter()`。
    *   在 `Tx.End()` 时调用 `Ebr.Exit()`。
    *   定期（如每 N 次事务）调用 `Ebr.TryReclaim()` 真正释放物理内存。

---

## 总结：构建清单

按照以下顺序开发和单元测试：
1.  **TaggedPointer** 单元测试（位运算正确性）。
2.  **TMVar** 基础读写测试（单线程）。
3.  **Load** 逻辑测试（模拟 Locator 处于不同状态时的读取结果）。
4.  **Allocator & EBR** 集成测试（确保无内存泄漏）。
5.  **Wound-Wait** 逻辑测试（多线程高冲突场景，验证高优先级事务是否能抢占）。
6.  **基准测试**（Benchmark），对比 `std::mutex` 和 OSTM_WW 的吞吐量。