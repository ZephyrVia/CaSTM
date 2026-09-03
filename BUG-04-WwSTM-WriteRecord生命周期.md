# BUG-04：WwSTM published WriteRecord 生命周期错误

状态：已修复

## 症状

`STM_WW_VERIFY_LOGIC_MODE=0` 的 CAS 路径曾以约 20% 的概率出现 double-free、invalid
free 或段错误。ASan 取证显示，同一 `WriteRecord` 可能先从冲突路径直接 `delete`，又被
EBR retire deleter 回收；另一个表现是 reader 仍持有记录时，记录已经被复用或释放。

这不是 EBR 纪元提前回收问题。EBR 纪元修复记录见
[`BUG-02-EBR-retire纪元标记提前回收.md`](BUG-02-EBR-retire纪元标记提前回收.md)。

## 根因

此前 `tryWriteAndGetRecord()` 在稳定性复核失败后只做了回滚 CAS，却继续复用原来的
`my_record`：

```text
ALLOC
→ INSTALL
→ REMOVE
→ REUSE
→ DELETE
→ UAF / double-free
```

`record_ptr_` 从记录改为 `nullptr`，只代表它不再是变量当前记录；已经执行过成功
install CAS 的 reader 仍可能在自己的 EBR 临界区中持有旧裸指针。因此“从共享结构摘除”
不等于“没有读者持有”。

## 生命周期不变量

本轮将 `WriteRecord` 的所有权明确分成两类：

```text
never published candidate
  → 可以复用或直接 delete

published once
  → 不得复用
  → 不得直接 delete
  → 从 record_ptr_ 移除后只能 retire
  → grace period 后恰好 reclaim 一次
```

回滚 CAS 失败仍表示记录已被其他线程接管；当前线程不再 delete、复用或重复 retire，
由窃取方负责退休。

## 修复

修复位于 [`include/WwSTM/TMVar.hpp`](include/WwSTM/TMVar.hpp)：

1. 为当前 candidate 记录 `ever_published` 状态。
2. 所有直接释放出口统一经过 `discard_private_candidate()`，仅允许未发布对象直接
   `delete`；击伤后若记录已发布但已不在 `record_ptr_`，不再碰该指针。
3. 稳定性复核失败且回滚 CAS 成功时，同时 retire `my_new_node` 与 `my_record`，清空
   局部指针并为下一轮分配全新的 candidate。
4. 保留原有回滚 CAS；CAS 失败仍按 ownership 已转移处理。

## 确定性回归

测试位于 [`tests/WwSTM/test_tmvar.cpp`](tests/WwSTM/test_tmvar.cpp)：

- `PublishedWriteRecordRetiresAfterHelper`：用 phase gate 精确排列
  `INSTALL → reader load → stable 改变 → rollback`。reader 活跃期间析构计数为 0，
  离开并经过 grace period 后为 1。
- `PublishedWriteRecordRetiredExactlyOnce`：同一已发布记录重复执行 abort 清理，确认
  第二次调用不会重复 retire，最终析构计数仍为 1。

hook 和析构计数由 `STM_WW_TEST_HOOKS` 控制，只在测试目标中启用；默认库构建不启用
这些同步辅助设施。

## 验证记录

| 验证 | 结果 |
| --- | --- |
| 旧实现反向跑确定性回归 | 失败：reader 活跃时析构计数已为 1，保存的记录不能安全解引用 |
| 修复后 mode=0 两项生命周期回归 | 通过 |
| mode=0 全量 active 测试 ×20 | 22/22 通过 |
| `ConcurrentPathIncrement` ×100 | 100/100 通过 |
| 高竞争 Ww disabled 测试 ×100 | 100/100 通过 |
| EBR 6 项 ×20 | 120/120 通过 |
| VERIFY 全量 ×20 | active 测试全通过；生命周期回归按设计跳过 |
| ASan+UBSan、`detect_leaks=0` 全量 | active 测试通过，无 UAF/double-free/invalid-free |
| ASan+UBSan 生命周期回归 ×20、高竞争 Ww ×20 | 全部通过，无 ASan 内存错误 |

UBSan 仍会报告项目原有的 `TxDescriptor alignas(64)` 对齐问题；本轮按范围未修改，
它不是本 bug 的新增错误。

## 本轮未处理

- single-head 架构重构
- COMMITTED publish window
- partial commit
- TxDescriptor lifetime/alignment
