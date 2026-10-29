# Embedding in a workstation

The library remains a standalone survey-data service. It can also be built using
`add_subdirectory`; `PDS_BUILD_DEMO`, `PDS_BUILD_TOOLS`, `PDS_BUILD_EXAMPLES`,
`PDS_BUILD_TESTS` and `PDS_INSTALL` default to the top-level-project setting.
They can be selected explicitly. With tests and demo disabled Qt Widgets is not
a required component of the library.

A host using the same headless seismic engine can pass
`PDS_SEISMIC_ENGINE_TARGET` and `PDS_SEISMIC_INCLUDE_DIR`. Both must refer to a
compatible target/header set. The service then does not build or link its own
copy of that engine. SDK installation requires the standalone engine instead.
The copied ZIP code is isolated in `paleo::data::zip`; host ZIP symbols cannot
silently override its implementation.

`paleo_data/catalog.h` supplies standard C++ entity, asset, immutable-version and
explicit-link records. `CatalogStorage` borrows a named, already-open QSQLITE
connection on the calling thread. The host owns connection lifetime, transactions,
schema-version gates, backups and recovery. Schema DDL is explicit, not run by
the constructor. Storage never commits, reopens or changes the access mode of
the host connection. JSON extension columns are preserved verbatim. File/path
containment and domain validation remain the host's responsibility.

These records complement the native `DataService` survey objects. They do not
automatically mirror or reconcile the two models. In particular native curve
updates are in-place; applications needing immutable versions must first define
an explicit version/source mapping and a coordinated commit protocol.

`DataService::openProjectReadOnly` reads an existing project without schema
upgrades. Import and reimport fail before staging or copying sources; edits fail
on the SQLite read-only connection. Use a new instance on every thread. SQLite
WAL readers may still use sidecar files; this is not an immutable snapshot mode.

Tests generate temporary inputs and cover reopen, rollback, access mode, UTF-8,
unknown catalog metadata and rejected mutations. No workstation dependency or
private project is needed.

## Publishing a prepared immutable version

`DataService::importPreparedFile` accepts a `ParsedFile` already normalized by a
trusted host format adapter. Pass the currently readable file, its original
identity, the source reference that will be valid after publication, and the
expected full SHA-256. The reference may be relative to the data project for a
relocatable managed source. This method neither copies nor rewrites the source.

The service checks the source digest before storing and again before commit.
Objects and the source record are committed together; shape errors, cancellation,
content drift and unresolved blocking issues roll back the entire batch, including
changes to shared wells. On failure, the method returns false, leaves sourceId at
zero, and reports the cause in error. Nonblocking issues remain stored and set the
source status to review. Successful imports return a database-local source ID. The host
must publish the resulting database with its own version journal and retain the
explicit source/object mapping. The API does not make a second writable catalog
or silently coordinate transactions across independently writable databases.

Prepared curve values must already contain NaN for source-declared missing cells.
Finite negative values, including -999 and -99999, are retained. Ordinary file
imports keep their existing parsing and null-value rules; `ParseOptions` has no
host-only switch. Workstation-specific columns may use the public `SourceTable`
store alongside the typed objects to preserve fields not yet in the shared model.

Close the writer before hashing/publishing its SQLite file, then use read-only
sessions on consuming threads. A host using immutable versions should create a
new prepared model for edits and reimports, rather than mutate a published model
or ask the ordinary parser to reconstruct its host-specific interpretation.

Prepared SEG-Y sources retain full SHA-256 identities. `relocateSource` and
`verifySource` use the stored identity scheme; ordinary SEG-Y imports retain their
existing quick fingerprints. Relocating a full-SHA source therefore reads the
whole replacement file, without copying it or loading it all into memory.


### 工区级模型接入

宿主新建工程调用 createProject，随后直接调用 importDirectory 读取资料总目录。
ImportOptions.excludedDirectories 在遍历前剪掉宿主输出与暂存目录；不以事后过滤
替代，避免大目录扫描与生成文件回流。snapshotDatabase(destination) 使用 SQLite
VACUUM INTO 创建包含已提交 WAL 的一致数据库副本；目标必须不存在。此 API 只复制
数据库，外部来源引用保持原义，宿主使用 copySources=false 的绝对来源路径。
宿主在暂存副本上增量导入，保留未变来源的人工修改，取消后丢弃整个副本。


