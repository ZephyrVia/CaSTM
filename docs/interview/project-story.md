# CaSTM 面试讲稿

统一定位：C++17 软件事务内存与并发控制原型。实现细节见
[当前设计](../../OSTM_WW.md)，验证数字见[测试范围](../testing.md)，
性能数据见[实验报告](../performance/README.md)。

## 30 秒版本

CaSTM 是我实现的 C++17 软件事务内存原型。它把共享变量建模为版本化对象，
提供两种不同的并发控制路线：OccSTM 通过乐观验证和重试处理冲突，WwSTM 通过
Wound-Wait 主动中止冲突事务。项目的难点包括 EBR 生命周期、
自研分配器，以及多变量提交在并发 helper 存在时仍然保持原子语义。

## 2 分钟版本

应用层通过 Transaction 或 TxContext 读取和写入 TMVar。OccSTM 在提交阶段检查
read set、write set 和条带锁；它在低冲突下结构简单，但高冲突时可能不断
validation failure 并耗尽 retry guard。

WwSTM 的每个共享变量只有一个 tagged head：

~~~text
head -> VersionNode | WriteRecord
~~~

WriteRecord 保存 owner descriptor、旧版本和新版本。读者根据 descriptor 状态选择
旧版本或新版本，helper 可以把终态 Record 展平回 Node。事务先完成 validation 和
prepare，再用一次 ACTIVE -> COMMITTED CAS 确定逻辑提交结果；后面的 Record -> Node
只是物理清理，因此多变量事务不会因为清理顺序不同而产生 partial commit。

生命周期方面，mode=0 中 reader、owner 和 helper 都可能暂时持有裸指针，所以
published Record 不能直接释放，必须通过 EBR 延迟回收；TxDescriptor 也要等
对应 Record 清理完成后再退休。默认 OccSTM 版本节点使用 TierAlloc，WwSTM 的
VersionNode/WriteRecord 使用系统堆，benchmark 可切换关键路径。对齐要求更高的
descriptor 和 EBR 元数据保持 system allocator 路径。

## 5 分钟深挖

### 故事一：value/version skew 导致 lost update

旧版 WwSTM 同时维护 data_ptr_ 和 record_ptr_。读操作可能从 stable data pointer
拿到 value，再从另一个 record/node 取得 version。并发切换时，value 和 version 不是
同一个逻辑快照，读集验证可能接受错误组合，最终表现为丢失更新。

修复不是给两个指针各加一个更强的 memory order，而是消除双重 authority：把入口收敛
为一个 tagged head。readSnapshot() 对一次 head 观察解析出同一 VersionNode，同时
复制 value 和 version，再复核 head identity；如果 head 发生变化就重试。这样结构上
保证了 value/version 的来源一致。

### 故事二：EBR premature reclaim 导致 UAF

问题表面上常表现为随机段错误、invalid free 或测试挂死。根因是退休对象使用了不
安全的 epoch，或 ThreadHeap 在线程退出时归还仍可能被其他线程观察的 chunk。此时
reader/helper 仍持有裸指针，但回收线程已经开始析构对象。

取证后分开修复了几层责任：登记后复核当前 epoch，退休时读取全局 epoch，
避免落后的线程 epoch 使对象过早进入回收窗口；GarbageNode 元数据独立使用
system allocator，payload 由与其分配方式配对的 deleter 释放；垃圾链表使用互斥保护；
线程退出和空 SizeClassPool 不再暴力归还 chunk。修复后的原则是：retire 只表示
进入延迟队列，真正析构必须等 grace period，且所有解引用都发生在 EBR critical
section 内。当前 ThreadHeap 在线程退出时仍保留部分 chunk，这个修复解决了
过早复用问题，长期线程 churn 下的内存有界性还需要单独验证。

### 故事三：OCC retry storm 与 Wound-Wait 的取舍

统一 benchmark 使用相同的 Shared KV/Array workload、8 个操作的逻辑事务和相同的
线程/读写比例。首次 432-run 矩阵中，WwSTM 108/108 正常完成，OccSTM 97/108；
OccSTM 的 11 次无效运行集中在高冲突 16T/20T，最终 checksum 均匹配。
既有分析将其归为 1,000,000 次 retry guard 耗尽的进展问题；原始 CSV 只保存
exit code 1，没有每次失败的细分类别，所以我会把统计事实与失败归因分开说明。

在 20T、10R/90W、高冲突场景，WwSTM 仍有约 80.7% 的 abort rate 和
882.73 us 的 p99。结果显示它在该矩阵中完成情况更稳定，mutex 则保持更高的绝对
吞吐。主矩阵还保留了两个后端不同的默认分配器，因此性能差异不能全部归因于
冲突算法；单独的 allocator ablation 用于比较关键对象分配路径的影响。

### 面试收束

这个项目的工程过程可以沿着下面的证据链展开：

~~~text
协议设计
  → 并发错误取证
  → 生命周期/线性化修复
  → 针对性回归测试
  → 可复现 benchmark
  → 对 invalid 和 trade-off 做诚实解释
~~~

## 五个常见追问

### 为什么做 STM，它解决什么问题？

我想研究共享状态跨多个变量更新时，如何用事务表达操作边界，以及冲突发生后如何
回滚或重试。项目通过两种并发控制路线比较实现代价，应用目前仍需按各自接口
处理事务状态，尚未形成统一业务 SDK。

### 多变量提交为什么能一起生效？

WwSTM 的多个 Record 引用同一 descriptor。全部 prepare 后，descriptor 的
ACTIVE → COMMITTED CAS 改变它们的逻辑解释，后续展平只做物理清理。
读事务仍需校验读集并成功提交；不能把执行中的任意中间读值当成已接受的一致快照。
对应证据是确定性交错与 MultiVariableAtomicityStress 等回归测试。

### 使用 atomic 后为什么仍会 UAF？

原子更新 head 只控制指针发布。其他线程可能已经读到旧指针，head 改变并不代表
它们停止使用对象。因此 Record、旧节点和 descriptor 都需要按各自生命周期
退休，等 EBR grace period 后才能析构释放。

### 为什么很多场景比 mutex 慢？

这组 workload 每个事务只有八个简单的整数操作，STM 仍要处理版本对象、读写集、
验证、冲突和回收。实验确实显示锁基线通常更快，这些额外路径是合理的开销候选。
我还没有逐项 profiling，不能声称已经确定哪个模块占了多少时间。

### 现在验证到什么程度，下一步先做什么？

本轮 mode=0 与 ASan+UBSan 各 36 个测试通过；VERIFY 有 29 个通过、7 个跳过；
另有 1 个高冲突测试禁用，OccSTM/TierAlloc 独立测试未纳入默认构建。
TSan 本轮未跑，ASan 关闭泄漏检测，线程退出保留 chunk 的边界也仍然存在。
如果继续做性能工作，我会先记录失败原因、重试分布和主要路径耗时，再决定优化点。
