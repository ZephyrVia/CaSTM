# BUG-02：EBR retire 纪元标记错误导致提前回收

状态：已修复

修复提交：`6f5bcc5 fix(ebr): tag retired objects with global epoch`

相关前序提交：`2bf708e`（登记复核、账本迁移系统堆、垃圾链表互斥）

## 根因现场

在一次 ASan 现场中：

```text
对象实际 retire 时 global_epoch = 2849
retire() 却读取线程 announced_epoch = 2848
对象进入 bucket 2848
global_epoch 到 2850 后 collect(2850 - 2) = collect(2848)
```

此时另一个 reader 仍然 active，announced epoch 为 2849，并继续访问退休对象，触发
`readProxy` 中的 use-after-free。

问题不是 reader 没有进入 EBR，而是 retire 账本把对象记到了比实际退休时刻更老的桶。

## EBR 模型

当前实现是三桶轮转的 grace-period 模型，而不是简单的“两 epoch”模型：

1. 对象在全局纪元 `R` 退休，进入 `R % 3` 桶；
2. 当没有 active 且 announced epoch 小于当前全局纪元的槽位时，global epoch 推进一代；
3. 推进到 `G` 后，回收 `G-2` 对应的桶。

因此，`collect(G-2)` 的正确性前提是：对象必须按实际逻辑退休时的全局纪元入桶，不能
因为退休线程的 announced epoch 落后一代而进入更老的桶。

## 正确性推导

若 reader 在 `E+1` active，并在对象退休于 `E+1` 前观察到对象：

```text
对象进入 bucket E+1
G=E+2 时只 collect bucket E
G=E+3 时才可能 collect bucket E+1
```

而该 reader 在 `E+2` 仍是落后槽位，会阻止 `E+2 -> E+3` 的推进，直到它离开。
所以对象不会在 reader 仍 active 时被回收。

若 reader 在 `E` active、对象也退休于 `E`，则 reader 在全局推进到 `E+1` 后成为落后者，
会阻止到达 `E+2`，同样保证安全。

## 补丁

`EBRManager::retire()` 现在只读取退休时的全局纪元：

```cpp
uint64_t retire_epoch =
    global_epoch_.load(std::memory_order_acquire);
garbage_lists_[retire_epoch % kNumEpochLists].pushNode(g_node);
```

本轮没有修改 WwSTM 事务算法、allocator、descriptor 或线程注册结构，也没有增加全局
锁或 retire 扫描。

## 确定性回归

`tests/EBRManager/test_EBRManager.cpp` 使用 C++17 phase gate 和析构计数器，不依赖 sleep：

- `RetireUsesGlobalEpochWhenOwnerSlotIsBehind`：活跃 `E+1` reader 在 `E+2` 时析构计数必须为 0；
- `RetireAtLaggingSlotUsesCurrentGlobalEpoch`：隔离验证 slot=`E`、global=`E+1` 时对象进入当前全局桶；
- `RetireAtCurrentGlobalEpochUsesNormalGracePeriod`：验证同 epoch retire 仍按正常两纪元回收。

旧实现运行第一项时实际得到 `destruction_count == 1`，修复后通过。

## 验证

- EBR 6 项 ×100 轮通过；
- Debug/VERIFY 全量 20/20 通过；
- `mode=0` 全量 20/20 通过；
- ASan/UBSan 下 EBR 6 项 ×20 通过；完整 21 项运行无 UAF、double-free 或 invalid-free。

完整 sanitizer 仍报告已知的 `TxDescriptor alignas(64)` 对齐警告，本轮未处理。
