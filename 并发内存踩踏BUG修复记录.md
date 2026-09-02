# 并发内存踩踏 BUG 修复记录（2026-09）

> 本文完整记录一次跨 `main` 与 `bug/WwSTM-memory-issue` 两个分支的并发缺陷诊断与修复：
> 从两个"玄学"故障（测试挂死 / 稳定段错误）出发，最终定位出 **5 个独立缺陷**，
> 每一个都有实锤证据链与量化验收数据。文中所有行号以修复前的代码为准。

---

## 0. 总览

| # | 缺陷 | 位置 | 症状 | 修复提交 |
|---|------|------|------|----------|
| 1 | 线程退出暴力归还含活块 chunk | `SizeClassPool.cpp` | 链表成环→OOM"挂死"、teardown 段错误、deleter 跳野地址 | `a988256` |
| 2 | EBR 协议四处弱点 | `EBRManager.cpp` 等 | 加剧 1 的爆炸半径 | `f128e84` |
| 3 | retire 默认 deleter 与分配方式不配对 | `EBRManager.hpp` | `free(): invalid pointer` | `a345e04` |
| 4 | tryWriteAndGetRecord 回滚协议缺陷 | `WwSTM/TMVar.hpp` | mode=0 约 20% 双重释放 | `7efba79` |
| 5 | Wound-Wait 平局裁决用堆地址 | `WwSTM/TxContext.hpp` | 两个 WoundWait 测试顺序依赖失败 | `8b3cdf5` |

**因果主线**：缺陷 1 是 main 上两个故障的共同根因——被复用的 chunk 上，
`VersionNode` 的 payload（链表 `next`）被垃圾数据覆盖 → 链表成环 → 无限遍历 →
读集 vector 无限增长 → RSS 4 秒 15GB → OOM killer SIGKILL（表象是"挂死"，
退出码 137 曾被长期误读为外部 kill）。

---

## 1. 症状与第一现场

### 1.1 症状清单（修复前）

| 症状 | 复现条件 | 实际退码 |
|------|----------|----------|
| 全量套件"挂死"在 `STMTest.ConcurrentOrderedList` | main，单进程全量运行 | 137（OOM SIGKILL） |
| `WwSTMCleanupTest.Debug_GraphDetection` 段错误 | main，单独运行稳定复现 | 139 |
| `~GarbageNode` 内 deleter 跳野地址 `0x5555000003e9` | main，全量 + gdb | 139 |
| mode=0 完整路径约 20% 失败（`double free or corruption` / 断言失败） | bug 分支 | 134 / 139 / 1 |

### 1.2 关键诊断实验（按时间序）

| 实验 | 操作 | 结果 | 结论 |
|------|------|------|------|
| RSS 监控 | 挂死进程采样 | 2s→9GB，4s→15GB，随后 SIGKILL | 不是挂死，是内存无限膨胀 |
| 遍历探针（gdb 打印节点值） | 打印 `ConcurrentOrderedList` 遍历序列 | val=0,4,2,1,3 无限循环，含逆序回指边（3→0、4→2），闪现幽灵节点 val=6 | **链表 payload 被内存复用写坏**，OCC 读集验证逻辑上不可能提交逆序链接 |
| E1：关闭 EBR 回收 | 注释 `collectGarbage_` 调用 | 最小对（`BasicReadWrite`+`ConcurrentOrderedList`）仍 10/10 失败 | **排除 EBR 回收路径**（此前的主假设被证伪） |
| B2：分配器短路 | `ThreadHeap::allocate/deallocate` 转发 malloc/free | 最小对 10/10 通过 | **锁定 TierAlloc 分配器** |
| 金丝雀 | 见 §5 | `RETURN-LIVE-CHUNK`，slab bs=24 block#0 state=1，栈：`~SizeClassPool ← __GI___call_tls_dtors ← start_thread` | **根因实锤：线程退出析构归还含活块 chunk** |

**最小复现对**：`STMTest.BasicReadWrite` + `STMTest.ConcurrentOrderedList` 同进程 =
100% 失败；任一单独跑 = 100% 通过。机制：4 个插入线程中先做完的那个**提前退出**，
其 thread_local ThreadHeap 析构时 chunk 里还装着全局存活对象。

---

## 2. 缺陷 1：`SizeClassPool` 线程退出暴力归还 chunk（根因）

### 原因

`src/TierAlloc/ThreadHeap/SizeClassPool.cpp` 的 `~SizeClassPool()`（原代码自己标注了
`[KNOWN LIMITATION / 已知缺陷]`）在线程退出时无条件把 `current_slab_`、
`partial_list_`、`full_list_` 里的 chunk 全部 `returnChunk` 给 CentralHeap，
而 chunk 中仍有**生命周期超出本线程**的对象：

