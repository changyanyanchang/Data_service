# 地层对比：算法定义、接口与维护约定

本文对应 `paleo_stratigraphy_core` 和 `paleo_stratigraphy_service`。它们处理数据服务 SDK 已提供的公开对象，不读取 LAS、XML、DAT 等文件，不依赖 Python、模型权重或 GPU。QtWidgets 只在演示界面中使用。

这里记录的是当前实现的数学约定、工程检查和适用范围。合成测试验证公式、数据保真及事务行为，不能据此宣称真实地质解释准确率或替代专家验收。

## 1. 分层及数据契约

| 层 | 入口 | 责任 |
|---|---|---|
| 数据服务 | `include/paleo_data/interpretation.h` 及 SDK | 获取原始曲线、人工解释道、时深、轨迹、层位；保存快照、版本和来源 |
| 纯计算 | `include/paleo_stratigraphy/core.h` | 信号计算、界面求交、标准井模板、旋回划分、拖拽校验、聚类 |
| 工作流 | `include/paleo_stratigraphy/workflow.h` | 按井及段编排、取消、错误隔离、记录参数；返回新文档 |
| 界面 | `demo/stratigraphy` | 参数确认、标准井选择、显示、拖拽、撤销、调用保存接口 |

共同约定：

- `InterpretationSeries.axis` 与 `values` 等长，各曲线保存自己的轴；不得用 GR 行号直接替代派生曲线行号。
- 计算曲线使用明确的 MD、米。输入的未知单位或未知轴必须确认；TVD/TVDSS 不能仅改名为 MD。工作流可把明确的英尺输入换算成米，但保留原快照。
- 曲线 `quality` 位含义：1 缺失、2 插值、4 边缘或不稳定。1/4 不作为有效计算样本，2 可使用。此标记不同于 SDK 的 `sourceQuality`：后者 4 表示人工修改，不能被当作无效边缘样本。
- 非有限值表示缺失。空值哨兵由 SDK 依据源文件定义处理；负值不天然代表空值。
- 支持已知等价单位别名，如 M/m/metre、MS/ms/millisecond；不将 m 与 ft、ms 与 s 的冲突当成同义词。
- 参数确认只作用于计算副本。`WellInterpretation.input` 和 `InterpretationDocument.snapshot` 保留源值、源元信息和来源记录。
- `Boundary.md` 是当前解释深度；`originalMd` 保留该对象首次来源深度。人工移动更新前者，证据保留后者。
- E12 = T70–T72，E3 = T72–T73，E4 = T73–T80。缺少段边界时不能用均匀切分补造。

## 2. Burg PACE / INPEFA 数值约定

入口：`burgPace`，工程封装：`deriveCurves`。源文件：`src/stratigraphy/core/signals.cpp`。

连续有效序列先减去自身均值，得到 `x[n]`。Burg 递推使用前后向误差，反射系数为：

```text
k = -2 Σ f[n] b[n-1] / Σ (f[n]² + b[n-1]²)
```

反射系数限制在 `[-0.999999, 0.999999]`。AR 多项式首项为 1，其余系数按 Burg 对称递推更新。最终预测误差及累计量：

```text
e[n] = Σ(j=0..p) a[j] x[n-j]
PACE[n] = Σ(i=p..n) e[i]
```

- forward：沿样本递增方向累计。
- reverse：反转输入，执行同一 forward 算法，再反转输出。
- bidirectional：上述 forward 与 reverse 的逐样本平均。
- 阶数 `p` 的单位是**样本数**，并非米、地层级别或旋回厚度。实际阶数为 `min(请求阶数, 连续样本数-2)`。
- 原始低层函数初始化部分返回 0，是数值约定；`deriveCurves` 将对应初始化样本标记为边缘/不稳定，数值保留为缺失，禁止把这些 0 当成地质信号。
- 未归一化累计值的单位记录为 `cumulative(输入曲线单位)`。这里是离散累计，没有乘深度步长，不是以米积分的面积。
- 常数输入或极短信号不能产生可信旋回边界。算法不以均匀分段兜底。

这里的 PACE 指保留 Web 实现的 Burg 累计预测误差约定；不声称与所有第三方软件中同名算法逐点一致。

