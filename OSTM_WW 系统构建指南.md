# CaSTM 构建与代码阅读指南

本文于 2026-11-04 更新为当前仓库的构建指南。历史文件名 OSTM_WW 对应现在的
WwSTM；旧版分阶段伪代码不再作为实现规范。协议见[当前设计说明](OSTM_WW.md)。

## 构建入口

在仓库根目录执行：

~~~bash
cmake -S . -B /tmp/castm-package-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-DSTM_WW_VERIFY_LOGIC_MODE=0"
cmake --build /tmp/castm-package-release --target run_tests stm_benchmark -j2
ctest --test-dir /tmp/castm-package-release --output-on-failure --timeout 60
~~~

现有 CMake project 名为 CppProjectTemplate，内部静态库目标名为 mylib；
它们是历史构建标识，项目对外名称统一为 CaSTM。

| 目标 | 用途 |
| --- | --- |
| mylib | 分配器、EBR 与 OccSTM 的非模板实现 |
| run_tests | tests/CMakeLists.txt 中启用的 GoogleTest 源文件 |
| stm_benchmark | 四后端共用的 Shared KV/Array workload |

本轮验证使用 Linux/WSL2、GCC 11.4 和 CMake 3.22，C++ 标准设为 C++17。
GoogleTest 已内置；Python runner 不依赖绘图库，绘图脚本另需 matplotlib。
其他平台没有本轮验证记录。

## 模式与配置

| 配置 | 当前含义 |
| --- | --- |
| STM_WW_VERIFY_LOGIC_MODE=0 | 启用 WwSTM 的真实 EBR 退休与回收路径 |
| STM_WW_VERIFY_LOGIC_MODE=1 | 逻辑验证模式，增加 TMVar 锁并保留部分已发布对象 |
| STM_WW_TEST_HOOKS=1 | 仅测试目标启用的确定性交错和生命周期计数 |
| STM_ALLOCATOR_MODE=native/system/tier | CMake 中的 benchmark 选项，切换关键对象分配路径 |
| STM_BENCHMARK_NATIVE=ON | benchmark 可选的 -march=native；默认关闭 |

Config.hpp 在没有显式定义时默认 VERIFY=1。上面的测试命令因此显式传入 mode=0。
benchmark 目标自行固定 VERIFY=0、TEST_HOOKS=0、NDEBUG 和 -O3；
应使用独立 Release 构建目录，避免与 VERIFY/sanitizer 编译参数混用。

默认 allocator native 下，OccSTM 的 VersionNode 使用 TierAlloc，WwSTM 的
VersionNode/WriteRecord 使用系统堆。system/tier 选项只用于 benchmark 的关键路径
对照，TxDescriptor、EBR 元数据和标准容器缓冲区不在切换范围内。
WwSTM 的显式事务 alloc 路径还受 VERIFY 配置影响，不能把 benchmark 开关解释为
替换了整个进程的分配器。

VERIFY 和 ASan+UBSan 的完整命令、跳过范围见[测试说明](docs/testing.md)。
性能矩阵和 allocator ablation 命令见[benchmark 使用说明](benchmarks/README.md)。

## 建议的阅读顺序

1. [TxContext](include/WwSTM/TxContext.hpp)：读写集、validate/prepare/status-CAS/cleanup。
2. [TMVar](include/WwSTM/TMVar.hpp)：单一 head、快照读取、Record 发布与帮助清理。
3. [TxDescriptor](include/WwSTM/TxDescriptor.hpp) 与 [WriteRecord](include/WwSTM/WriteRecord.hpp)：
   逻辑状态、准备门闩和对象关系。
4. [EBRManager](src/EBRManager/EBRManager.cpp)：线程登记、全局 epoch、退休与 grace period。
5. [SizeClassPool](src/TierAlloc/ThreadHeap/SizeClassPool.cpp)：slab 复用、跨线程释放及退出边界。
6. [测试范围](docs/testing.md)与[性能报告](docs/performance/README.md)：对应实现结论的证据。

应用接口目前分为 Occ::Transaction / STM::atomically 和 Ww::TxContext。
事务体可能重执行；使用 WwSTM 时要检查事务活跃状态和 commit() 返回值。
共享对象的销毁需要确保使用者已退出；系统没有持久化提交、网络接口或统一业务层 API。
