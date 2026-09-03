# BUG-03：EBR 默认 deleter 与对象分配堆不配对

状态：已修复

修复提交：`a345e04 fix: EBR retire 默认 deleter 统一为 delete 分派约定，恢复 EBR 测试`

## 症状

EBR 测试曾在第一个用例就报告：

```text
free(): invalid pointer
```

根因是 `retire<T>` 的默认释放方式与对象实际分配方式不一致。

## 两种错误组合

1. 对 ThreadHeap 裸分配、没有类内 `operator delete` 的对象使用系统 `delete`：
   系统堆收到 ThreadHeap 指针。
2. 对系统堆分配的 Ww `VersionNode` / `WriteRecord` 使用
   `~T() + ThreadHeap::deallocate`：ThreadHeap 收到系统堆指针。

模板本身无法从 `T*` 判断对象来自哪个堆。

## 统一约定

默认 deleter 使用：

```cpp
T* typed_p = static_cast<T*>(p);
delete typed_p;
```

依靠类型自身的 `operator delete` 分派：

| 对象 | 分配方式 | `delete` 去向 |
|---|---|---|
| Occ `VersionNode` | ThreadHeap，类内重载 new/delete | ThreadHeap |
| Ww `VersionNode` / `WriteRecord` | 系统堆 | 系统堆 |

如果对象由 ThreadHeap 裸分配，必须提供匹配的类内 new/delete；否则使用显式的
`void*` deleter。

## 回归防线

`tests/EBRManager/test_EBRManager.cpp` 的 `TrackedObject` 明确提供 ThreadHeap 配对的
`operator new/delete`；`CustomDeleterWithStandardHeap` 则显式验证系统堆对象和自定义
deleter 的组合。EBRManager 测试已恢复到 `tests/CMakeLists.txt`，不再被注释掉。

## 边界

EBR 的 `GarbageNode` 元数据本体使用系统堆分配/释放，这是账本生命周期修复；它与被
管理 payload 的 deleter 是两层不同的所有权，不能混为一谈。
