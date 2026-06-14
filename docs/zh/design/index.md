# Index

clice 维护一个持久化的符号索引，用于跨翻译单元的查询——查找引用、调用层次、工作区符号搜索等。索引在后台构建，使用 FlatBuffers 序列化存储到磁盘。

## 架构

### 数据层

- **TUIndex**（`src/index/tu_index.h`）— 单翻译单元的符号数据，由 `SemanticVisitor` 在编译时产生。包含符号哈希、出现位置和关系（定义、引用、基类、覆盖、调用者/被调用者）。这是一个临时输出，会被合并到下面的持久化存储中。

- **ProjectIndex**（`src/index/project_index.h`）— 全局跨 TU 符号索引。将符号哈希映射到其定义位置、名称、种类以及整个项目中聚合的关系。

- **MergedIndex**（`src/index/merged_index.h`）— 按文件分片，合并头文件上下文。单个头文件可能通过多个宿主源文件被索引；合并索引将这些协调为统一视图。

### Indexer（`src/server/compiler/indexer.h`）

`Indexer` 类是查询和调度层。它本身不持有索引数据——持久化数据在 `Workspace` 中（ProjectIndex + MergedIndex 分片），未保存缓冲区的每文件数据在 `Session` 中（OpenFileIndex）。

职责：
- 跨文件导航查询（定义、引用、层次）
- 符号搜索（`workspace/symbol`）
- 带空闲超时和去重的后台索引调度
- 将 TUIndex 结果合并到持久化存储
- 索引分片的磁盘保存/加载

## 后台索引

文件编译后，其 TUIndex 会被合并到项目级索引中。后台索引在空闲期运行（可通过 `idle_timeout_ms` 配置，默认 3 秒）：

1. 文件在打开、保存或其依赖发生变化时入队。
2. 空闲定时器去重快速变化——只有超时后才开始索引。
3. 任务以可配置的并发度分发到无状态工作进程。
4. 用户发起的请求（hover、补全）期间通过 `ScopedPause` 暂停索引。
5. 通过 LSP `$/progress` 通知向客户端报告进度。

## 查询

索引器支持以下跨 TU 查询：

| 查询 | 方法 |
|------|------|
| 跳转到定义 | `query_relations(path, pos, Definition)` |
| 查找引用 | `query_relations(path, pos, Reference)` |
| 调用层次（传入） | `find_incoming_calls(hash)` |
| 调用层次（传出） | `find_outgoing_calls(hash)` |
| 类型层次 | `resolve_hierarchy_item()` |
| 工作区符号 | 搜索 ProjectIndex |

对于有未保存更改的已打开文件，查询会先检查 Session 的 OpenFileIndex，然后回退到持久化的 MergedIndex。

## 序列化

索引数据使用 [FlatBuffers](https://flatbuffers.dev/)（`src/index/schema.fbs`）序列化：
- 零拷贝反序列化——索引分片可以直接从磁盘内存映射
- 紧凑的二进制格式——符号数据比 JSON/protobuf 更小
- 高效的部分读取——只加载查询所需的分片

## 符号标识

符号通过 64 位哈希（`SymbolHash`）标识，派生自 Clang 的 USR（Unified Symbol Resolution）字符串。USR 生成（`src/index/usr_generation.cpp`）为每个符号产生一个跨 TU 稳定的规范标识符。
