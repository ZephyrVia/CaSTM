# BUG-06：WwSTM 版本-值偏斜导致丢失更新

状态：已修复

修复提交：`ffc7f14 fix: readProxy 非提交路径统一改读 stable data_ptr_，根除版本-值偏斜丢失更新`

## 症状

`mode=0` 曾出现约 2%～4% 的稀有丢失更新。环形事件缓冲捕获到读集记录中版本和数值
不一致，例如：

```text
ts=2319, val=2318
```

读事务的前后版本检查没有发现异常，但读到的版本号和数值并不属于同一个稳定快照。

## 根因

`readProxy()` 在 ABORTED 路径返回安装时保存的 `record->old_node`，而
`getDataVersion()` 检查的是当前 `data_ptr_`。这两个指针可能描述不同版本：

```text
readProxy 返回 old_node 的值
getDataVersion 读取当前 data_ptr_ 的版本
三明治检查通过，但版本和值并不配对
```

这是读路径的一致性问题，不是 EBR 纪元回收问题。

## 修复

非提交路径统一读取稳定的 `data_ptr_`，让读到的值与随后验证的版本来自同一稳定
数据源，消除版本-值偏斜。

## 验证

修复后 `ConcurrentPathIncrement`、高竞争路径及完整 `mode=0` 测试通过；该修复与
BUG-02 的 EBR 纪元标记修复、BUG-04 的 WriteRecord 生命周期协议分别记录和验收。
