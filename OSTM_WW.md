# OSTM_WW 设计规范文档
**Object-based Software Transactional Memory with Wound-Wait**

## 1. 概述与核心目标

本项目实现了一个基于 C++ 的高性能软件事务内存 (STM) 系统，命名为 **OSTM_WW**。该系统专为满足苛刻的并发控制要求而设计，核心特征如下：

*   **无锁设计 (Non-blocking / Obstruction-Free)**：摒弃互斥锁 (`std::mutex`)，全链路采用原子操作 (CAS) 和间接层 (Indirection) 实现。
*   **主动抢占 (Active Abort)**：采用 **Wound-Wait** 策略，允许高优先级事务主动中止低优先级事务，彻底消除死锁，保证进展。
*   **指针式 Undo Log**：利用数据结构的 **异地更新 (Out-of-Place Update)** 特性，旧版本指针天然构成 Undo Log，实现零拷贝回滚。
*   **MVCC 与时间戳验证**：基于 **LSA (Lazy Snapshot Algorithm)**，读操作利用多版本特性读取一致性快照，不阻塞写操作。
*   **内存安全**：结合 **EBR (Epoch-Based Reclamation)** 和 **Thread-Local Lock-Free Allocator**，解决无锁环境下的内存回收与性能瓶颈问题。

---

## 2. 核心数据结构设计

系统的核心思想是 **"Indirection" (间接层)**。共享变量不再直接指向数据，而是可能指向一个“定位器 (Locator)”，该定位器充当了动态的锁和日志容器。

### 2.1 事务描述符 (Transaction Descriptor)
事务的唯一标识，也是并发控制的核心“开关”。

*   **`uint64_t start_ts`**：事务开始的时间戳。用于 **MVCC 读可见性判断** 和 **Wound-Wait 优先级裁决**（时间戳越小，优先级越高，越老越强）。
*   **`std::atomic<TxStatus> status`**：原子状态机。
    *   `ACTIVE`: 事务正在运行。
    *   `COMMITTED`: 事务已提交，修改永久生效。
    *   `ABORTED`: 事务已回滚，修改全部作废。
    *   **关键点**：此状态允许被**其他线程**修改（实现 Active Abort）。

### 2.2 定位器 (Locator) —— 动态锁与日志
每次写操作都会分配一个新的 Locator（利用 Lock-free Allocator 低成本分配）。

*   **`TransactionDescriptor* owner`**：指向发起修改的事务。
*   **`VersionNode* old_version`**：**【Undo Log】**。指向修改前的旧数据节点。
    *   若事务 Abort，此指针即为回滚后的数据。
    *   并发读事务利用此指针实现无锁快照读。
*   **`VersionNode* new_version`**：**【Redo Log】**。指向新分配的、包含新值的数据节点。
    *   仅当 owner 状态变为 COMMITTED 时，此数据才对外可见。

### 2.3 共享变量 (TMVar) 与 标记指针 (Tagged Pointer)
*   **`std::atomic<uintptr_t> head`**：全局共享的指针。
*   **Tagged Pointer 机制**：利用指针末位 bit 区分当前指向的是 **稳态数据 (VersionNode)** 还是 **暂态结构 (Locator)**。
    *   `Bit 0 == 0`: 指向 `VersionNode`（无并发写，直接读）。
    *   `Bit 0 == 1`: 指向 `Locator`（有并发写，需解引用判断）。

---

## 3. 内存管理模型

本设计依赖两大支柱来解决 Out-of-Place Update 带来的内存挑战：

### 3.1 物理内存分配 (Allocation)
*   **策略**：**Thread-Local Lock-Free Allocator**。
*   **必要性**：由于每次写操作都要 new 一个 `Locator` 和 `VersionNode`，传统的 `malloc` 会成为瓶颈。TLS 分配器将分配成本降至指针移动级别。

### 3.2 内存安全回收 (Reclamation)
*   **策略**：**EBR (Epoch-Based Reclamation)**。
*   **逻辑分离**：
    *   **事务时间戳 (Global Clock)**：负责逻辑正确性（MVCC 版本可见性），每次提交推进。
    *   **EBR 纪元 (Global Epoch)**：负责物理内存安全，批量延迟推进。
*   **流程**：旧版本（Undo Log）在不再被引用时，不是立即 `delete`，而是 `retire()` 给 EBR，等待所有读线程离开旧纪元后安全释放。

---

## 4. 算法流程详解

### 4.1 读操作 (Load) —— MVCC + LSA
**原则**：读操作永远不修改共享内存，永远不阻塞。

1.  **读取 Head**：原子加载 `TMVar.head`。
2.  **类型判断**：
    *   **是 Node**：直接获取该 Node。
    *   **是 Locator**：读取 `Locator.owner.status`。
        *   `COMMITTED` -> 读 `new_version`。
        *   `ABORTED` -> 读 `old_version`。
        *   `ACTIVE` -> **发生冲突**。为了不等待，根据 MVCC 规则，直接读取 `old_version` (快照读)。
