# BUG-01：ThreadHeap 线程退出归还活跃 Chunk

状态：已修复

修复提交：`fbd86f1`（当前 `bug/WwSTM-memory-issue` 分支）

## 症状

同一进程顺序运行 `BasicReadWrite` 与 `ConcurrentOrderedList` 时，测试表现为
“挂死”，实际退出码是 OOM killer 的 137；另有 teardown 段错误、垃圾节点 deleter
跳野地址，以及偶发的 WwSTM 段错误。

## 取证

- RSS 约 2 秒增长到 9 GB、4 秒增长到 15 GB，随后被 SIGKILL。
- 链表遍历出现 `0,4,2,1,3` 无限循环，并出现逆序回指和幽灵节点。
- 关闭 EBR 回收后最小复现仍失败，排除“只是 EBR 回收过早”。
- 将 ThreadHeap 分配/释放短路到 `malloc/free` 后最小复现通过，责任域锁定到 TierAlloc。
- 金丝雀在 `~SizeClassPool` 现场报告 `RETURN-LIVE-CHUNK`：chunk 仍有活跃块，
  却在 TLS 析构时被归还 CentralHeap。

## 根因

`SizeClassPool::~SizeClassPool()` 无条件把 `current_slab_`、`partial_list_` 和
`full_list_` 中的 chunk 归还给 CentralHeap。但 chunk 中可能仍有跨线程、跨线程寿命的
对象，例如共享版本链上的 `VersionNode` 和 EBR 单例垃圾链表中的对象。

CentralHeap 将这些 chunk 再分配给其他线程后，placement-new 覆盖仍存活对象，导致：

```text
活对象的 next / deleter 被覆盖
    -> 链表成环或垃圾节点元数据损坏
    -> 无限遍历、RSS 膨胀、野函数指针、段错误
```

`SizeClassPool::deallocate()` 的空板路径与并发 `freeRemote` 也存在相同的生命周期风险。

## 修复

线程退出和空板路径不再销毁或 `returnChunk`：只断开本地链表，chunk 保留并等待后续安全
复用。精确归还需要额外的 quiescence / 引用计数协议，暂不作为本修复的一部分。

这是用常驻内存换取生命周期正确性的保守方案，代价是各 size class 的高水位空间不会
立即归还。

## 调试设施

`TIERALLOC_CANARY` 使用 side bitmap 记录块状态，并提供：

- 双重分配、重复本地/远程释放检测；
- 释放投毒和 UAF-write 复用检查；
- 归还含活块 chunk 检测；
- CentralHeap chunk 二次发放检测。

块状态不能放在块首，因为空闲块首字会被 freelist 链指针占用。

## 验证

该修复与 EBR、WwSTM 路径分开取证；后续 EBR 修复提交的普通构建、`mode=0`、
ASan/UBSan 压力测试均未再出现该类 ThreadHeap chunk 复用故障。
