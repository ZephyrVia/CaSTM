# BUG-04：WwSTM published WriteRecord 生命周期协议

状态：部分修复，仍有独立后续问题

相关修复：`7efba79 fix: tryWriteAndGetRecord 回滚协议修复，根除 mode=0 双重释放`

## 原始症状

`STM_WW_VERIFY_LOGIC_MODE=0` 的完整路径曾以约 20% 的概率出现 double-free、invalid
free 或段错误。ASan 观察到同一个 `WriteRecord` 先被直接 `delete`，又被 EBR retire
deleter 回收。

## 原始错误时序

稳定性检查失败后的旧代码使用无条件 `store(nullptr)` 并继续复用同一个记录：

```text
T1: CAS 安装 R1 成功
T1: 稳定性检查失败，尚未回滚
T2: 击伤 T1，偷走 R1，并 retire R1
T1: store(nullptr)，误清掉 T2 的记录
T1: continue，继续复用已经转移所有权的 R1
    -> 再次 retire，或在冲突分支直接 delete
```

核心错误是把“CAS 安装成功”误当成“记录所有权一直归本线程”。发布到共享
`record_ptr_` 后，所有权可能已经通过 steal 协议转移。

## 已完成的修复

`7efba79` 做了两件事：

1. 回滚使用 `CAS(my_record -> nullptr)`，只卸载仍属于自己的记录；CAS 失败时视为
   所有权已转移，不再 delete 或复用该指针。
2. 重试循环每轮复查事务状态；被击伤后，已发布的记录交给偷窃协议，未发布的本地
   记录才可以安全释放。

## 当前边界

这不是 EBR 提前回收问题。即使 EBR 纪元标记正确，如果某个 published `WriteRecord`
仍被错误地留在 `record_ptr_`，上层仍可能访问已退休对象；反过来，直接 `delete` 路径
也可能完全绕过 EBR。

后续取证中仍保留过一类独立的 record 悬垂窗口：确定性仲裁增强了击伤/偷窃交错后，
`record_ptr_` 可能指向已退休记录。该问题不在 BUG-02 的修复范围内。

## 验证

当前分支 `mode=0` 的 `ConcurrentPathIncrement` 100 轮、高竞争测试 50 轮通过；
本轮 EBR 修复没有修改 `tryWriteAndGetRecord()`、steal、COMMITTED 或 partial commit
协议。若后续再出现 Ww UAF，应先按“EBR reclaim”与“direct delete / record reuse”
两条路径分类，不能笼统归因于 EBR。
