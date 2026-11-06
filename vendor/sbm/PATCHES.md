# vendor/sbm 本地补丁清单

相对于上游 pinned commit `aae56c77` 的全部有意分叉。同步上游时需逐一重放。

## P1 · `src/Data/Sgy/SgyIndexCache.{h,cpp}` — 伴生 `.sgyidx` + 项目化缓存目录

来自 paleo_workstation 移植期的本地增强，上游无此能力：

- `CompanionPathFor()`：`Load()` 优先读源文件旁的 `<name>.sgyidx` 伴生文件，
  再回落集中式缓存目录——工区搬机器/共享盘场景直接带走索引。
- `LoadFromPath()` 独立接口、`Save(..., targetCachePath)` 可选落盘目标。
- 缓存目录 `SeismicF3Viewer` → `paleo_workstation`（`%LOCALAPPDATA%`/平台等价物下）。
- POSIX：`<unistd.h>`/`getpid()` 守护；路径转 UTF-8 走 `sgyio::ToUtf8Path`。
- `CacheDirectory()` POSIX 解析序（审计 01 M4，2026-10）：`SEISMIC_INDEX_CACHE_DIR`
  → `$XDG_CACHE_HOME/paleo_workstation/index-cache`（仅绝对路径）→
  `$HOME/.cache/paleo_workstation/index-cache` → `<tmp>/paleo_workstation-<uid>/index-cache`。
  原先固定 `<tmp>/paleo_workstation/index-cache`，多用户共享且不受 ctest XDG 沙箱隔离。
  `src/io/segyindexstore.cpp` 直接调用本函数预建目录（不再复刻解析序），并把
  `paleo_workstation*` 层收紧为 0700。

## P2 · `src/Data/Sgy/SgyVolume.{h,cpp}` — `TimeGridCache` + mmap 并行 `ReadSlice`

时间片读取的项目实现（原 085e88e WIP），替代上游整行顺序读：

- `TimeGridCache`/`EnsureTimeSliceGrid()`：inline×xline→trace 序号网格只算一次。
- `ReadSlice` 快路径：`QFile::map` 映射整卷 + `std::thread`（≤16 线程）按格式码
  直接解码样本（IEEE/IBM float、int16/int32/int8）；失败回落上游逐道顺序读。
- 依赖 `Qt6::Core`（QFile/QString）——vendor 库因此私有链接 Qt。

## P3 · `src/Data/Sgy/SgySequentialScan.cpp` — POSIX 同步读 buffer 分配

上游 `singleBuffer` 仅在 `queueDepth == 1` 时分配（Windows 异步路径用
`reader.Buffers()` 每槽独立 buffer）。POSIX 分支恒走 `singleBuffer` 同步读，
`ClassifyPath` 返回 Unknown 时 `readQueueDepth=2` → 空 vector `.data()` 为
nullptr → `istream::read(nullptr, bytes)` 段错误。补丁：POSIX 下无条件分配。

## P4 · `src/Engine/StorageProfile.cpp` — POSIX 回退

上游仅 Windows 实现（IOCTL_STORAGE_QUERY_PROPERTY）。POSIX 分支读
`/sys/block/*/queue/rotational` 判定介质类型；探测不到返回 Unknown，
不影响功能，只影响预读/页大小这类启发式默认值。

## P5 · `src/Data/Sgy/SgyIo.{h,cpp}` + 6 个调用点 — paleo 生产道字约定回退

`paleo_workstation` 生产工区（含 966MB 真实体与 `testdata/segy/synthetic_4x5.sgy`）的
INLINE@189/CROSSLINE@193 两字恒 0，实际编码为 field record@9（inline）+ CDP ensemble@21
（crossline）——`src/io/segyreader.cpp` 冻结的约定（见 tools/make_segy_fixture.py 头注）。
上游只读标准字，这些文件在 probe/index/read 链上全部退化为文本头回退或失败。

- `sgyio::ReadTraceKeyWords(header)`（SgyIo）：先读标准 INLINE/CROSSLINE；仅当两字
  同时为 0 才回退 @9/@21。上游规范文件行为逐位不变。
- 调用点统一换用该 helper（与 probe 同一取字口径，读回校验永不自相矛盾）：
  `SgyRuleLayout.cpp` ReadTraceKey（probe）、`SgyIndexBuilder.cpp` 全卷扫描、
  `SgySequentialScan.cpp` 记录循环（.pair 落盘同步换字）、`SgyVolume.cpp` /
  `SgyReadSession.cpp` ValidateRuleTrace、`SgyFileReader.cpp` 摘要 declared ranges。
- 未动 `Engine/RoiWorkspaceBuilder.cpp`（上游实验路径，SDK 链路不经过）。

## P9 · `src/Engine/PagedPipeline.{h,cpp}` — L0 质量账目（paleo P5 Phase 1）

- `PagedBuildResult` 新增 `missingTraceCount`/`damagedTraceCount`/
  `damagedTraceSample`（前 32 个）/`valueMin`/`valueMax`。
