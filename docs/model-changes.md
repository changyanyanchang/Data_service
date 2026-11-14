# 模型元数据变更日志与写入 gate

普通可写开库建立 journal v1。只读开库不升级：`readModelChangeHead` 对旧库返回
`available=false`；未知版本拒绝日志读取和 SDK 写入。日志的 storeId 稳定，generation
标识可以连续增量消费的一段历史；sequence 是该 generation 内已提交的 batch 序号。

SDK 写事务（含自动包装的无显式事务 DML）在提交前生成 operationId/receipt。持久
触发器把受影响的来源、井与父级对象按键去重；百万层位点、表格单元或层段行不会产生
百万项日志。原始数组、时深点和振幅不进入 after-image。来源卡片包含来源字段、归井
关系和对象目录，以及地震体/层位的薄元数据；独立井卡片保存井元数据。删除保留 tombstone。
尚不能精确投影的配置/解释变化保留 `kind=store,requiresRebuild=true`，调用方须保守重建，
不能把它当成已覆盖所有语义的精确依赖日志。

batch 和 items 与业务值同事务提交。失败/回滚不推进 head；只读或无受影响对象的空事务
不生成 batch。已保存的 B1 元数据在 B2 后仍独立可读，不查询当前值或原件。它不是历史
数值版本库，不承诺旧数组恢复，也不代替对象修订/facet 消费依赖。

`readModelChanges` 按 sequence/ordinal 游标有界读取，`complete` 表示本 batch 完结；
同一 batch 可跨页，接收方应先暂存完整 batch，再原子推进自己的 checkpoint。
`sequence=0` 选择首个大于 afterSequence 的 batch。expectedGeneration 不匹配、指定
batch 缺失或单项超出页预算均明确失败。单卡片最大 8 MiB、batch 元数据最大 64 MiB；
页面预算最大 16 MiB。这里只读持久元数据，不把摘要/目录成员集合伪称数值载荷历史。

外部/旧客户端直接 SQL 写入仍被允许。触发器发现没有 SDK 事务 context 时设置 unmanaged；
日志读口返回 `RebuildRequired`。SDK 不为这些写入补造不存在的完整 after-image 历史。
下次明确的可写开库重置 generation 和增量历史，必须重新 bootstrap；只读打开不修复。
内部影子块/审计表不等于新的业务对象；刻意删除日志触发器或伪造内部 context 的客户端
不属于兼容旧 writer 的保证范围。

`tryAcquireModelWriteLease(directory,timeoutMs,error)` 使用 `directory + ".write.lock"`，
锁文件位于模型目录之外。SDK 正常可写打开、模式升级、写事务和顶层 savepoint 持有同一
gate，提交/回滚后释放。lease 只能在所属线程中嵌套，其他线程/进程等待或返回忙。
发布者可持 lease，校验 active head，关闭所有数据库句柄后完成目录 rename。
`forkModelGeneration(expected,renewed,error)` 在写 gate/事务内检查 storeId+generation+
sequence，开启新的 generation，保留 storeId 和所有业务数值。原 checkpoint 需 bootstrap。

gate 只能协调已适配 writer。绕过 gate 的旧外部 SQL 可能在目录发布窗口发生，不能据此
承诺跨旧 writer 的原子发布；可检测到的 unmanaged 写入仍要求保守重建。Windows rename
前关闭句柄是发布者的责任，持有 gate 不会强制关闭其他读者。

可写连接显式采用 `synchronous=FULL`。外层写 gate 后按物理文件 ID 和 active 库的
storeId/generation 校验已打开连接，失败返回 `StaleModelGeneration`，要求重新打开；
不会把旧连接的后续修改写进 retained 备份。检查只探测文件身份和两个标量，不运行
完整开库/升级。当前实例自己成功提交代次切换后刷新缓存，其他空闲连接不能自行认领
新代次。普通重新打开、INSERT OR IGNORE 和同值元数据替换不应伪造代次或提交。
