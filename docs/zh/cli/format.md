# 格式化

## 概述

`clice format` 对工作区中的文件运行 clang-format，同时启动多个进程，并沿用项目中已有的 `.clang-format` 配置。clice 自己不添加任何风格设置：风格来自 clang-format 自身的查找规则，即每个文件向上最近的那个 `.clang-format`。

**用法**：`clice format [--workspace <dir>] [--configuration <tag>] [--check] [--clang-format <path>] [--jobs <n>] [<path>...]`

不带路径时，它格式化构建自身的文件：编译数据库中的每个翻译单元，以及它们包含的每个带 C 族后缀的文件——这些文件须位于工作区内，在构建树之外（构建树指根目录下存放编译数据库的目录，构建会在其中生成或拉取源文件），在其编译命令的系统包含目录之外，也在缓存目录之外。目录参数会把这个集合收窄到该目录下的文件；带 C 族后缀的文件参数则无论构建是否知道它都会被格式化。路径相对于工作区。`clice.toml` 中写有 `format = false` 的规则会把匹配到的文件排除在外，例如随项目附带的第三方库或生成的源文件：

```toml
[[rules]]
patterns = ["third_party/**", "gen/**"]
format = false
```

文件会被原地重写。`--check` 不改写任何文件，只打印 clang-format 会修改的地方，每处一条诊断，最后给出有多少个文件需要格式化。`--clang-format` 指定要运行的可执行文件，默认使用 `clang-format`，在 `PATH` 中查找。

退出码：`0` 表示所有文件都已格式化（带 `--check` 时表示不会有任何改动），`1` 表示 `--check` 发现了需要格式化的文件，`2` 表示本次运行未能完成——没有找到 clang-format、它在某个文件上失败，或请求的配置不存在。