3.  **时间戳验证 (LSA)**：
    *   获取所读 Node 的 `write_ts`。
    *   **校验**：若 `node.write_ts > tx.start_ts`，说明读到了“未来”的数据，违反一致性，当前事务自杀 (Abort)。
    *   **更新快照区间**：根据 LSA 算法更新当前事务的有效时间区间。

### 4.2 写操作 (Store) —— 遭遇式写 + Active Abort
**原则**：异地更新 (Out-of-Place)，Wound-Wait 策略解决冲突。

1.  **准备阶段**：
    *   分配 `NewNode` (写入新值)。
    *   分配 `Locator`，设置 `NewNode` 指针，`Owner` 指向自己。
2.  **冲突检测循环 (CAS Loop)**：
    *   读取 `TMVar.head`。
    *   **若当前是 Locator (遭遇冲突)**：
        *   获取对方 Owner。
        *   **Wound-Wait 裁决**：
            *   若 `Me.start_ts < Enemy.start_ts` (我更老，优先级高)：**Active Abort**。原子地将 `Enemy.status` 设为 `ABORTED`。我不等待，直接尝试覆盖它的 Locator。
            *   若 `Me.start_ts > Enemy.start_ts` (我年轻)：自杀 (Self-Abort)，抛出重试异常。
        *   **设置 Undo Log**：解析出当前的逻辑有效值，赋值给 `Locator.old_version`。
    *   **若当前是 Node (无冲突)**：
        *   直接将该 Node 赋值给 `Locator.old_version`。
3.  **原子上位**：
    *   执行 `CAS(expected=old_ptr, desired=locator_ptr)`。
    *   成功则持有该变量，失败则重试循环。

### 4.3 提交 (Commit)
**原则**：单点原子生效。

1.  **获取提交时间戳**：`commit_ts = GlobalClock.fetch_add(1)`。
2.  **状态翻转**：`CAS(status, ACTIVE, COMMITTED)`。
    *   成功：所有被我挂上 Locator 的变量，瞬间对外展示为 `NewNode`。
    *   失败（被别人 Active Abort）：抛出异常进行清理。
3.  **后处理 (Post-Commit Cleanup)**：
    *   遍历写集，尝试将 `TMVar.head` 从 `Locator` 替换回 `NewNode` (去除间接层，加速后续读取)。
    *   将替换下来的 `Locator` 和 `OldNode` 提交给 **EBR** 回收。

### 4.4 回滚 (Abort)
**原则**：逻辑回滚，零拷贝。

1.  **状态翻转**：`status = ABORTED`。
    *   此时，所有读者看到我的 Locator，都会自动去读 `old_version`。
2.  **后处理**：
    *   (可选) 尝试将 `TMVar.head` 恢复指向 `OldNode`。
    *   将废弃的 `NewNode` 和 `Locator` 提交给 **EBR** 回收。

---

## 5. 关键机制深度解析

### 5.1 为什么说这是 Undo Log？
传统数据库的 Undo Log 记录的是“操作的反向逻辑”或“旧值拷贝”。本设计的 `Locator.old_version` 指针保留了**旧对象的完整物理引用**。
*   当事务运行时，这个指针就是 Undo Log，保证了旧数据的留存。
*   当事务回滚时，系统通过指针切换（或读逻辑路由）回到旧对象，实现了**逻辑上的 Undo 回放**。
*   **优势**：避免了 `memcpy` 的开销，实现了 O(1) 复杂度的回滚。

### 5.2 为什么说这是 Active Abort？
在传统的基于锁的 STM 中，遇到冲突通常只能等待锁释放。而在本设计中：
*   我们利用 `TxDesc` 暴露出的原子 `status`。
*   当高优先级事务发现资源被低优先级事务占有时，它可以**直接修改对方内存中的状态** (`CAS(&enemy.status, ACTIVE, ABORTED)`)。
*   这就是“主动杀人”，确保了高优先级事务永远不会被阻塞（Non-blocking），满足了 Obstruction-Free 甚至 Wait-Free (针对写操作) 的特性。

### 5.3 为什么必须使用 EBR？
由于采用了 **New Node** 模式，旧节点被替换下来后，可能仍有并发读事务持有其指针（MVCC 特性）。
*   不能直接 `delete`，否则读线程会 Segfault。
*   不能用引用计数，效率太低。
*   **EBR** 提供了一个“安全删除”的缓冲区，只有确信没有读线程处于旧纪元时，才真正释放内存。这是 C++ 无锁 STM 能够正确运行的基石。

---

## 6. 总结

**OSTM_WW** 是一个高度优化的无锁事务引擎。它通过 **Locator 间接层** 实现了无锁的读写并发，通过 **指针式 Undo Log** 降低了回滚开销，通过 **Wound-Wait Active Abort** 解决了死锁与饥饿问题，并利用 **EBR** 和 **Lock-free Allocator** 解决了内存管理的痛点。该设计完全符合高性能、强一致性及非阻塞的学术与工程要求。