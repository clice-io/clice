# 格式化

## 概述

`clice format` 用 clice 内置的 clang-format 格式化工作区中的文件，与编辑器中的格式化一致，一次处理多个文件，并沿用项目中已有的 `.clang-format` 配置。clice 自己不添加任何风格设置：风格来自 clang-format 自身的查找规则，即每个文件向上最近的那个 `.clang-format`。

**用法**：`clice format [<path>...] [--workspace <dir>] [--configuration <tag>] [--check] [--jobs <n>]`

不带路径时，它格式化构建自身的文件：编译数据库中的每个翻译单元，以及它们包含的每个带 C 族后缀的文件——这些文件须位于工作区内，在构建树之外（构建树指根目录下存放编译数据库的目录，构建会在其中生成或拉取源文件），在其编译命令的系统包含目录之外，也在缓存目录之外。目录参数会把这个集合收窄到该目录下的文件；带 C 族后缀的文件参数则无论构建是否知道它都会被格式化。路径相对于工作区。`clice.toml` 中写有 `format = false` 的规则会把匹配到的文件排除在外，例如随项目附带的第三方库或生成的源文件：

```toml
[[rules]]
patterns = ["third_party/**", "gen/**"]
format = false
```

文件会被原地重写；符号链接指向的源文件会在其目标处重写，而目标也必须位于工作区内。`--check` 不改写任何文件，只打印 clang-format 会修改的地方，每处一条诊断，最后给出有多少个文件需要格式化。

退出码：`0` 表示所有文件都已格式化（带 `--check` 时表示不会有任何改动），`1` 表示 `--check` 发现了需要格式化的文件，`2` 表示本次运行未能完成——参数无法解析，某个文件或它的 `.clang-format` 无法读取，工作区、其配置或某个编译数据库无法找到或加载，或请求的配置不存在。

## 示例

### 构建所用的文件

不指定路径时，会格式化构建所用的文件；指定路径可以缩小处理范围，也可以将额外文件纳入格式化范围。

<!-- BEGIN GENERATED ITEMS: format/files -->

<!-- BEGIN CAPABILITY: supported -->

**格式化构建所用的文件**

不指定路径时，会格式化构建所用的源文件及其包含的头文件；随项目附带的第三方头文件和系统头文件保持不变

```case
tests/format/files/01_build_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**格式化指定路径**

指定文件时，无论该文件是否属于构建，都会对其进行格式化；指定目录时，只格式化该目录下构建所用的文件

```case
tests/format/files/02_named_paths
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 原地重写

运行时，会重写所有尚不符合相应风格要求的文件。

<!-- BEGIN GENERATED ITEMS: format/rewrite -->

<!-- BEGIN CAPABILITY: supported -->

**原地格式化**

不使用 `--check` 时，会按各文件对应的风格重写构建所用的所有文件

```case
tests/format/rewrite/01_format_in_place
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 检查

`--check` 会报告将要发生的改动，不写入任何内容。

<!-- BEGIN GENERATED ITEMS: format/check -->

<!-- BEGIN CAPABILITY: supported -->

**检查格式**

使用 `--check` 时，不会重写任何文件；会逐一报告 clang-format 将要修改的位置

```case
tests/format/check/01_check_formatting
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
