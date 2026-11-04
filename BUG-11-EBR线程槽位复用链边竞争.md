# BUG-11：EBR 线程槽位复用链边竞争

记录日期：2026-11-04，分支 `codex/occ-reclamation-audit`。

[BUG-10](BUG-10-EBR纪元推进与退休桶复用竞争.md) 的修复提交 `52c39dd` 在
[CI #10](https://github.com/ZephyrVia/CaSTM/actions/runs/41771064692) 中，前三项成功，
TSan 仍以 66 退出。远端完整日志未取得；本地先进行了 20 个独立进程、每进程
100 轮的普通调度检查，均通过。将同一二进制限制到两个逻辑 CPU 后，第 13 轮
复现了另一处数据竞争：槽位归还的 `push` 写 `ThreadSlot::next`，与另一个线程
领取槽位的 `pop` 读同一字段重叠。

失败二进制 SHA-256：`005b709a3578a8164e4b694617b0c248f894db82d7542766167b4f8b3dcf4d7a`。
源码为 `52c39dd48bf7fd2e688c210c0d73ffa656cb9e59`。
不能仅凭退出码认定远端具体交错与本地相同；本页结论以本地完整错误栈为依据。

## 原因与修复

原 `LockFreeReuseStack` 使用带 16 位 stamp 的栈头 CAS，但侵入式 next 是普通指针。
某个 pop 在取得旧 head 后可能暂停；另一个 pop 已取走该节点，并在槽位再次归还
时重写 next。前一个 pop 即便最终因 stamp 不同而 CAS 失败，之前对 next 的读取
仍然与重写构成数据竞争。有限位宽的 stamp 也不能提供无限期 ABA 防护。

槽位领取、归还发生在线程生命周期边界，不在已登记线程的每次事务中。修复用
一把互斥锁同时保护栈头与 next 的读写，保留 push/pop 接口和历史类型名称。
此实现不再是无锁栈，不据历史名称宣称 lock-free。槽位数组仍由 manager 持有，
没有借修复改变内存所有权或 EBR 临界区范围。

新增 `EBRManagerTest.ReuseStackPreservesExclusiveOwnership`：4 个线程复用 4 个
节点，完成 80,000 次领取/归还，检查独占所有权及最终每个节点恰好出现一次。
这与真实线程槽位路径的并发回归共同覆盖复用行为；没有用压力通过证明所有交错。

## 验证与复现

Release / mode=0 与 ASan+UBSan 各 64 个用例通过；VERIFY 57 个通过、7 个跳过；
原有 1 个用例保持 disabled。Python 9 个方法通过。ASan 仍关闭泄漏检测。
TSan 使用 system allocator，覆盖 20 个 OccSTM 用例及 2 个 EBR 用例。
普通调度和两个逻辑 CPU 的限制下各重复 100 轮，共 4,400 次执行通过，无 TSan 报告。

构建参数沿用 [BUG-09](BUG-09-OccSTM并发剪枝链边数据竞争.md)，在同一源码上运行：

~~~bash
timeout 180s taskset -c 0,1 setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  /tmp/castm-occ-tsan-after/tests/run_tests \
  --gtest_filter='Occ*:EBRManagerTest.ConcurrentEpochRecyclingKeepsReadersAlive:EBRManagerTest.ReuseStackPreservesExclusiveOwnership' \
  --gtest_repeat=100
~~~

普通调度检查去掉 `taskset -c 0,1`。CPU 编号按本机允许的 CPU 集合选择。
CI 使用同一过滤范围，重复 50 轮（1,100 次），保留失败即退出和原有 timeout。
TSan 验证范围不包含 TierAlloc 独立接口或全仓库，也不是长期线程 churn/RSS 结论。

原始失败、普通调度与双核修复日志，以及源码和日志哈希见
[验证记录](docs/validation/slot-reuse-2026-11-04/result.json)。源码提交
`19d43f6fb84753daea43f7d9bac795d3f395eeb3` 的
[CI #11](https://github.com/ZephyrVia/CaSTM/actions/runs/41772192425) 四项 job 全部通过，
见 [CI 核验记录](docs/validation/slot-reuse-2026-11-04/ci.json)。之后在该干净提交上
重新运行 108 次策略对照，见[最终性能报告](docs/performance/occ-ebr-performance.md)。
