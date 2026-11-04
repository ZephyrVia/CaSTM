# BUG-10：EBR 纪元推进与退休桶复用竞争

记录日期：2026-11-04，分支 `codex/occ-reclamation-audit`。

后续修复提交 `52c39dd` 的 TSan 仍失败；限制本地 CPU 数量后发现独立的槽位复用
链边竞争，见 [BUG-11](BUG-11-EBR线程槽位复用链边竞争.md)。本页保留桶复用修复的阶段证据。

原子 prev 修复提交 `9bccd33` 的四项 CI 曾通过，随后仅增加性能报告的 `89578c1`
在 [CI #9](https://github.com/ZephyrVia/CaSTM/actions/runs/41769838034) 的 TSan job
失败，其他三项成功。公开页面只显示退出码 66，远端完整日志需要登录，未读取其
错误栈；不能据此断言远端和下面本地复现的具体交错完全相同。

## 本地复现

对原 `9bccd33` 的 TSan 二进制增加重复次数：

~~~bash
timeout 180s setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  /tmp/castm-occ-tsan-after/tests/run_tests --gtest_filter='Occ*' --gtest_repeat=100
~~~

第 10 轮的 `OccReclamationTest.ConcurrentSnapshotTraversalAndPruning` 报告
data race，退出码 66。一侧为 `EBRManager::leave → collectGarbage_ → chainDeleter_`
释放版本节点，另一侧为读者 `Transaction::load` 访问该节点；该读者仍位于 EBR guard
内。[原始日志](docs/validation/ebr-reclamation-2026-11-04/tsan-before.txt)保留调用栈。
这是节点生命周期问题，区别于 [BUG-09](BUG-09-OccSTM并发剪枝链边数据竞争.md)
已经修复的普通链边指针读写竞争。一次 10 轮通过不足以排除它。

## 代码中允许的危险交错

旧实现先 CAS 推进 global epoch，再重新读取全局纪元，最后按 `(epoch - 2) % 3`
摘取退休链表。桶的互斥锁只保护 push/steal 本身，没有将推进与摘取绑定在一起。
即使把后一次全局读取换成 CAS 的结果，延迟摘取仍可能遇到桶被复用。

下面是代码允许的一种交错，用于说明缺口，不是从 TSan 还原的完整执行轨迹：

1. A 推进 2 → 3，准备收集 epoch 1 的桶（index 1），在摘取前暂停。
2. B 推进 3 → 4，正常处理 epoch 2 的桶。
3. 读者在 epoch 4 进入并持有 P；写者摘除 P，以 epoch 4 退休到同一个 index 1。
4. 其他线程可推进到 5，但 epoch 4 的读者会阻止继续推进到 6。
5. A 恢复，按旧任务摘取 index 1，误把 epoch 4 的 P 交给 deleter。

P 没有经过所需的 grace period。单独保护桶链表或只保证退休时使用全局纪元，
都不能关闭这个跨代复用窗口。

## 修复

`tryAdvanceAndDetach_` 用同一把短期互斥锁串行化扫描、纪元推进和过期桶摘取：

- 以 try_lock 进入；锁忙时跳过本次推进，后续 leave 仍可尝试。
- 收集目标使用本次成功推进得到的精确纪元，不再另读可能变化的 global epoch。
- 在允许下一次推进之前，把符合回收条件的整个链表摘为私有批次。
- 释放推进锁之后再执行 deleter，避免持锁运行用户析构函数。

retire 仍按退休时全局纪元入桶，桶内 push/steal 仍使用已有同步；活跃读者阻止推进
的规则保持原样。这是 correctness 修复，新增锁的性能影响需要在新提交上重新测量。

## 回归与验证范围

新增 `EBRManagerTest.ConcurrentEpochRecyclingKeepsReadersAlive`：4 个读者读取并
跨 yield 保留系统堆对象，1 个写者进行 2,000 次替换/退休，另有 2 个线程反复进入/
退出临界区。检查不可变 payload，最终确认 2,001 个对象全部析构。它是并发压力
回归，不宣称每次确定性命中上述五步交错。

| 本地验证 | 结果 |
| --- | --- |
| Release / mode=0 | 63 passed、1 disabled |
| Debug / VERIFY | 56 passed、7 skipped、1 disabled |
| ASan+UBSan / mode=0 | 63 passed、1 disabled；leak detection 关闭 |
| Python runner | 9 个方法通过，含四后端短时计数对账 |
| TSan / system allocator | 20 个 OccSTM 用例加新增 EBR 用例，100 轮共 2,100 次通过 |

记录和日志哈希见 [result.json](docs/validation/ebr-reclamation-2026-11-04/result.json)，
[修复后 TSan 日志](docs/validation/ebr-reclamation-2026-11-04/tsan-after.txt.gz)为原始日志压缩件。
CI 的 TSan 范围同步加入该 EBR 用例，重复 50 轮（1,050 次），保留 120 秒 timeout。
不降低检查强度、不屏蔽 TSan 报告。本地结果不能代替后续精确提交的远端 CI 状态。

TSan 复现使用与 BUG-09 相同的构建选项，运行命令改为：

~~~bash
timeout 180s setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  /tmp/castm-occ-tsan-after/tests/run_tests \
  --gtest_filter='Occ*:EBRManagerTest.ConcurrentEpochRecyclingKeepsReadersAlive' \
  --gtest_repeat=100
~~~

本轮没有证明完整 EBR 内存模型、无饥饿或任意嵌套 guard 的行为。长时 RSS、慢读者
和线程 churn 仍需单独测试。`9bccd33` 的性能归档保留为含此已知问题的中间版本，
不能把其 93.1 万 TPS 当作本次 EBR 修复的性能结果。
