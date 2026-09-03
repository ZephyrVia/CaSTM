# BUG-05：Wound-Wait 平局裁决的堆地址顺序依赖

状态：main 谱系已修复；`bug/WwSTM-memory-issue` 分支暂缓纳入

main 谱系修复提交：`8b3cdf5 fix: Wound-Wait 平局裁决改用创建序号，消除堆地址彩票`

## 症状

main 的 `WoundWait_OldKillsYoung` / `WoundWait_YoungDies` 在全量套件中会失败，
单独运行却能通过，表现为明显的测试顺序依赖。

## 根因

`GlobalClock::now()` 只是读取原子计数器，只有 commit 的 `tick()` 才推进时钟。测试中
两个事务之间的 `sleep_for(2ms)` 并不会改变 `start_ts`，两个事务可能拿到相同时间戳。

原平局裁决使用：

```cpp
if (my_ts == enemy_ts)
    i_am_older = (my_desc_ < conflict_tx);
```

`TxDescriptor*` 的堆地址取决于此前测试对分配器的扰动。于是同一组事务的老幼关系变成
地址彩票：单独运行时碰巧通过，全量运行时堆布局改变便翻转。

失败现场曾看到两条 `StartTS:2`，与该机制一致。

## 修复方向

在 descriptor 创建时盖章单调递增的 `creation_serial`，平局时按创建序号裁决，不能
比较堆地址；测试也应停止用 sleep 假设逻辑时钟会推进，必要时显式制造一次 tick。

该修复会改变击伤/偷窃交错分布，因此 bug 分支上暂不与当前 Ww 写路径协议修复混合，
待 BUG-04 的生命周期协议单独收口后再决定合流。
