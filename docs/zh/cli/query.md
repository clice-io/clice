# 查询

## 概述

`clice query` 向持久化索引提出结构化问题，并将答案以 JSON 形式输出到 stdout——适合不想对接 LSP、又想获取语言服务器所掌握信息的脚本与 coding agent。它直接从磁盘读取索引，读取的正是 `clice index` 构建、编辑器会话持续更新的那份索引，因此编辑器打开时可以照常运行，未打开时也无需启动服务器。

**用法**：`clice query <question> [<locator> | <file>] [--workspace <dir>] [--configuration <tag>] [--fresh] [question options]`

## 问题

- `symbol-search [<locator>] [--limit <n>]` 列出[定位串](#locators)（locator）匹配到的符号，最佳匹配排在最前，附带种类、文件、行号、所属容器和 `symbol`；函数还会附上区分各个重载的 `signature`。在多处定义的符号——弱函数及其强定义、每个程序各自的 `main`——会在每处定义各列一次；没有任何文件定义的宏则列在 `<command line>`（编译命令中的 `-D`）或 `<built-in>`（编译器内置宏）。
- `definition <locator>`、`read-symbol <locator>`、`references <locator> [--include-declaration]`、`call-graph <locator> [--direction callers|callees|both]`、`type-hierarchy <locator> [--direction supertypes|subtypes|both]` 和 `implementation <locator>` 针对定位串指明的那一个符号作答：没有任何定义时，`definition` 给出它的声明；在多处定义时，把其余定义列在 `otherDefinitions` 中；没有任何文件定义的宏，则给出 `<command line>` 或 `<built-in>`；每条引用都会指明所在的函数；`implementation` 列出类之下各层的所有派生类，或函数之下各层的所有重写函数。定位串匹配到多个符号时，会将它们列在 `candidates` 中；没有任何符号与所写名称完全一致（区分大小写）时，不会指明任何符号，而是同样把最接近的匹配列出来，哪怕只有一个。`definition` 和 `read-symbol` 还会给出符号的 `documentation`：以 Markdown 形式返回其文档注释，解析方式与编辑器的悬停一致——展开 `@param`、`@return` 和 `@copydoc`，无注释的重写函数则显示被重写函数的文档。
- `document-symbols <file>` 给出该文件的大纲。
- `project-files [--filter all|source|header|module]` 列出构建涉及的文件；`file-deps <file> [--direction includes|includers|both] [--depth <n>]` 沿包含关系图查询，`impact-analysis <file>` 则同时沿包含关系图以及导入该文件所属模块的文件查询。
- `counterparts <file>` 列出与该文件配对的文件——头文件的源文件、源文件的头文件、模块的接口单元与实现单元——最佳匹配排在最前，每项均附带配对理由，并将编辑器无需询问就会直接打开的那一个标为 `preferred`（参见[切换源文件／头文件](../features/navigation.md#switch-source-header)）。

问题中的路径可以是相对于当前目录的相对路径或绝对路径，答案中的路径一律为绝对路径；行号从 1 开始。

## 定位串

凡是接受定位串的命令，都用它来指明符号。定位串是一个字符串：空格分隔各个词项，引号和尖括号内的空格则原样保留。其中一个词项指明符号，其余词项用来收窄结果。

| 定位串                       | 查找结果                                                                                                                                                                                                                                |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `foo`                        | 名称按单词边界对齐、可将 `foo` 作为子序列匹配的符号——`LinLis` 找到 `LinkedList`，`up` 找到 `unique_ptr`——完全一致的名称排在最前，其次是以查询词开头的名称，再次是其余匹配；六个字母及以上的查询还会找到相差一处拼写错误的名称，排在最后 |
| `"foo"`                      | 完整名称，区分大小写                                                                                                                                                                                                                    |
| `foo*`、`*_test`、`get?Name` | 该 glob 模式匹配的名称；模式中一旦含有大写字母即区分大小写                                                                                                                                                                              |
| `ns::Foo::bar`               | 位于外层容器链按序包含 `ns` 和 `Foo` 的容器内的符号，其间与前后允许有其他容器                                                                                                                                                           |
| `::ns::Foo::bar`             | 恰好位于该容器内的符号                                                                                                                                                                                                                  |
| `ns::*`、`ns::**`            | 该容器的直接成员；该容器之下的全部符号                                                                                                                                                                                                  |
| `Widget<int>`                | 以这些实参写出的模板特化                                                                                                                                                                                                                |
| `#1a2b3c`                    | 具有该 ID 的符号；仅部分文件可见的符号还需附上 `path:` 指明一个含有它的文件，相对路径或绝对路径均可                                                                                                                                     |
| `src/a.cpp:120`              | 在该行定义的符号                                                                                                                                                                                                                        |
| `src/a.cpp:120:8`            | 该光标位置下的符号，行号和字节列号均从 1 开始                                                                                                                                                                                           |
| `kind:function,method`       | 仅保留这些种类                                                                                                                                                                                                                          |
| `path:src/index/`            | 在该文件内或该目录下声明的符号，路径相对于当前目录；只写文件名（如 `path:a.cpp`）时匹配所有同名文件                                                                                                                                     |

局部变量或参数只有其所在文件知晓：按名称查找时，必须同时用 `path:` 指明该文件或其所在目录。命名空间作用域内具有内部链接的实体——`static` 实体，或位于匿名命名空间中的实体——无论在何处都能直接按名称找到。

答案列出的每个符号都带有 `symbol`，即反过来指明该符号的定位串：内容为其 ID，仅部分文件可见的符号还会在后面跟上 `path:` 及其所在文件。将其原样传回即可。在 shell 命令行中，请给整个定位串加上引号：在那里 `#` 会触发注释，`<` 和 `>` 会触发重定向，`*` 会被通配展开。

## 答案

每个答案都是一个 JSON 对象：成功时为 `{"result": ..., "stale": [...]}`；问题无法回答时为 `{"error": "...", "stale": [...]}`，退出码为 1（如未知符号、文件不存在，或 `--depth` 为负数）；参数无法解析（如未知问题、定位串或文件缺失或过多、`--filter` 或 `--direction` 的值不在可选范围内）或无法读取项目及其索引（如找不到项目、配置无法解析、索引数据库损坏）时同样返回错误对象，退出码为 2。定位串匹配到多个符号时，错误对象会在 `candidates` 中列出这些候选符号，每项均带有各自的 `symbol`。

`stale` 列出答案不得不略过其条目的文件，因为这些文件在磁盘上的内容已与建立索引时不一致——索引为它们保存的位置会指向已经偏移的文本。仅因所包含的头文件发生更改的文件，仍使用上一次的索引作答。定义位于过期文件中的符号会报告为未找到，并指明该文件，以便调用方区分“符号不存在”与“尚未重新索引”。只有答案实际查阅过的文件才会被检查：文件建立索引后新增的符号只会直接缺席，因为没有任何索引条目指向该文件——若要查询磁盘的当前内容，请使用 `--fresh`。索引从不读取编辑器中未保存的缓冲区，始终以磁盘内容为准。

`--fresh` 会在作答前将索引更新到与磁盘一致：若当前有编辑器会话打开着该项目，由其服务器执行本轮更新；否则由命令自行运行索引器，尚无索引时从零构建。无论哪种方式，都只重新编译输入发生更改的翻译单元。不带 `--fresh` 时，尚无索引的项目会返回错误。

## 示例

### 问题

每个问题指明一个符号或文件，并返回索引记录的相关信息。

<!-- BEGIN GENERATED ITEMS: query/basics -->

<!-- BEGIN CAPABILITY: supported -->

**符号的引用**

列出提及某个方法的所有位置，包括其声明以及经由被重写方法发起的调用，每处均附带所在函数

```case
tests/query/basics/01_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按名称搜索符号**

名称查询匹配按单词边界对齐的子序列

```case
tests/query/basics/02_symbol_search
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关于符号的问题**

按名称、ID 或行号查找符号，查询其定义与使用位置、调用方、基类、实现以及文件所声明的符号

```case
tests/query/basics/03_symbol_questions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关于文件的问题**

列出项目文件、某文件包含的文件与包含它的文件、修改头文件的影响范围，以及配对的源文件

```case
tests/query/basics/04_file_questions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文档**

符号的文档注释随其定义一并返回，解析方式与编辑器显示的一致

```case
tests/query/basics/10_documentation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有定义的符号**

没有任何定义的符号以声明作为定义返回，与编辑器中跳转到定义的答案一致

```case
tests/query/basics/11_declaration_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**经由实例化得出的答案**

引用与调用图包含由模板实例化和宏展开解析出的结果，每条均用 `via` 标明来源

字面代码本身无法确定的使用——模板中的依赖调用、宏体中的名称——在答案中带有 `"via": "instantiation"` 或 `"via": "expansion"`；字面代码直接确定的答案不带 `via`。调用方仅在其全部调用都来自实例化时才带有该字段。

```case
tests/query/basics/12_instantiations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 按路径收窄范围

`path:` 词项可将搜索范围收窄到部分文件，也能查找仅单个文件可见的名称。

<!-- BEGIN GENERATED ITEMS: query/names -->

<!-- BEGIN CAPABILITY: supported -->

**文件局部符号**

静态符号可在任意范围按名称找到；仅在所属函数内可见的局部变量与参数，可配合 `path:` 在所在文件内按名称找到

```case
tests/query/names/05_file_local_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按路径收窄范围**

`path:` 过滤条件仅保留指定路径下文件中的符号，可写相对于当前目录的路径、纯文件名或绝对路径

```case
tests/query/names/06_path_filters
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 已更改的文件

答案会略过建立索引后发生更改的文件，`--fresh` 则会先将索引更新到最新状态。

<!-- BEGIN GENERATED ITEMS: query/freshness -->

<!-- BEGIN CAPABILITY: supported -->

**建立索引后更改的文件**

建立索引后磁盘内容发生更改的文件，其索引条目会在答案中略过，并在 `stale` 中列出文件名

```case
tests/query/freshness/01_changed_files_withheld
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**作答前刷新索引**

传入 `--fresh` 时，会先将索引更新到最新状态再作答；尚无索引则会先建立索引

```case
tests/query/freshness/02_fresh_batch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
