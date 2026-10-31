# 四子层段地层框架（stratigraphic_framework）

界面（Boundary）≠ 单元。相邻界面 → 父单元（Interval）→ 固定 4 个子层段 S1–S4（SubIntervalId）。
头文件 `include/paleo_data/stratigraphic_framework.h`；纯算法 `src/flow/stratigraphic_framework.cpp`；
存储与 API `src/flow/stratigraphic_framework_service.cpp`（懒建表 `strat_schemes` / `strat_scheme_revisions` / `strat_result_bindings`，
`ds_meta.stratigraphic_framework_schema=1`，非破坏性，旧工程可读，只读工程不写）。

- 修订不可变（JSON 快照 + sha256），提交需 expected head，回退生成新修订。
- 提交同事务把几何发生变化的子层段成果与父单元综合标为 `stale`。
- `invalidateSubIntervalResult(id, error)` 将输入变更后的成果单向标为 `stale`，保留原成果 ID、来源、修订及历史内容；重复失效幂等，只读工程拒绝写入。宿主可按不可变版本 ID 重建登记并同步输入失效，不用再次执行地质算法。
- 历史单界面标签只能 `legacy_unresolved`；E12/E3/E4 算法分段不变，`Interval.legacySegment` 仅为引用。
- 地震掩膜按每道顶/底比例生成（半开区间，S4 含底），缺失/倒置道值为 2。

测试：`tests/stratigraphic_framework_tests.cpp`。
