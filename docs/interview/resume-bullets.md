# CaSTM 简历项目描述

项目名称：**CaSTM：C++17 软件事务内存与并发控制原型**。
以下内容对应已有实现；[测试范围](../testing.md)与
[最终修复后的性能复测](../performance/occ-ebr-performance.md)及
[历史性能报告](../performance/README.md)可在仓库中核验。
性能数字必须保留 workload、机器和有效性条件。

当前性能数字对应四项 CI 通过的最终源码 `19d43f6`；早期和中间版本的数据保留为
历史记录，不用它们直接计算 EBR 修复的开销。

## 中文

- 实现 C++17 软件事务内存原型，提供 OccSTM 乐观验证与 WwSTM 主动冲突裁决两条路线，支持版本化共享变量的事务读写。
- 设计 WwSTM single-head Locator，统一版本节点与写记录入口；以 descriptor 状态 CAS 确定多变量事务的逻辑提交，再由 owner/helper 完成物理清理。
- 实现 EBR 延迟回收和分层线程分配器，定位并修复提前回收、已发布写记录生命周期及 descriptor 对齐问题，补充确定性交错与并发回归测试。
- 建立四后端统一事务基准，完成 432 次矩阵运行和 60 次分配器对照；分别记录吞吐、尾延迟、重试与 checksum，保留无效运行并分析收益与退化边界。

若岗位更重视性能，可用以下一句替换第四条中的部分内容：

> 在 WSL2 / i5-14600KF 的 8 线程、50R/50W、65,536 keys 场景中，对 WwSTM
> 关键对象切换 TierAlloc 后，5 次独立运行的平均吞吐较系统分配器提高约 30.3%；
> 同时记录高冲突配置下的性能退化。

本分支新增的性能诊断经历也可以作为替换项，数据见[最终修复后的退避对照](../performance/occ-ebr-performance.md)：

> 补齐 OccSTM 失败分类与逐线程重试统计，定位热点读锁冲突引发的重试风暴，
> 实现有界随机退避，修复并发剪枝及 EBR 竞争后完成 108 次策略对照；在本机 20T、50R/50W、
> 16 keys 场景中，有效运行由 0/3 提升为 3/3，平均约 90.6 万 TPS，同时记录低冲突下的吞吐退化。

## English

- Implemented CaSTM, a C++17 software transactional memory prototype with OccSTM optimistic validation/retry and WwSTM active conflict management over versioned shared variables.
- Designed a single-head WwSTM Locator with a descriptor status CAS as the logical commit point for multi-variable transactions, followed by owner/helper physical cleanup.
- Implemented EBR deferred reclamation and a tiered thread allocator; diagnosed premature reclamation, published-record lifetime and descriptor alignment bugs, with deterministic interleaving and concurrent regression tests.
- Built a common four-backend transaction benchmark with 432 matrix runs and 60 allocator-ablation runs, recording throughput, tail latency, retries and checksums while preserving invalid runs and performance regressions.

## 岗位侧重点

- C++ 后端：共享状态的多对象原子更新、事务失败处理与并发问题定位。
- 基础架构：对象生命周期、EBR、分配路径及实验方法。
- 存储：内存事务的提交点、版本管理与冲突控制；当前范围不含持久化、恢复或存储服务。

## 面试时的限定说明

- 历史矩阵和新的退避对照均来自单机 WSL2、Intel i5-14600KF、GCC 11.4、Release -O3，但参数与源码不同，不能混合成同一次实验。
- WwSTM 108/108、OccSTM 97/108 是主矩阵中的有效运行次数。无效行的原始错误只有 exit code 1，重试耗尽归因及其诊断限制见报告。
- 锁基线通常更快；现有数据不能证明普遍加速、线性扩展或无饥饿。
- 当前包含互斥锁，验证只覆盖明确列出的测试；追加 EBR 修复后 mode=0/ASan+UBSan 各 64 个通过，VERIFY 57 个通过、7 个跳过，另有 1 个禁用用例。
- TSan 仅验证 20 个 OccSTM 用例及 2 个 EBR 用例和 system allocator；ASan 关闭泄漏检测，TierAlloc 仍在线程退出时保留部分 chunk，不能写成全仓库无竞争、完整无泄漏或生产就绪。
- 最终源码 `19d43f6` 四项远端 CI 通过；其 108 次性能复测中 none 为 30/36 次有效，yield/jitter 各 36/36。修复前含已知竞争的独立批次不能用来宣称 EBR 修复的开销百分比。