## 3. GR 残差累计与真正的 Fischer CDMT

两者不能混称，也不能使用同一横轴解释。

**GR 残差累计** `FISCHER_GR_RESIDUAL`：在每个连续计算区间独立计算均值，按深度反向累计：

```text
R[i] = Σ(j=i..N-1) (GR[j] - mean(GR))
```

它对应旧代码 `-cumsum((mean(GR)-GR)[::-1])[::-1]`。输入是 GR，横轴是 MD，未归一化单位是 `cumulative(API)` 等。它是曲线形态指标，不是由地质旋回厚度构建的 Fischer 图。

**Fischer CDMT** `FISCHER_CDMT`：入口 `fischerCdmt(well, segment, level)`，输入必须已有有序、连续、不重叠的同级旋回。各旋回厚度为 `h[i]=bottomMD-topMD`：

```text
meanH = Σ h[i] / N
CDMT[0] = 0
CDMT[k] = Σ(i=1..k) (h[i] - meanH)
```

输出 `axisKind=CYCLE_INDEX`、轴单位 `1`、数值单位 `m`。末端累计偏差应接近 0。L3 和 L4 可各自计算。源人工旋回存在未覆盖间隙时，函数拒绝假装连续；需要先完成实际解释或使用能形成连续层序的已确认模板结果。

## 4. Hilbert 余弦相位

入口 `cosinePhase`，输入是某阶 PACE 的有效非初始化段，先减均值。采用离散傅里叶变换生成解析信号：正频率乘 2，负频率置 0，直流及偶数长度 Nyquist 分量保留；逆变换后：

```text
z = x + i H(x)
cosPhase = real(z) / abs(z)
```

`invertPhase=true` 返回负余弦，false 返回正余弦。输出范围 `[-1,1]`，单位 `1`；它不是相位角、频率或概率。振幅接近零时输出缺失。不同阶数余弦逐点取有效值中位数，形成 `INCOSIN`。

FFT 使用标准 C++ 复数、radix-2 与 Bluestein，保留任意原始长度；不会把非 2 的幂长度曲线截短。有限记录的 Hilbert 结果存在边界和周期延拓影响，增加 padding 可减轻但不能保证消除。当前质量标记明确屏蔽 AR 初始化，不等同于经过统计标定的全体 Hilbert 可信区间。

旧 Web 通常显示负余弦；部分历史导出工具使用正余弦。迁移时必须读取 profile 的符号，不能仅因为曲线名相同就认为正负一致。

## 5. 重采样、缺口和默认参数

| 参数 | 默认值 | 语义 |
|---|---:|---|
| arOrders | 24, 60, 120 | AR 样本阶数；每个连续段实际阶数另记 |
| sampleStep | 1 m | 统一 MD 网格的间距 |
| resample | true | false 使用原采样行，不生成统一网格 |
| maxGap | 5 m | 相邻有效点允许线性插值的最大间距 |
| padding | 120 m | 显示段两侧计算上下文，受实际观测范围限制 |
| direction | reverse | PACE 累计方向 |
| invertPhase | true | 输出负余弦 |
| normalize | false | true 对各连续计算段累计曲线做 min-max 归一化 |
| minimumThickness | 5 m | 目标旋回硬性最小厚度 |
| usePace / useResidual / usePhase | true | 事件评分是否使用对应派生信息 |
| paceWeight | 1.0 | 多阶 PACE 总权重分配给各阶 |
| residualWeight | 0.35 | GR 残差累计事件权重 |
| phaseWeight | 0.5 | 多阶余弦中位数事件权重 |
| referenceWeight | 0.7 | 标准井相对界面位置约束权重 |

统一网格只在相邻有效观测之间插值；不会穿越显式缺失点，也不会超过 `maxGap` 或外推到源曲线范围外。原采样模式按缺失、超限间隔及偏离中位步长的间隔切开（0.5–1.5 倍）。连续段少于 8 个样本不进行派生计算。

padding 的单位始终是米，而非旧工具中的“额外样本行数”。因此原采样模式和 1 m 重采样模式可能不同。归一化在含 padding 的连续计算段进行，不是在显示范围二次缩放；参数中记录归一化范围，可由原始快照重新计算，未单独保存每个段的 min/max 数值。

