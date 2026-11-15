# 格式化

## 概述

`clice format` 使用 clice 内置的 clang-format 格式化项目文件，行为与编辑器一致，一次处理多个文件，并沿用源码树中已有的 `.clang-format` 配置。clice 不额外定义任何风格选项：风格完全遵循 clang-format 自身的查找规则，取每个文件上层目录中最近的一份 `.clang-format`。

**用法**：`clice format [<path>...] [--workspace <dir>] [--configuration <tag>] [--check] [--jobs <n>]`

不指定路径时，它会格式化构建自身的文件：编译数据库中位于构建树之外的每个翻译单元（构建树指含有 `CMakeCache.txt` 或 `build.ninja` 的目录，FetchContent 拉取的源码就存放在这类目录中），以及这些翻译单元包含的、带 C 语言族后缀的文件——前提是文件位于项目内部，且不在构建树（根目录下存放编译数据库的目录，构建生成或拉取的源码位于其中）、编译命令的系统包含目录及缓存目录之内；根目录中嵌套的子项目（带有自身 `clice.toml` 或 `.clice/config.toml` 的目录）内的文件一律不格式化。传入目录参数会将范围收窄到该目录下的上述文件；传入带 C 语言族后缀的文件参数时，无论构建是否包含该文件都会格式化。路径均相对于当前目录解析。在 `clice.toml` 中为规则设置 `format = false` 可排除匹配的文件，例如随仓库维护的第三方库或生成代码：

```toml
[[rules]]
patterns = ["third_party/**", "gen/**"]
format = false
```

文件会原地重写；若源文件是符号链接，则重写其指向的目标文件，且目标文件同样必须位于项目内。`--check` 不改写任何文件，只输出 clang-format 将会改动的位置（每处一条诊断），最后汇总需要格式化的文件数量。

退出码：所有文件均已完成格式化（或使用 `--check` 时无需任何改动）返回 `0`；`--check` 发现有文件需要格式化返回 `1`；运行未能完成则返回 `2`，包括参数无法解析、文件或其 `.clang-format` 无法读取、找不到或无法加载项目、项目配置或编译数据库，以及所请求的配置不存在。

## 示例

### 构建中的文件

不指定路径时会格式化构建自身的文件；指定路径可收窄处理范围或加入额外文件。

<!-- BEGIN GENERATED ITEMS: format/files -->

<!-- BEGIN CAPABILITY: supported -->

**格式化构建中的文件**

不指定路径时，会格式化构建中的源文件及其包含的头文件，第三方头文件和系统头文件保持原样

```case
tests/format/files/01_build_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**格式化指定路径**

指定文件时，无论构建是否包含该文件都会格式化；指定目录时，则将范围收窄到该目录下属于构建的文件

```case
tests/format/files/02_named_paths
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 原地重写

运行命令会重写所有尚不符合对应风格的文件。

<!-- BEGIN GENERATED ITEMS: format/rewrite -->

<!-- BEGIN CAPABILITY: supported -->

**原地格式化**

不带 `--check` 时，构建中的每个文件都会按其风格原地重写

```case
tests/format/rewrite/01_format_in_place
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 格式检查

`--check` 只报告将会发生的改动，不写入任何内容。

<!-- BEGIN GENERATED ITEMS: format/check -->

<!-- BEGIN CAPABILITY: supported -->

**检查格式**

带 `--check` 时不重写任何文件，逐处报告 clang-format 将会修改的位置

```case
tests/format/check/01_check_formatting
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