- 共享 `TMVar` 版本链上的 `VersionNode`（提交线程可能是任何 worker）；
- EBR **全局**垃圾链表里的 `GarbageNode`（挂在单例上，与本线程同寿命？不——比线程长）。

CentralHeap 把 chunk 重新发给存活线程后，新分配（placement-new）直接覆盖活对象：
`next` 指针变成垃圾数据 → 链表成环；`GarbageNode::deleter` 变成数据碎片 →
回收时跳野地址执行；Ww teardown 遍历树踩到坏指针 → 段错误。
三种症状同源。

另外 `SizeClassPool::deallocate()` 的空板路径（`Destroy + returnChunk`）与并发
`freeRemote` 之间存在窗口：其他线程的远程释放可能仍指向本 chunk 的块，归还同样危险。

### 解决

```cpp
// ~SizeClassPool：只断开本地链表，chunk 保留到进程结束（空间换安全）
current_slab_ = nullptr;
while (!partial_list_.empty()) partial_list_.pop_front();
while (!full_list_.empty())    full_list_.pop_front();

// deallocate 空板路径：不销毁不归还，空板留在 partial_list_ 中复用
```

精确的按引用计数归还需要 quiescence 协议，列为后续优化；当前策略内存代价 =
各 size class 的高水位，对测试套件和常驻服务可接受。

---

## 3. 缺陷 2：EBR 协议四处弱点

### 3.1 `enter()` 两步取纪元竞态（`EBRManager.cpp`）

```cpp
uint64_t current_epoch = global_epoch_.load(relaxed);  // ① 读纪元
slot->enter(current_epoch);                            // ② 登记
```
①②之间其他线程可连续推进纪元两格并完成回收，晚登记的读者以过期纪元开始读取，
错过它本应挡住的推进。

**解决**：登记-复核循环——登记后重读全局纪元，不一致则 `setEpoch` 前移到最新
（前移只发生在本线程任何读取之前，方向安全）。

### 3.2 `retire()` 用重新加载的全局纪元

退休对象挂到"更新"的列表会使宽限期不足两纪元。**解决**：改用本线程**已登记**的
纪元（只可能落后于全局值 → 只会延后回收，方向安全）。

### 3.3 `GarbageNode` 元数据从 ThreadHeap 分配

EBR 的回收账本写在被它管理的分配器里，正是"账本被复用内存覆盖 → deleter 变野指针"
的通道。**解决**：`::operator new` / `::operator delete` 配对迁系统堆。
注意：只迁**元数据本体**，payload 的 deleter 约定见 §4。

### 3.4 垃圾链表的 16 位 stamped Treiber 栈

高频 push/steal 下 stamp 回绕存在 ABA 隐患，且每纪元至多 steal 一次，收益不抵风险。
**解决**：整体替换为 `head_ + std::mutex`。

---

## 4. 缺陷 3：retire 默认 deleter 与分配方式必须配对

### 原因（一次回归与修复的往复）

`EBRManager::retire<T>` 的默认 deleter 曾被改为两种错误形态各出过一次事故：

- `delete typed_p` 遇到**裸 ThreadHeap 分配**（无类内 operator new/delete）的对象
  → 系统堆 free 收到 ThreadHeap 指针 → `free(): invalid pointer`；
- `~T() + ThreadHeap::deallocate` 遇到**系统堆分配**的对象（bug 分支 Ww 的
  `VersionNode`/`WriteRecord` 都走 `::operator new`）→ 同样崩溃。

模板 `retire<T>` 无法在编译期知道对象来自哪个堆。

### 解决：统一约定

**默认 deleter 一律 `delete typed_p`，依赖类型自身的 `operator delete` 分派**：

| 类型 | 类内 operator new/delete | delete 的实际去向 |
|------|--------------------------|-------------------|
| Occ `VersionNode` | ThreadHeap | `ThreadHeap::deallocate` ✓ |
| Ww `VersionNode` / `WriteRecord` | 系统堆 | `::operator delete` ✓ |

配套规则：**走默认 deleter 的类型若使用 ThreadHeap，必须提供类内 new/delete**
（`test_EBRManager.cpp` 的 `TrackedObject` 据此补齐）；否则显式传 deleter。
同时把被注释掉的 EBRManager 测试恢复进 `tests/CMakeLists.txt`——回归防线不应关闭。

---

## 5. TIERALLOC_CANARY 调试设施（本次新增，随 a988256 入库）

编译期加 `-DTIERALLOC_CANARY` 启用（`Slab.hpp/.cpp` + `SizeClassPool.cpp`）。
**注意设计要点**：块状态不能存在块内——空闲块首字被 freelist 链指针占用，
因此使用 side-bitmap。

