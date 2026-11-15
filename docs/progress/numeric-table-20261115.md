# numeric_table.v1 SDK 前置与验收

日期：2026-11-15。基线：`9160615d`；实现 `b71a7106`，审阅修复及实际编译源码
冻结于 `2c711ce1`。后续仅补验收记录。主仓 I1 接线与端到端验收仍待实施。

## 范围

以既有 `source_tables` 为来源，提供显式数值投影声明、CAS 确认、描述及有限列窗口。
公开 DTO 使用标准 C++；原始 `source_cells`、展示 `DataTable` 和既有地质对象类型不变。
schema 只允许显式可写升级；普通读取和只读打开不隐式迁移。
契约与边界见 [numeric-table](../numeric-table.md)。

首版为 `source_row` 无量纲轴加最多 15 个 Float64/Boolean 值列、每窗最多 8192 行。
读取在同一 savepoint 核对实际 model head/投影修订，再以列优先索引读取；字段和累计
文本预算在 SQL 返回载荷前校验。失败不交付部分数值。ReadCut 是实际观测切面，不是
历史数值租约；统计分别描述 SQL 返回量、输出和保守工作预留，不冒充物理 I/O 或 RSS。

## Review 修复

审阅发现声明中的 null 标记经 JSON 转义后可能超出原估算，导致可写声明随后无法按默认
预算读取或 CAS 修正。修复按实际持久编码计费，同时保留原保守余量，不扩大 16 KiB
声明上限；内部 CAS 比较使用合法声明所需的硬元数据预算，调用者读取预算保持独立。
新增 `encodedMetadataAndCasBudgets` 覆盖首次原子拒绝、近界合法声明、低读取预算、
无操作确认及缩小声明后的 CAS 更新。

## 实际验证

Linux 独立物理 build，C++20，构建并行度 2；七个目标实际编译通过：

```text
numeric_table_tests metadata_budget_tests model_change_tests readonly_service_tests
data_page_tests snapshot_tests object_descriptor_tests
```

对应 CTest **7/7 通过，27.95 秒**：numeric_table_contract、metadata_budget_contract、
model_change_contract、readonly_service_contract、data_page_contract、snapshot_contract、
object_descriptor_contract。测试覆盖值/null/bool、精确身份、CAS/取消回滚/重开/只读、
源改动失效、并发快照、字段/累计/输出预算、超大单元格和未选列、索引计划及 schema 修复。

本仓没有独立 CI 源码门禁脚本；本批按 AGENTS 执行五项边界/格式检查，**5/5 通过**：
15 个公开头无 Qt include，8 个 parser 源/头无 SQL/store/UI include，84 个服务实现源/头
无 QtWidgets include，新增差异无机器用户路径，`git diff --check` 通过。
这不等于主仓 Source gates，也不替代主仓集成验证。Windows 原生 SDK 与应用集成结果
不从这套 Linux 验收外推；本记录不宣称新增主仓 Agent 工具已可调用。