宿主可设置 ParseOptions.comprehensiveWellMetreDepth，声明已确认的综合井导出约定。
只有“测井曲线／离散曲线”工作表且首四列严格为“井号、深度、TVD、TVDSS”时
使用 MD／米；其他表格的深度类型及单位继续保持未知。默认关闭此宿主约定，
不改变独立服务对普通 SpreadsheetML 的保守解析；显式 depthUnit 优先。

此选项参与导入去重：从默认关闭改为开启时，相同文件按新约定生成新批次，
已有批次及其人工修改保留；继续使用相同配置则复用已有结果。修正旧批次应显式
调用 reimportSource。默认关闭与旧版缓存签名兼容，不因升级重复发布原有数据。

井位页中唯一、明确的井名可以用于同一工作簿内无井名的曲线页；多井或冲突
声明不会自动归井。井底、轨迹和位移坐标列不会作为井口坐标导入，原始表格
仍保留供核对。已缓存的旧解析结果需明确重新导入，避免覆盖已有人工修改。

## 文件关联与图谱接口

`paleo_data/graph.h` 定义渲染无关的 `FileGraphNode`、`FileGraphEdge` 和
`SourceRelation`。宿主调用 `DataService::buildFileGraph` 按需取得同一 SQLite
读取快照中的图，不需要重新扫描或解析文件，也不需要维护第二份图数据库。
节点 ID 在工区内稳定，文件使用 `file:<sourceId>`；节点保留来源路径、存储路径、
SHA-256、状态和原始类别，宿主可据此选择交互和渲染方式。

默认图包含已登记文件及显式业务对象。`contains` 连接文件与其对象，
`belongs_to` 连接井对象与井，`references` 连接层位与已确认的所属地震体。
不会根据文件名、目录或坐标相似度猜测关系。将 `FileGraphOptions.includeObjects`
设为 false 可只取文件图；非活动来源默认排除，节点和边预算可防止超大结果。
接口支持只读会话和取消；失败时结果保持为空。

`relateSources` 保存用户确认的有向文件关系，重复保存同一来源、目标和关系类型
只更新说明并保留稳定关系 ID；`listSourceRelations` 和 `removeSourceRelation`
用于管理这些记录。图边通过 `basis` 区分 `stored` 业务投影与 `manual` 人工关系。
数据服务不分配布局坐标、不渲染图谱；可视化和交互由宿主负责。

旧工区在可写打开时增量增加 `source_relations` 表，保留原 schema_version 和已有
ID。只读打开不升级；旧格式仍可构建业务关系图，人工关系视为空。

## 工程坐标系统的自动采集与查询

目录或单文件导入会从明确声明中收集坐标系统：Petrel 井口表头、显式
`ParseOptions.coordinateSystem/coordinateUnit`、预处理模型中的井位/地震体/层位，
以及已核实配准的图件。空间伴随 `.crsmeta.xml` 的显式 `AuthorityCode` 也会登记为
来源级候选。没有井口 X/Y 的井、未核实配准的图件以及单纯的坐标数值不会生成候选。
不会根据经纬度范围、投影坐标数量级、文件名或目录猜测 EPSG。

候选与对象在同一文件事务中发布，并保存在 `source_coordinate_systems`。
当全部有效候选规范化为同一个明确 EPSG，且非空坐标单位不冲突时，服务自动设置
`project_crs/project_crs_unit`，并把 `project_crs_basis` 设为 `import`。出现不同
CRS 或不同非空单位时，自动选择会被撤销；仅有非 EPSG 名称时保留候选但不猜测代码。
`setProjectCrs` 将选择依据设为 `manual`，后续导入绝不覆盖人工选择。

主系统调用 `readProjectCrsInfo` 获取当前 `coordinateSystem/coordinateUnit`、
`selectionBasis`、`hasConflict` 和全部 `ProjectCrsCandidate`。候选带来源 ID、
来源路径、对象类别和对象 ID，便于展示冲突依据；`projectCrs/projectCrsUnit`
和 `readProjectCoordinates` 的轻量契约保持不变。本接口不执行坐标转换。

旧工区可写打开时只增加候选表，不根据旧对象反推或补造来源声明；新导入或明确
重导入会采集候选。只读打开不迁移，旧格式仍可读取已保存的工程 CRS，候选返回空。