| 检测项 | 机制 | 输出标记 |
|--------|------|----------|
| 双重分配 | 块状态机 0=free/1=alloc/2=remote-free | `DOUBLE-ALLOC` |
| 双重本地/远程释放 | 同上 | `BAD-LOCAL-FREE` / `BAD-REMOTE-FREE` |
| 游离指针写入（UAF-write） | 释放投毒 0xAA（跳过链指针字）+ 复用校验 | `UAF-WRITE` |
| 归还含活块 chunk | 归还前遍历 bitmap 终检 | `RETURN-LIVE-CHUNK` |
| CentralHeap chunk 二次发放 | 全局活跃 chunk 注册表 | `CHUNK-DOUBLE-HANDOUT` |

全部命中即 `abort()` 并打印 slab/block/state 信息，可直接上 gdb。
注册点在 `SizeClassPool::allocFromNew_`（池生命周期入口）而非 `Slab::CreateAt`
（测试代码直接 CreateAt 复用内存是合法用法，会误报）。
局限：只覆盖 ThreadHeap，系统堆对象需 ASan（本次正是两者配合破的案）。

---

## 6. 缺陷 4：Ww `tryWriteAndGetRecord` 回滚协议（mode=0 双重释放）

### 原因（bug 分支，ASan 双重释放签名实锤）

`WriteRecord<int>` 先后经 `TMVar.hpp` 冲突分支的 `delete my_record` 和
EBR retire deleter 释放。原回滚路径三重缺陷：

```cpp
// 原代码：稳定性检查失败后
record_ptr_.store(nullptr, release);   // ① 无条件 store
std::this_thread::yield();
continue;                              // ② 复用同一个 my_record
```

致命时序：
```
T1: CAS 安装 R_T1 成功
T1: 稳定性检查失败 —— 但还没执行回滚
      T2: T1 恰在此窗口被击伤（status→ABORTED）
      T2: 以偷 ABORTED 锁路径 CAS(R_T1→R_T2) 成功
      T2: retire(R_T1->new_node) + retire(R_T1)      ← 第一次退休
T1: 回滚 store(nullptr)          ← 还误清了 T2 刚装的锁
T1: continue 复用 my_record(=R_T1)，循环体不查自身状态
    → 要么再次安装已被退休的 R_T1（被 abort 路径二次 retire）
    → 要么撞冲突分支 delete my_record（直接 delete 已退休对象）
```

### 解决（`7efba79`）

1. **回滚改条件 CAS**：`CAS(my_record→nullptr)` 只卸仍属于自己的记录；
   CAS 失败即知被偷——所有权已转移，立即返回、绝不 delete/复用该指针；
2. **循环每轮复查 `tx->status`**：被击伤时若记录仍挂在变量上则保持安装
   （owner=ABORTED，交由偷窃协议回收），未安装则安全删除本地对象。

### 验收

mode=0 完整路径 ×100：**崩溃类 0/100**（修复前约 20% 是崩溃主力）；
断言失败 4/100 为另一先在缺陷（见 §8.1）。

---

## 7. 缺陷 5：Wound-Wait 平局裁决的堆地址彩票

### 原因（main 谱系）

`GlobalClock::now()` 只读不推进（仅 commit 的 `tick()` 推进），测试中相邻构造的两个
`TxContext` 常拿到**相同 start_ts**（失败现场日志：两条 `[WRITE-INIT]` 都是 `StartTS:2`）。
原平局裁决：

```cpp
if (my_ts == enemy_ts) i_am_older = (my_desc_ < conflict_tx);   // 比较堆地址！
```

地址顺序取决于此前分配历史：单进程新堆时先分配者恰好地址更低 → 通过；
全量套件前置测试搅动堆布局后顺序翻转 → 年轻事务反杀年老事务。
`WoundWait_OldKillsYoung` 老事务自杀（read 返回 0）、`WoundWait_YoungDies`
年轻事务提交成功——与观测完全一致。

### 解决（`8b3cdf5`）

`TxDescriptor` 增加静态原子计数器盖章的 `creation_serial`，平局时先创建者判老。
同步修正测试中"sleep 推进时间戳"的误导注释。

### 验收与一个重要限制

fix 分支（main 谱系）：全量 61/61 ×（24/25 + 复跑 15/15）。
**bug 分支上此修复已回退**：确定性的"老必胜"仲裁改变了击伤/偷窃交错分布，
暴露出新 Ww 写路径另一个潜伏洞（§8.2），对照实验 4/100 vs 0/100 崩溃实锤关联。
待写路径协议收尾后随套落地。

---

## 8. 残余已知问题（已取证，待跟进）

### 8.1 mode=0 稀有丢失更新（~2-4%，先于缺陷 4 存在）

