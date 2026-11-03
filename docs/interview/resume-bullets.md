# CaSTM 简历项目描述

项目名称：**CaSTM：C++17 软件事务内存与并发控制原型**。
以下内容对应已有实现；[测试范围](../testing.md)与
[性能报告及原始数据](../performance/README.md)可在仓库中核验。
性能数字必须保留 workload、机器和有效性条件。

## 中文

- 实现 C++17 软件事务内存原型，提供 OccSTM 乐观验证与 WwSTM 主动冲突裁决两条路线，支持版本化共享变量的事务读写。
- 设计 WwSTM single-head Locator，统一版本节点与写记录入口；以 descriptor 状态 CAS 确定多变量事务的逻辑提交，再由 owner/helper 完成物理清理。
- 实现 EBR 延迟回收和分层线程分配器，定位并修复提前回收、已发布写记录生命周期及 descriptor 对齐问题，补充确定性交错与并发回归测试。
- 建立四后端统一事务基准，完成 432 次矩阵运行和 60 次分配器对照；分别记录吞吐、尾延迟、重试与 checksum，保留无效运行并分析收益与退化边界。

若岗位更重视性能，可用以下一句替换第四条中的部分内容：

> 在 WSL2 / i5-14600KF 的 8 线程、50R/50W、65,536 keys 场景中，对 WwSTM
> 关键对象切换 TierAlloc 后，5 次独立运行的平均吞吐较系统分配器提高约 30.3%；
> 同时记录高冲突配置下的性能退化。

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

- benchmark 是历史的单机 WSL2、Intel i5-14600KF、20 logical CPUs、GCC 11.4、Release -O3 结果；短时运行检查不作为新的性能结论。
- WwSTM 108/108、OccSTM 97/108 是主矩阵中的有效运行次数。无效行的原始错误只有 exit code 1，重试耗尽归因及其诊断限制见报告。
- 锁基线通常更快；现有数据不能证明普遍加速、线性扩展或无饥饿。
- 当前包含互斥锁，验证只覆盖明确列出的测试；mode=0/ASan+UBSan 各 36 个通过，VERIFY 29 个通过、7 个跳过，另有 1 个禁用用例。
- TSan 本轮未验证，ASan 关闭泄漏检测；TierAlloc 仍有线程退出保留 chunk 的策略，不能写成完整无泄漏或生产就绪。
