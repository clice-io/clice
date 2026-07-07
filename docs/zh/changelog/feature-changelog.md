# Feature Changelog

本文件记录 clice 各版本面向用户的功能变化。每个版本章节记录新功能、破坏性变更与废弃项。

<!-- 在下方按时间倒序添加版本条目。 -->
<!-- 格式：
## vX.Y.Z (YYYY-MM-DD)

### Added
- ...

### Changed
- ...

### Fixed
- ...

### Removed
- ...
-->

## v1.0.0（即将发布）

v1.0 会先以滚动的测试版发布（`v1.0.0-beta.1`、`beta.2`……）公开试跑一段时间；修复以小版本持续发布、不设代码冻结窗口，试跑稳定后打正式的 `v1.0.0`。本条目汇总自 `v0.1.0-alpha` 系列以来所有用户可见的变化。

### Added

- **多进程架构。** 编译在主进程协调下的工作子进程中执行，单个翻译单元的编译器崩溃不再拖垮整个会话——崩溃的工作进程会被重新拉起，其负责的文档按需恢复。有状态工作进程以 LRU 淘汰驻留文档，进程池在系统内存压力下会卸载后台负载。
- **头文件的编译上下文。** 打开头文件时，clice 会在某个包含它的源文件的上下文中编译它：重建 include 链合成 preamble（对 X-macro/`.def` 片段还会合成 suffix），使依赖 includer 宏的符号正确解析。编辑器可以通过 `clice/queryContext`、`clice/currentContext`、`clice/switchContext` 扩展列出并切换活跃上下文——同一机制也用于在一个文件的多个编译数据库条目之间切换。自包含的头文件会被识别并直接编译。
- **LSP 功能面。** Hover（定义打印、文档注释、含 size/offset/padding 的结构布局、表达式求值、`auto`/`decltype` 推导）；带签名详情、参数 snippet、废弃符号删除线以及 `#include`/`import` 路径补全的代码补全；签名帮助；跳转到定义（含 `#include` 行与模块名）、声明、实现、类型定义与查找引用（由项目索引提供）；文档符号；带丰富修饰符的语义高亮；inlay hints；折叠区间；文档链接（含 preamble 内与 `#embed`）；基于 `.clang-format` 的全文与区间格式化；调用层级与类型层级；工作区符号搜索；诊断发布；`#if` 关闭代码的置灰（`clice/inactiveRegions` 扩展）。
- **后台索引。** 持久化的项目级索引支撑跨文件功能，无需逐个打开文件。索引搭编译的便车、以低优先级并发执行、遇到交互请求会让路、通过 LSP `$/progress` 汇报进度，并跨重启复用。两层过期检测（先 mtime 后内容哈希）使 `touch` 和 `git checkout` 不会触发无谓重建。
- **编辑器之外的变化追踪。** stat 轮询的文件追踪器可以发现编辑器外发生的变化——重新生成的 `compile_commands.json`、`git checkout`、代码生成器——无需重启服务器。
- **C++20 命名模块。** 模块接口经由拉取式依赖图按需编译并缓存 PCM；支持 `import` 补全与模块名上的跳转定义。
- **统一磁盘缓存。** PCH、PCM 与索引产物存放在同一个内容寻址存储中，写入崩溃安全，跨会话与重启复用；可重建的产物（PCH/PCM）按大小上限淘汰。
- **配置。** `clice.toml`（或 `.clice/config.toml`）加 LSP `initializationOptions` 覆盖；带 glob 模式的按文件 `[[rules]]` 追加/移除编译标志；`[tracker]` 轮询间隔；基于 XDG 的缓存路径与 `${workspace}` 替换。配置文件格式错误会以带行列号的诊断报告在配置文件上。
- **工具 API。** 面向 AI agent 与外部工具的 TCP JSON-RPC API：项目文件、符号搜索、读取完整符号体、定义/引用、调用图、类型层级——以及 `clice query` 命令行。
- **编辑器扩展。** 仓库内维护的 VS Code（已发布到 Marketplace）、Neovim 与 Zed 扩展。
- **可运维性。** 按会话的文件日志与崩溃现场捕获；环境有问题（如找不到编译数据库）时通过 `window/logMessage` 给出指引；LSP 会话录制回放（`--record`）；`--version` 输出与发布 tag 一致。

### Changed

- **配置文件位置。** `config` 命令行选项已移除；配置从 `${workspace}/clice.toml`（或 `${workspace}/.clice/config.toml`）读取。
- **配置键更名。** `compile_commands_dirs` 更名为 `compile_commands_paths`；旧键会被忽略。
- **从 0.x 升级会重建缓存。** 磁盘缓存与索引格式已变化；升级后首次启动会丢弃旧产物并对项目重新索引一次。这同时淘汰了旧版本生成的 PCH——旧缓存键不含编译标志，可能用错误的宏配置提供结果。
- **工具链基线。** clice 现基于 LLVM/Clang 21.1.8 构建，并为 Linux、macOS、Windows 的 x64 与 arm64 提供预编译二进制。

### Fixed

- 快速连续编辑不再因编译/更新竞态产生虚假的"重定义"错误。
- 修复了多个畸形输入导致的工作进程崩溃（空 AST consumer、无效文件 ID、缺失缓存目录）；仍发生的崩溃由进程隔离兜住。
- 被工作进程 LRU 淘汰的文档不再无声失去 hover/语义高亮——下次请求时重新编译。
- 生命周期边界：早于 `initialize` 握手到达的 `didOpen` 会被受理；较新 LSP 客户端的未知枚举值不再导致握手失败；客户端就绪前产生的诊断会在 `initialized` 后补发。
- 保存头文件现在会正确地重索引包含它的已关闭文件，保持跨文件引用最新。

### Known gaps

- 代码补全、导航、文档链接与诊断可用但尚不完整（各功能状态见 feature overview 页面）。
- Code action 尚未实现（也未在 capabilities 中宣告）。
- clang-tidy 集成在计划中；`clang_tidy` 配置项会被解析但当前无效果。
