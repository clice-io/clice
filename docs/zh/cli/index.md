# 索引

## 概述

`clice index` 预先为工作区构建持久化符号索引，让编辑器会话热启动，而不是在你工作时才在后台建立索引。它用 worker 池运行与服务器相同的索引流程，两者共用 `cache_dir` 下的索引，每个构建配置各一份。

**用法**：`clice index [--workspace <dir>] [--configuration <tag>] [--jobs <n>] [--json]`

同一时刻只有一个进程为工作区写索引。编辑器会话打开着工作区时，这个角色由它的服务器担任，因此 `clice index` 会请服务器执行这轮索引并等待结果，而不是自己启动一个 worker 池；摘要中会说明这一点。若锁被一个无法请求的进程持有（另一个 `clice index`、另一个 clice 构建的服务器，或配置里关闭了后台索引的服务器），命令会失败并给出持有者——等那个进程结束后再重新运行。

索引构建可在中断后继续：按 Ctrl-C 或发送 SIGTERM（`timeout` 和 CI 任务时限发的就是它）中断会保存当前进度，下次执行时会从中断处继续；只有再按一次 Ctrl-C 才会不保存、立即退出。每五分钟也会保存一次进度，因此崩溃最多只会损失五分钟的工作。运行期间，无论日志级别如何，每十秒都会向 stderr 输出一行进度；最终在 stdout 输出的摘要会统计已建立索引的翻译单元数、生成的文件分片数和符号数，列出本次运行中每个编译出错的翻译单元及其第一条错误（它的索引保留解析出的部分）、所有索引构建失败的翻译单元，并在这些之外另行列出即使单独运行也放不进内存的每个翻译单元，附上其 worker 达到的内存；摘要还会给出会话日志目录。`--json` 则改为以一个 JSON 对象输出同一份报告，其中附有每个出错翻译单元的全部错误。通过运行中的服务器执行时，摘要及其 JSON 只列出索引构建失败的翻译单元，放不进内存的也算在其中。索引完整时退出码为 0，部分翻译单元索引失败或放不进内存，或者部分索引无法持久化时为 1，参数无法解析时为 2，中断时为 130。

## 查看索引

同一条命令的这些操作读取已持久化的索引，而不改动它。每个操作都输出纯文本行，便于在终端查看或交给 `grep` 处理；即使编辑器会话正打开着索引，它们也能正常工作。

- `clice index stats [--limit <n>]` 输出索引内容的统计信息：翻译单元和文件分片、分片各列及各类已存记录的字节占用明细、符号按引用文件数和名称长度的分布、文件分片按变体数量的分布，以及最大的分片。`--variants` 会为每个文件分片追加一行以制表符分隔的记录，其中包含该分片的变体数量，便于比较两次运行的结果。
- `clice index symbol <locator>` 输出索引中记录的、由[定位串](./query.md#locators)指明的那些符号的信息：种类、名称和标志，拼出限定名的父级链，规范声明所在的文件，以及每个符号在各文件中定义、声明和引用的数量。名称会指明完整写出该名称的所有符号（`Box<int>`、`ns::Box<int>`），只要 `clice query` 能找到它们；id 即使在没有任何文件声明该符号时也能指明它，例如在命令行上定义的宏。
- `clice index file <path>` 输出文件的分片：构建它所依据的内容版本、每个变体及贡献它的翻译单元、每种关系的数量，以及身份只存在于该文件中的文件局部符号。
- `clice index unit <path>` 输出翻译单元的清单：索引时间、它为每个文件贡献的变体，以及它的包含树。

## 示例

### 构建索引

每次运行都会为构建中的所有翻译单元建立索引，规则排除的翻译单元除外。

<!-- BEGIN GENERATED ITEMS: index/batch -->

<!-- BEGIN CAPABILITY: supported -->

**构建索引**

为构建中的每个翻译单元建立索引，让编辑器会话热启动

```case
tests/index/batch/01_build_the_index
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**将翻译单元排除在索引构建之外**

设置了 `index = false` 的规则会将匹配的翻译单元排除在索引构建之外；打开时仍会编译这些翻译单元

```case
tests/index/batch/02_keep_units_out
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**无法编译的翻译单元**

建立索引时会指出每个编译出错的翻译单元，并给出它的第一条错误

这样的翻译单元照样会建立索引：它的索引保留解析出的部分。

```case
tests/index/batch/05_compile_errors
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 在服务器运行时

编辑器的服务器持有索引时，`clice index` 会将索引工作交给它。

<!-- BEGIN GENERATED ITEMS: index/delegation -->

<!-- BEGIN CAPABILITY: supported -->

**通过运行中的服务器**

编辑器的 clice 服务器运行期间，`clice index` 和 `--fresh` 会请求该服务器建立索引，而不是与其同时建立索引

```case
tests/index/delegation/01_running_server
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
