# 查询

## 概述

`clice query` 向持久化索引提出一个结构化的问题，并把答案以 JSON 形式打印到 stdout——供那些不想说 LSP、又想拿到语言服务器所掌握事实的脚本和 coding agent 使用。它直接从磁盘读取索引，正是 `clice index` 构建、编辑器会话持续更新的那一份，因此编辑器开着时它照常工作，编辑器没开时也不需要服务器。

**用法**：`clice query <question> [<locator> | <file>] [--workspace <dir>] [--configuration <tag>] [--fresh] [question options]`

## 支持的问题

- `symbol-search [<locator>] [--limit <n>]` 列出[定位串](#locators)（locator）匹配到的符号，最佳匹配排在最前，并附上各符号的种类、文件、行号、所属容器和 `symbol`。
- `definition <locator>`、`read-symbol <locator>`、`references <locator> [--include-declaration]`、`call-graph <locator> [--direction callers|callees|both]`、`type-hierarchy <locator> [--direction supertypes|subtypes|both]` 和 `implementation <locator>` 针对定位串指明的那一个符号作答：没有任何定义时，`definition` 给出它的声明；每条引用都会指明它所在的函数；`implementation` 列出类之下各层的所有派生类，或函数之下各层的所有重写函数。定位串匹配到多个符号时，会把它们作为 `candidates` 列出；没有符号与该名称完全一致时，同样会列出最接近的匹配。`definition` 和 `read-symbol` 还会给出符号的 `documentation`：以 markdown 形式给出它的文档注释，读法与编辑器的悬停一致——解析 `@param`、`@return` 和 `@copydoc`，没有注释的重写函数显示它所重写函数的文档。
- `document-symbols <file>` 给出该文件的大纲。
- `compile-command <file>` 给出编辑器编译该文件时会使用的命令，以及它的来源：文件自身的数据库条目、头文件的宿主源文件、规则的默认命令、从邻近翻译单元借来的命令（借给临时成员时为 `provisional`，借给构建之外的文件时为 `inferred`），或内置的回退命令。给出的是经编译器解析后的命令，带有目标平台和系统头文件目录；询问编译器失败时，`toolchainError` 说明原因，命令则停留在未经解析的驱动命令。
- `project-files [--filter all|source|header|module]` 列出构建涉及的文件；`file-deps <file> [--direction includes|includers|both] [--depth <n>]` 沿包含关系图查询，`impact-analysis <file>` 则沿包含关系图以及导入该文件所属模块的文件查询。
- `counterparts <file>` 列出与该文件配对的文件——头文件的源文件、源文件的头文件、模块的接口单元和实现单元——最佳的排在最前，每个都附上理由，并把编辑器无需询问就会直接打开的那一个标为 `preferred`（参见[切换源文件／头文件](../features/navigation.md#switch-source-header)）。

问题中的路径可以相对于工作区，也可以是绝对路径，答案中的路径一律是绝对路径；行号从 1 开始。

## 定位串

定位串用来指明符号，凡是接受定位串的命令都是如此。它就是一个字符串：空格分隔其中的各个词项，引号和尖括号里的空格则原样保留。其中一项指明符号，其余各项用来收窄答案。

| 定位串                       | 匹配结果                                                                                                                                                                                                                                |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `foo`                        | 名称能按单词对齐、把 `foo` 作为子序列匹配上的符号——`LinLis` 找到 `LinkedList`，`up` 找到 `unique_ptr`——名称完全一致的排在最前，其次是以查询开头的名称，然后才是其余的；查询有六个及以上字母时，还会找到相差一处拼写错误的名称，排在最后 |
| `"foo"`                      | 完整的名称，区分大小写                                                                                                                                                                                                                  |
| `foo*`、`*_test`、`get?Name` | 该 glob 模式匹配上的名称；模式中一旦出现大写字母就区分大小写                                                                                                                                                                            |
| `ns::Foo::bar`               | 位于某个容器内的符号，且该容器链按此顺序列出 `ns` 和 `Foo`，其间和两端允许有别的容器                                                                                                                                                    |
| `::ns::Foo::bar`             | 恰好位于该容器内的符号                                                                                                                                                                                                                  |
| `ns::*`、`ns::**`            | 该容器的成员；以及它之下的全部内容                                                                                                                                                                                                      |
| `Widget<int>`                | 以这些实参写出的那个特化                                                                                                                                                                                                                |
| `#1a2b3c`                    | 具有该 id 的符号；只有部分文件能看到的符号，还要用 `path:` 给出一个含有它的文件，路径可以相对于工作区，也可以是绝对路径                                                                                                                 |
| `src/a.cpp:120`              | 在该行上定义的符号                                                                                                                                                                                                                      |
| `src/a.cpp:120:8`            | 该光标处的符号，行号从 1 开始，列号按字节计                                                                                                                                                                                             |
| `kind:function,method`       | 只保留这些种类                                                                                                                                                                                                                          |
| `path:src/index/`            | 在该文件中或该目录下声明的符号，路径是从工作区根目录起算的相对路径；只写文件名时（如 `path:a.cpp`）匹配所有同名文件                                                                                                                     |

局部变量或参数只有它所在的文件才知道：按名称查找时，必须同时用 `path:` 给出该文件或该文件所在的目录。命名空间作用域中具有内部链接的实体（`static` 实体，或位于匿名命名空间中的实体）则无论位于何处，都能按名称找到。

答案列出的每个符号都带有 `symbol`，即反过来指明该符号的定位串：它的 id；只有部分文件能看到的符号，id 后面再跟上 `path:` 和它所在的文件。把它原样传回即可。在 shell 命令行中，要给整个定位串加上引号：在那里 `#` 会开始一段注释，`<` 和 `>` 会重定向，`*` 会被展开。

## 答案

每个答案都是一个 JSON 对象：成功时是 `{"result": ..., "stale": [...]}`；问题无法回答时（符号不存在、文件不存在、选项取了不支持的值）是 `{"error": "...", "stale": [...]}`，退出码为 1；参数无法解析时（问题未知，定位串或文件缺失或多余）同样如此，退出码为 2。定位串匹配到多个符号时，错误中会用 `candidates` 列出它们，每个都附上自己的 `symbol`。

`stale` 列出答案不得不略过其数据行的文件：它们在磁盘上的内容已经与建立索引时不一致，索引为它们保存的位置会指向移动过的文本。仅因所包含的头文件发生变化的文件，仍用它上一次的数据行作答。定义位于已过期文件中的符号会被报告为未找到，并给出文件名，这样读者就能区分“不存在”和“尚未建立索引”。只有答案查阅过的文件才会被检查：在文件建立索引之后新增的符号只会缺席，因为没有任何数据行指向那个文件——要询问磁盘的当前状态，请用 `--fresh`。索引从不读取编辑器中未保存的缓冲区：它描述的是磁盘上的内容。

`--fresh` 会在作答前把索引更新到与磁盘一致：若编辑器会话正打开着该工作区，由它的服务器执行这轮索引；否则由命令自己运行索引器，尚无索引时从零开始建立。两种情况下都只重新编译输入发生变化的翻译单元。不加 `--fresh` 时，没有索引的工作区只会返回错误。

## 示例

### 问题

每个问题都指定一个符号或文件，答案包含索引中有关它的信息。

<!-- BEGIN GENERATED ITEMS: query/basics -->

<!-- BEGIN CAPABILITY: supported -->

**符号的引用**

所有出现某个方法名称的位置，包括其声明，每处都附上它所在的函数

```case
tests/query/basics/01_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按名称搜索符号**

名称查询会匹配按单词边界对齐的子序列

```case
tests/query/basics/02_symbol_search
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关于符号的问题**

按名称、id 或所在行查找符号，查询它的定义和使用位置、调用者、它继承自哪些类型、哪些类型继承自它，以及文件中有哪些声明

```case
tests/query/basics/03_symbol_questions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关于文件的问题**

列出项目中的文件、某个文件包含的文件及包含它的文件、修改头文件的影响范围，以及与头文件对应的源文件

```case
tests/query/basics/04_file_questions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**文档**

符号的文档注释随它的定义一起给出，读法与编辑器显示的一致

```case
tests/query/basics/10_documentation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有定义的符号**

没有任何定义的符号，其定义就是它的声明，与编辑器中“跳转到定义”给出的答案一致

```case
tests/query/basics/11_declaration_only
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 按路径缩小范围

使用 `path:` 条件可以将搜索范围缩小到部分文件，并找到仅在单个文件中可见的名称。

<!-- BEGIN GENERATED ITEMS: query/names -->

<!-- BEGIN CAPABILITY: supported -->

**文件局部符号**

静态符号在任何地方都能按名称找到；局部变量和参数只有所在的函数能指称，借助 `path:` 可以在其所在文件内按名称找到

```case
tests/query/names/05_file_local_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按路径缩小范围**

`path:` 过滤条件只保留指定路径下文件中的符号；路径可写为相对于工作区根目录的路径、文件名或绝对路径

```case
tests/query/names/06_path_filters
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 已更改的文件

答案会略过建立索引后发生更改的文件，`--fresh` 则会先将索引更新到最新状态。

<!-- BEGIN GENERATED ITEMS: query/freshness -->

<!-- BEGIN CAPABILITY: supported -->

**索引建立后更改的文件**

对于建立索引后磁盘内容发生更改的文件，答案会略过索引中记录的相关信息，并在 `stale` 中列出这些文件

```case
tests/query/freshness/01_withheld_rows
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**回答前刷新索引**

使用 `--fresh` 时，会先将索引更新到最新状态再回答问题；如果尚无索引，则会先建立索引

```case
tests/query/freshness/02_fresh_batch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