每条输出保存 `parametersJson`、`inputFingerprint`、`runId`、`segmentId` 和状态。参数包含计算/显示范围、实际连续区间、实际阶数、样本模式、符号和归一化定义。

## 6. 事件检测与模板约束划分

入口：`detectCycleEvents`、`extractTemplate`、`divideSegment`。源文件：`src/stratigraphy/core/interpretation.cpp`。

事件候选由实际 GR 和选中的派生曲线给出。在 3/8/15 m 等尺度邻域（受实际步长下限影响），计算左右中位数差和局部曲率项：

```text
E = |right-left| + 0.5 |left+right-2 center|
```

每个信号以区间内事件能量第 90 百分位缩放，截到 3，按权重合成；局部极大值为候选，最多保留 2048 个强候选。该评分不是准确率或统计概率。GR、PACE、残差和相位均来自同一条曲线，不能表述为四份独立地质证据。

标准井模板来自人工/导入解释或明确确认的旋回，包括：

- 每个段的真实旋回数及相对边界位置；
- 每个 L4 的稳定序位身份及正反方向；
- L4 对应的 L3 组、L3 明确方向；
- 标准井编号、来源版本和协调记录。

没有预设 E12/E3/E4 必须为 6/4/4 个旋回。人工符号道使用独立 `manualTrack` 与 `manualLevel3Track`；地层单位道使用 `formationTrack`。级别可位于“三级/四级”道名，方向位于“正旋回/反旋回”记录名，不能只查找 properties 字段。

目标段采用有序动态规划，最大化事件得分，惩罚偏离模板相对位置：

```text
score(next) = score(previous) + eventScore
              - referenceWeight × ((relativePosition-templatePosition)/0.15)²
```

约束包括正厚度、最小厚度、固定序位数、不跨区域段以及经过每个锁定人工界面。默认候选来自观测事件、区域边界及已锁定点。启用 allowTemplateGuidedCandidates 时，还加入人工模板相对位置先验；先验不是观测特征，选中时明确记录 template_position_prior/v1、observedFeature=false 和待校核提示。数量与父子组合完全由对应段模板约束，不使用任意等厚数目替代模板。源曲线须覆盖目标段，且按检查网格至少有 80% 可观测 GR。模板传播不能把标准井自己当成目标。

算法返回 `SegmentationResult.usedSeries`，它就是产生本次事件的计算曲线。调用方应保存/显示这一组，不应使用另一套 padding、阶数、采样或符号重新计算后替代。

### 人工端点协调

原人工 L3/L4 与区域段端点可能略有偏差。`extractTemplate` 的 `sourceReconciliationToleranceM` 默认 0，严格要求可对应。用户明确选定正容差后，可以将 L3 区间映射到 L4 序位，并把细小的原始间隙用于模板位置协调，但不会移动原人工界面。

`sourceReconciliationJson` 记录原端点、映射序位、采用容差和 `sourceDepthsModified=false`。映射存在歧义或超出容差时拒绝。由此得到的模板是目标井的解释约束，不能说原始人工解释已经被自动修正。

## 7. 地震层位、时深与井轨迹求交

入口 `buildTrajectory`、`intersectHorizons`；源文件 `src/stratigraphy/core/geometry.cpp`。此模块处理层位面，不解析地震体振幅。

轨迹来源依次为已有明确 MD/XY positions、带 XYZ-MD 的时深测点、测斜最小曲率轨迹、明确确认的直井。优先使用实际测点位置，不把井口坐标误当全部井段坐标。

测斜最小曲率需要已确认网格北方位、以米计的井口位置及 MD=0 起点；不猜测磁偏角、不补造未测浅部轨迹。调用方提供的 `positions` 必须已在公共规范下使用 MD 米及明确的 XY 坐标系。

时间转换先按声明单位换成 ms，再处理 TWT/明确允许的 OWT×2，最后应用显式 sign 和 offset。负时间没有自动翻转规则。井时间基准、层位时间基准、MD 基准、XY CRS/单位均须一致或对未知值明确确认；已声明冲突不能被“确认”覆盖。本模块不做 CRS 投影变换、基准面转换或自动时移估计。