- L0 道循环：缺席（FindTraceIndex<0）与损坏（ReadTrace 失败）计数 + NaN
  填充 + 记样，损坏道不再让整个金字塔 IoError 失败；值域 NaN 感知统计。
  LOD 层沿 L0 指针聚合（LOD 构建本身无源道概念）。

## P6 · `src/Engine/TranscodeJob.{h,cpp}` — 并行分片编码 + 质量账目 + scanning 阶段（paleo P5 Phase 1）

- `TranscodeOptions.writerThreads`（1..4，默认 1=上游行为）：codec=zstd 时
  产者 → N 编码线程（BoundedQueue 容量 8）→ 单写线程（容量 4）。写序与
  可续跑布局不变；raw codec 自动退单线程（无编码可并行）。**关队语义**：
  产者只关 encodeQueue；最后一个编码器负责关 writeQueue（否则在途编码块
  会被已关闭的写队列丢弃——静默丢块 bug，测试 tst_seismic_transcode 锁定）。
- `TranscodeResult` 新增缺失/损坏道计数、损坏道样、值域（与 P9 同形）；
  坏道 NaN 填充不炸整体（上游 ReadTrace 失败原本就是静默 continue，此处
  只是把账记全）。
- `TranscodeProgress.phase="scanning"` 头文档承诺但从未发射——现在在
  SgyVolumeSource::Open（索引/续跑探测）前真实发射。

## P7 · `src/Engine/WorkspaceFormat.{h,cpp}` — WriteChunkPrepared + meta 头探测（paleo P5 Phase 1）

- `WorkspaceWriter::WriteChunkPrepared(cs,ci,cx,payload,bytes,err)`：
  写已编码载荷；`WriteChunk` = 编码 + 该函数（P6 编码池消费）。
- `WorkspaceMetaSummary` + `ProbeWorkspaceMeta(base,...)`：只读头与完成
  位图（不碰 chunk 表/shard），供续跑 UX 与 Auto 就绪判定廉价调用。
- `WorkspaceReader::IsComplete()`：完成位图 popcount ≥ chunkCount。

## P8 · `src/Engine/Sdk.cpp` — Auto 只认完整工作区（paleo P5 Phase 1）

`Dataset::Open(Auto)` 的伴生 `.sf3c.meta` 发现改为：`ProbeWorkspaceMeta`
判定 complete 才升级工作区后端；半成品（取消留下的可续跑 meta）保持直读
后端并置 `FellBackToDirect()`（消费侧如实看到「伴生存在但未就绪」）。
杜绝「半个有效 meta 的假完成态」被 Auto 当成品读出错。

## P10 · `src/Data/Sgy/SgyVolume.{h,cpp}` — 切片/时间网格尺寸 64 位运算与上限

inline/xline 计数由文件内容决定，`inlCount * xlCount`（int）可回绕：对角测网
65536 道时乘积为 2^32 → 0，`assign(0)` 后按真实行列写越界（ASan SEGV）。

- `GridCellCount(a, b)`：64 位求积，超过 2^28 格视为异常几何返回 0。
- `EnsureTimeSliceGrid()` 改返回 `bool`；`ExtractSlice` 的 Time/Inline/Xline 分支在
  分配前拒绝超限尺寸并给出错误，所有行列下标改为 `size_t` 运算。
- 回归：`tests/tst_seismic_engine.cpp::timeSliceGridOverflowRejected`。

## P11 · `src/Data/Sgy/SgyRegularFile.h` — 普通文件读取守卫收口

`SgyIndexCache.cpp` 与 `SgySequentialScan.cpp` 的六处非抛异常
`is_regular_file(path, error_code)` 统一经 `IsRegularFile`。POSIX 目录可打开、
读取却抛 EISDIR 的拒绝策略保持不变；原调用方的打开/检查顺序、错误原因、
CRC/截断与指纹降级逻辑保留。`SgyIndexService.cpp` 不直接读取文件，无同构块。

## P12 · `src/Data/Sgy/SgyCoordinateMapper.cpp` — 中心化最小二乘仿射拟合（#135）

上游 `Fit()` 用未中心化的原始 inline/xline 累加 3×3 法方程、Cramer 法则求解：
inline/xline ~1e3、投影坐标 ~1e5..1e6 时行列式只有 O(1e2) 而余子式乘积
O(1e20)，灾难性抵消——精确仿射的 2×4 测网拟出 rms=2 m（FMA 收缩下 6404 m，
与 CI 逐位一致），5×5 测网被 25 m 门限误拒（`tst_crossplot_samples` 恒红根因）。
补丁：先对 inline/xline/x/y 去均值，在中心化 2×2 Gram 系统上解斜率、由均值
回推截距；退化判据改为相对量 `det > 1e-9·sii·sjj`；残差在中心化坐标系计算。
公开接口/字段语义（a..f、rms/max、拒绝文案）不变。回归：`tests/tst_sgycoordmapper.cpp`。
宜回馈上游。