环形事件缓冲取证：丢失更新的读集记录存在**版本-值偏斜**——
`ts=2319, val=2318`，即 `TxContext::read` 的 v_pre/v_post 三明治放行了一条
"版本号新、值旧"的记录，随后写后校验（`getDataVersion()==read_ts`）也放行。
方向：`readProxy` 的 ABORTED 分支返回 `record->old_node`（安装时刻的快照），
与 `getDataVersion` 读的 `data_ptr_`（当前值）可能描述不同版本；
三明治只保证 readProxy 调用期间 data 没动，不保证 old_node 在调用前不曾落后。
读路径一致性需要重新设计（如 ABORTED 分支改读 data_ptr_，或按节点身份验证）。

### 8.2 确定性仲裁暴露的 record 悬垂窗口

`record_ptr_` 指向已被 EBR 回收的 `WriteRecord`（表现为 `current->owner->status.load`
段错误，owner 字段为垃圾 `0x1451f8001`）。出现在 creation_serial 平局裁决启用时，
说明"老杀少"密集的交错下，偷窃/击伤协议仍存在把已退休记录留在 record_ptr_ 上的窗口。
修复缺陷 4 时已排查的路径（回滚、wounded-exit、abortRestore、steal 的 CAS 互斥）
均闭环，具体交错待下一轮取证。

### 8.3 main 旧 Ww 的 ASan 专属角落

两个 WoundWait 测试失败后紧跑 `DebugStressTest` 在 ASan 下会卡死
（普通构建 20 次全过）。旧实现已被 bug 分支重写取代，不修。

---

## 9. 验收数据汇总

### fix 分支 `fix/ebr-threadheap-premature-reclaim`（基于 main）

| 项目 | 修复前 | 修复后 |
|------|--------|--------|
| 全量套件单进程 | 100% OOM/挂死 | ×(20+24+15) 跑完，61/61（单次偶发旧Ww时序敏感） |
| 最小对 ×50 | 100% 失败 | 0 失败 |
| WwSTMCleanupTest ×20 | 100% 段错误 | 0 失败 |
| ASan 全量（排除 2 个 §8.3 遗留） | 无法跑完 | 59/59，零报告 |
| 金丝雀全量 ×5 | — | 零命中 |

### bug 分支 `bug/WwSTM-memory-issue`

| 项目 | 修复前 | 修复后 |
|------|--------|--------|
| VERIFY 模式全量 | EBR 测试 `free(): invalid pointer` | 15/15 |
| mode=0 ×100 | 崩溃 ~20% | **崩溃 0**，断言失败 4（§8.1） |

---

## 10. 提交索引

```
fix/ebr-threadheap-premature-reclaim（worktree /tmp/castm-main，基于 main d5e4147）
├─ a988256  fix: 线程退出/空板销毁不再归还 chunk，根除并发内存踩踏（含金丝雀设施）
├─ f128e84  fix: EBR 协议加固——登记复核/登记纪元退休/账本迁系统堆/垃圾链表互斥
└─ 8b3cdf5  fix: Wound-Wait 平局裁决改用创建序号，消除堆地址彩票

bug/WwSTM-memory-issue（主仓库）
├─ fbd86f1  fix: 线程退出/空板销毁不再归还 chunk（cherry-pick a988256）
├─ 2bf708e  fix: EBR 协议加固（cherry-pick f128e84）
├─ a345e04  fix: EBR retire 默认 deleter 统一为 delete 分派约定，恢复 EBR 测试
└─ 7efba79  fix: tryWriteAndGetRecord 回滚协议修复，根除 mode=0 双重释放
```

fix 分支已具备合并回 main 的条件；bug 分支的 `mode=0` 完整路径仍开放（§8）。

---

## 11. 经验沉淀

1. **"挂死"先看 RSS 和退出码**：137 是 SIGKILL（本例为 OOM），不是挂起；stdout
   缓冲会吞掉测试进度，用 `stdbuf -oL` 行缓冲再看一次结论可能反转。
2. **自研分配器是 ASan 盲区**：TierAlloc 的 slab 走 mmap，ASan 看不见内部踩踏；
   系统堆对象才是 ASan 的辖区——两类对象混用时金丝雀（分配器内）+ ASan（系统堆）
   配合使用。
3. **替换式实验是最快的责任域隔离**：把可疑子系统整体短路成标准实现
   （本例 ThreadHeap→malloc），一次实验顶十次代码审读。
4. **deleter 必须与分配方式配对**，跨堆对象的默认 deleter 应依赖类内
   operator delete 分派，而不是假设单一堆。
5. **时间不推进时间戳**：`now()` 只是读，`tick()` 才是写；任何依赖"先后构造=
   新旧"的仲裁都必须有确定性的序号来源，堆地址不是。
6. **海森 bug 的取证用旁路记录**：fprintf 插桩会改变时序（本次 80 次不复现），
   环形缓冲二进制事件 + 事后导出几乎无扰动（15 次命中）。