时深表使用 MD 时直接构造 TWT(MD)；使用 TVD/TVDSS 时，需要单调、可逆的相应轨迹轴，并保留轨迹折点，使组合 `TWT(TVD(MD))` 不被错误简化成稀疏 MD 点之间的一条直线。TVDSS 还需要明确向上/向下约定。

层位以有 inline/crossline 索引的相邻有效节点构成三角片。缺失节点对应的孔洞不跨越插值；`maximumSurfaceEdge` 可进一步限制异常长边。对于每段线性井轨迹和时深关系，先以重心坐标求其穿过三角片的区间，再精确求线性时间差的零点，因此不会因为固定扫描步长而漏掉同一步内的两次穿越。

- 多个交点全部返回，要求人工选择；不取“第一个”作为事实。
- 井轨迹与层位一段重合时没有唯一解，不自动挑一点。
- 层位空间支持之外不外推。核心默认也不延伸时深；业务层可启用时深和XY两端趋势外推，以有效GR范围及maximumLogExtrapolationM限定搜索。本工作台采用100m余量，时深独立距离上限0表示不另加距离限制。末窗采用端点锚定稳健斜率；两点仅形成低支持候选。timeDepthExtrapolation分别记录时间/轨迹延伸、曲线端外距离、拟合窗、原覆盖和未修改源数据声明。
- 精确求解只针对当前分片线性表示，不代表真实地质面或时深关系没有误差。
- 没有断层块标签时，索引邻接并不保证地质连续；完整缺行可能与稀疏规则网格不可区分，必须由数据整理/人工核对解决。
- `searchStepMd`、`intersectionToleranceMd` 是早期扫描求解器保留字段；当前 v2 精确求解不依赖它们跳过样本或合并真正不同的交点。不要把它们显示为影响精度的主要参数。

返回界面保留来源 resourceId、源指纹、原始深度、XY、TWT、符号/偏移和明确确认记录。工作流保留已锁定或人工确认的区域边界，另外存放候选；多解候选使用 `candidate:T70` 等角色，避免与正式 T70 混淆。

## 8. GR 形态聚类与标准井选择

入口 `clusterWells`，源文件 `src/stratigraphy/core/workflow.cpp`。

聚类只协助挑选参考井，不自动将簇中心认定为人工标准井。过程为：

1. 以明确的 T70/T80 为归一化深度范围。
2. 验证连续有效覆盖不低于 85%（相邻有效点间隔不超过 5 m）。
3. 分为 64 个相对深度箱，优先取观测中位数，允许符合 5 m 约束的相邻插值；有效箱至少 85%。
4. 所有合格井至少有 48 个共同观测箱，不对剩余缺口填平均值。
5. 在共同箱上逐井 z-score，使用均方欧氏距离，确定性最远点初始化的 k-means，最多 100 次。

组数 1–8，不能多于合格井或不同形态数。常数 GR 无法支撑形态聚类。结果保存源指纹、边界深度、特征、方法、参数、排除理由及是否存在人工旋回。形态相似不等于地层必然等时；标准井及每段模板仍由解释人员确认。

## 9. 共享界面、拖拽与失效

相邻同级旋回恰好接壤时共用一个 BoundaryId。父子层级相同端点也共用同一对象；原人工解释有实际间隙时保留间隙，不强行补齐。稳定序位身份与当前深度分开保存。

`moveBoundary` 在副本上修改并先校验：区域次序 T70<T72<T73<T80、正厚度、不重叠、父子包含及共享端点。失败时原对象不变。成功后保留 `originalMd`，将点标为人工锁定。

- 移动内部旋回界面：相关 CDMT 标为 stale。
- 移动区域界面：相邻段派生曲线及 CDMT 标为 stale，相关旋回需复核。
- 旧曲线数组与原 parametersJson 保留，不删除，以支持历史和撤销；未受影响段保持原状态。
- 原始 GR 不更改。重新传播算法时，锁定点仍必须被有序路径经过。

## 10. 公共工作流 API

三个方法均返回新的 `InterpretationDocument`，无数据库/文件副作用：