## 编译层适配（非源码补丁）

- `paleo_sbm` 目标加 `-fno-char8_t`（MSVC `/Zc:char8_t-`）：上游 30 处
  `.u8string()` 按 C++17 返回 `std::string` 的语义写；头文件零 char8_t
  使用，故 flag 不外泄到消费方 TU。
- `SEISMIC_HAVE_ZSTD`：系统 libzstd 存在时定义并链接。

## P13 · 方向50：非有限 SEG-Y 样点 → quiet NaN + 质量账目

- `Data/Sgy/SgySampleSanitizer.h` 仅改非有限值；IEEE/IBM 的所有有限振幅
  逐值保持原样。`SgyReadSession` 与 `SgyVolume` 的整道、单样、mmap 时间片、
  时间预览缓存都经过同一清洗，逐次日志使用 `PALEO-SEGY-SANITIZED`。
- `SourceStatistics.sanitizedSampleReads` 为解码次数口径（重复读会再次计数）；
  session、planned、legacy/mmap 路径均计入，取消/失败前已解码的样点也保留计数，
  ResetStatistics 同时复位。
- SF3C/Paged L0 新增 `sanitizedSampleCount` / `sanitizedTraceCount`；SF3C
  时间分块导致重复读时按源道去重。值域仅纳入有限样点，保持 NaN 缺失传播。
  计数是本次实际源道解码的质量账目；续跑跳过既有块时不冒充重新验过源样点。
- C++ `SourceStatistics` 布局增加字段，SDK facade `kSdkAbiVersion` 提升到 2。
  本仓 vendor 与所有消费目标统一重编；公共 C 函数参数/结果布局未更改。
- 上层 `SeismicTranscodeReport` 转码摘要与 JSON 同时展示清洗数；
  `tst_io_seismicquality` 对拍 direct/voxel/legacy/mmap、SF3C 单/多次读、Paged L0
  与服务报告，`tst_io_robustness` 钉住旧属性/反演 null 传播。

## P14 · #611：SgySectionStats 新增 columnDistancesM（米弧长口径）

- `Data/Sgy/SgySectionBuilder.h` 的 `SgySectionStats` 增补
  `columnDistancesM`（`std::vector<double>`）：剖面列间累计**米**弧长。
  与既有 `columnDistances`（线号弧长）并存，两口径互不替代。
- 填充规则：`seismictaskservice_slice.cpp` 在 inline/xline 仿射可得时按
  步长×√(dx²+dy²) 累计；仿射不可得（任意线 legacy 源）时保持**空**——
  never-synthesized 契约，消费方见空即知无米制信息，回落路径同步 clear。
- 消费面：`SectionGeometry`/canvas 道距标注（tst_seismic_index 米口径断言）。
  上游同步重放本补丁时若漏此字段，`seismictaskservice_slice.cpp` 编译失败
  即为显性信号。

## 2026-11-06：SF3P 目录/完成位图按需读取

- `Engine/PagedWorkspace.{h,cpp}` 普通 Open 和 metadata-only Open 均只读
  versioned header；普通模式校验目录总范围确实位于文件内。页目录和 bitmap
  共用按需 LRU，最多 64KiB/目录块，默认保留 4MiB；预算为 0 时只读当前条目。
  目录查找按 chunk ordinal 的 ci 连续顺序，避免大时间面反复跨块重读；随后仍按
  payload offset 合并物理读取，排序比较本身不做 I/O。
  无格式变化，v6/v7、raw/zstd、真实轴和数值布局保持兼容。
- 分配/seek 前检查布局乘加溢出、轴端点、page 尺寸/codec、payload 起止范围；
  读请求检查完成位，未完成 chunk 不读取未提交页。完整 backend 仍拒绝 partial。
  Reader 每请求最多 64M float 输出/1M planned pages，单页解码上限 64MiB；
  只规划命中的页，缓存命中/缺失原地划分，失败/取消清空数值输出。
- cacheSourceKey 改为哈希持有的 header，不再使用 move 后的局部 vector；同路径/
  大小/mtime 不变的 generation/轴变化也使解码页缓存失效。旧页校验算法/seed 不改。
- Reader Stats 追加 header、directory/bitmap I/O 及目录缓存 buffer 驻留/峰值统计。
  原 readBytes/readCalls 仍为 payload 口径；目录缓存字节不是进程 RSS，也不冒充全局
  内存账本。句柄仍按现有 Dataset 的单线程使用约定。
- `tst_sf3p_directory` 使用稀疏 1.51GiB 目录证明无全目录预载，覆盖 v6/v7 数值/
  NaN/方向、raw/zstd、坏目录、布局溢出、完成位、取消和同属性不同代次缓存隔离。
  `PALEO_SF3P_READONLY_PATH` 可选择只读一个现有文件的 1×1×1 窗，不创建或转码原件。
