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

随后在 `codex/perf-diagnostics` 分支补上失败原因、预热/测量分离和逐线程重试
直方图。新的 108 次同二进制退避对照明确记录了 6 次 retry_limit；20T、50R/50W、
16 keys 的立即重试 0/3 次有效，失败尝试中 99.94% 首先检测到读锁冲突。
有界随机退避让该点 3/3 次有效，测量窗口内单逻辑事务最大重试为 50～59 次。
同时保留了单线程低冲突吞吐下降约 1.5%～2.6% 的结果。这个对照支持该 workload
中的冲突放大解释，不证明无饥饿，也不能回填历史失败日志缺失的细节。
[报告和数据](../performance/occ-retry-diagnostics.md)可以逐项核验。

随后审计发现，EBR 只延迟释放节点，不能消除普通 prev 指针在读者遍历与写者剪枝
之间的数据竞争。先用 TSan 复现，再改为原子链边并补充回归；修复提交 `9bccd33`
的四项远端 CI 通过后，按相同配置重新做了 108 次性能对照。
本轮 none 为 31/36 次有效，yield/jitter 各 36/36；上述 20T、50R/50W 热点场景
仍由 0/3 提升到 3/3，jitter 平均 930,577 TPS，p99 均值 446.65 μs，单逻辑事务
最大重试为 67～87 次。当前版本应引用[修复后报告](../performance/occ-postfix-performance.md)。
本轮 20 线程低冲突的 jitter 吞吐较 none 下降 0.42%～3.89%；与旧批次相比的下降
也完整保留，没有用两批独立实验直接量化原子链边修复成本。

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

追加 EBR 修复后 mode=0 与 ASan+UBSan 各 64 个测试通过；VERIFY 有 57 个通过、7 个跳过；
另有 1 个高冲突测试禁用。新增 OccSTM 的 TMVar/Transaction 与诊断回归；
其他历史 OccSTM/TierAlloc 测试仍未全部纳入默认构建。
TSan 在 system allocator 下验证了 20 个 OccSTM 用例及 2 个 EBR 用例。
已复现并修复读者遍历与写者剪断普通 prev 指针的竞争；证据见 [BUG-09](../../BUG-09-OccSTM并发剪枝链边数据竞争.md)。
修复提交的四项远端 CI 已通过，并已归档该干净提交的性能复测与逐线程诊断。
这条通过记录指 `9bccd33`；随后仅文档提交的 TSan 失败暴露了覆盖不足。本地进一步
复现节点读取与释放竞争，并修复纪元推进和桶摘取之间的跨代复用窗口，见
[BUG-10](../../BUG-10-EBR纪元推进与退休桶复用竞争.md)。此前性能保留为中间版本记录。
限制到两个逻辑 CPU 后，又复现并修复槽位复用链边竞争，见
[BUG-11](../../BUG-11-EBR线程槽位复用链边竞争.md)；普通调度通过不能替代不同调度条件的检查。
ASan 关闭泄漏检测，线程退出保留 chunk 的边界也仍然存在。
失败原因、退避对照和已发现的历史链边竞争修复已完成。下一步测长时运行与 RSS，
进一步检查慢读者与线程 churn；新的热点路径优化需要单独 profiling 后决定。
