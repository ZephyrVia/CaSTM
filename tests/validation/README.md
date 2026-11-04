# OccSTM 长时内存与回收验证

`occ_memory_soak.cpp` 是独立验证程序，不为生产 STM/EBR 添加计数接口。
目标是核对生命周期与内存变化，不用于替代热点吞吐实验。

## 负载与对账

每个 worker 独占一个 TMVar，每次事务读取一次，交替执行只读与递增写入。
20 个变量共享真实 EBR 和分配路径。没有其他线程写同一个变量，因此每次成功
写入超过 MAX_HISTORY=8 后恰好退休一个旧版本，可以精确计算应退休数量。
意外提交失败、payload 校验失败、回收不一致都会使运行无效。

每个版本持有一个独立的 shared_ptr payload。计数记录 payload 创建、EBR 回收时
析构，以及变量销毁时保留链上的 payload 析构；**不是生产 VersionNode 的全局
分配计数**。事务内部的临时 shared_ptr 在 guard 退出前释放。慢读者只保留旧节点
裸指针及一个独立析构标志，不用 payload 的引用计数延长被检查对象的生命周期。

同一进程依次运行三个阶段，每阶段默认 600 秒：

| 阶段 | 默认配置 | 检查 |
| --- | --- | --- |
| steady | 20 个固定 worker，目标每 worker 1,000 事务/秒 | 回收在持续版本更新中推进 |
| slow_reader | 同上，加一个读者反复持有 guard 5 秒、恢复 5 秒 | 已摘除旧节点在 guard 内存活，释放后在恢复窗口内析构 |
| thread_churn | 每 5 秒新建并退出 20 个 worker，每线程最多 2,000 次事务 | 槽位/分配器线程生命周期，逐批检查回收与 checksum |

每阶段结束，worker 与慢读者全部退出，推进 16 次 epoch 后对账：
checksum 等于成功写入数，预期退休数等于已回收 payload 数，活对象数等于最多
每变量 9 个保留版本。随后销毁变量、再推进 epoch，要求全部 payload 析构。
churn 在每一批线程退出后也进行上述保留链对账。

## 内存与时间序列

从 Linux `/proc/self/status` 每秒记录 VmRSS、VmHWM、VmSize、RssAnon 和线程数。
慢读者事件和 churn 批次完成时额外采样。RSS/VmSize 使用 KiB；VmHWM 是进程
累计峰值，不是单阶段峰值。没有强制归还 allocator 缓存或调用 malloc_trim。

活跃运行时计数是独立原子量的异步快照，pending 的瞬时差值不能当作严格队列
长度。`quiescent=1` 的行才核对完整计数和 checksum；销毁之后的 checksum 字段
保留最后一次经过验证的值。RSS 或虚拟内存增长需要与对象计数、allocator 策略
共同分析，不能仅凭 RSS 未下降断言 payload 泄漏，也不能仅凭 payload 全析构
断言 allocator 内存有界。

默认资源上限为 RSS 1 GiB、VmSize 8 GiB；触发则记录错误、提前停止阶段并使
结果无效。sanitizer 有大规模影子虚拟内存，其短时检查需单独提高 VmSize 上限。

## 构建与运行

allocator 宏同时传给库与验证目标，防止头文件内联路径不一致：

~~~bash
cmake -S . -B /tmp/castm-memory-native \
  -DCMAKE_BUILD_TYPE=Release -DSTM_ALLOCATOR_MODE=native \
  -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0 -DSTM_ALLOCATOR_MODE=-1"
cmake --build /tmp/castm-memory-native --target occ_memory_soak -j2

cmake -S . -B /tmp/castm-memory-system \
  -DCMAKE_BUILD_TYPE=Release -DSTM_ALLOCATOR_MODE=system \
  -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0 -DSTM_ALLOCATOR_MODE=0"
cmake --build /tmp/castm-memory-system --target occ_memory_soak -j2

python3 scripts/run_memory_soak.py \
  --native-binary /tmp/castm-memory-native/benchmarks/occ_memory_soak \
  --system-binary /tmp/castm-memory-system/benchmarks/occ_memory_soak \
  --output-dir bench/results/occ-memory-soak
~~~

两种分配路径顺序运行，各 30 分钟主负载，合计 60 分钟，另有短暂基线和收尾采样。
输出目录必须尚不存在。metadata 保存精确提交、dirty 状态、源码与二进制哈希、
运行参数和环境；state 保存命令、退出码及进程耗时，sha256 保存原始数据哈希。

~~~bash
python3 scripts/verify_memory_soak.py bench/results/occ-memory-soak
~~~

短时预检可设 `--phase-seconds 3 --threads 4 --sample-ms 100 --hold-ms 300
--recovery-ms 300 --batch-ms 500 --batch-transactions 200 --settle-ms 100`。
短时结果必须使用 `--min-phase-seconds 3` 复核，默认长时校验器会拒绝它。
使用真实短时数据检查校验器能否拒绝错误结果：

~~~bash
STM_MEMORY_SOAK_RESULTS=/tmp/castm-memory-smoke \
  python3 -m unittest discover -s tests/validation -p 'test_*.py' -v
~~~

这套验证仅覆盖给定负载、时长和分配路径，不证明完整隔离级别、任意嵌套 guard、
任意线程数量或无限运行时间下的内存有界性。