```cpp
auto anchored = calculateHorizons(document, geometry, cancelCheck, progress);
auto derived = deriveForSegments(anchored, geometry, cancelCheck, progress, "E12");
std::vector<TemplateAssignment> assignments{
    {targetWellId, "E12", approvedE12Template},
    {targetWellId, "E3", approvedE3Template}
};
auto result = propagateTemplates(derived, assignments, geometry,
                                 cancelCheck, progress, ""); // 空筛选表示全部指定段
```

旧调用方式 `map<wellId, InterpretationTemplate>` 仍有重载，转发为模板中各段。新调用应使用井×段分配，以便不同段使用不同人工标准。

- `segmentFilter` 空/E12/E3/E4。只替换选定段，其他段的数值、参数、runId 及 current/stale 状态不改。
- progress 在调用线程执行，粒度为井或井×段。取消在任务之间检查；单次 Burg/DP/三角面计算不能被中途抢占。调用者可把整个工作流放到工作线程，界面用信号更新进度。
- 取消抛 `WorkflowCancelled`，不返回已修改到一半的文档；回调异常也直接传播。
- 单井/段计算失败写入 warnings 和运行诊断，保留该项旧结果；整次 run 标 complete/partial/failed。调用者必须展示诊断，不能把 partial 隐藏成全部成功。
- 自动传播不会覆盖人工/导入/已确认旋回。若确需建立替代方案，应先在新解释版本明确归档旧方案，不应删除原始人工输入。
- 新旋回接受后保存 `usedSeries`，并根据真正的 L3/L4 厚度生成 CDMT。普通段重算同样刷新已有旋回的 CDMT，因此人工拖拽后不必重新传播模板；存在间隙时保留原 CDMT 为 stale，运行记录标 partial，但不丢弃已完成的 GR 派生曲线。
- 工作流产出曲线的 `runId` 直接对应文档 `runs`；原纯计算的标识保存在 `parametersJson.algorithmRunId`，可同时追溯编排和数值计算。
- run 记录快照、完整 profile、geometry 确认、段筛选及每项模板版本。模板相同 ID/version 不允许有不同内容；不同版本应使用明确可区分的版本 ID。
- 只有获得完整返回值后，才通过 SDK 的 expectedHeadRevision/idempotencyKey 提交。这将“计算取消无半成品”和“数据库并发版本冲突检查”分开处理。

## 11. 测试来源、可移植性和验收边界

核心测试 `tests/stratigraphy_core_tests.cpp`：

- AR1 解析算例及 AR3 legacy golden；
- 奇数/非 2 幂/2 幂长度 Hilbert 正负余弦；
- 缺口、初始化边缘、插值质量、采样模式和归一化；
- 实际标准井相对厚度、L3/L4 方向、共享点、人工锁定、无信号拒绝；
- 人工 L3/L4 与区域端点不一致时原深度保真及显式容差映射；
- CDMT 公式与轴单位；
- 求交的负时间、单位冲突、轨迹优先级、孔洞、多解、共面及 TVD 组合折点；
- 确定性形态聚类、相同输入顺序无关；
- 拖拽失败回滚、旧曲线保留和分段 stale。

AR3 golden 由保留的 Haiyou Web `algorithms.py` 中 `_burg_ar` / `calculate_global_inpefa` 的 NumPy 实现对合成数组 `[12,19,11,8,17,24,15,9,18,21,14,7]` 生成，分别记录 forward/reverse/bidirectional。测试容差 `1e-9`；该步骤只在开发时获取数字，测试及产品运行时无 Python 依赖。Hilbert 使用已知周期正弦解析期望，不拟合地质数据。

工作流测试 `tests/stratigraphy_workflow_tests.cpp` 验证按井×段不同模板、过滤重算、来源保真、已知单位别名、真正冲突、取消和回调异常、失败项保留、人工边界/旋回保护以及运行记录。

核心和工作流源文件使用 C++20 标准库，不调用 Win32、COM、进程、磁盘或网络 API。当前 Windows/MSVC 构建验证不等于已经完成 Linux/macOS 端到端验收；其他平台仍需在相应编译器和 Qt/SDK 配置中运行测试。

当前没有自动地层精度标定、可学习置信度、断层跨块解释、通用 CRS 转换、自动速度模型、地震体振幅推理或三维地震体显示。这些是明确的后续范围，不应以本轮函数和合成测试完成替代真实项目地质验收。
