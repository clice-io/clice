# 模块化

## 概述

`clice modulize` 将程序的第三方库转换为 C++20 具名模块（named modules），无需改动任何源文件。每个库仍以其头文件为准：生成的模块接口单元（module interface unit）会在全局模块片段（global module fragment）中包含这些头文件，并导出它们声明的所有命名空间作用域名称。程序保留原有的 `#include` 指令；位于头文件搜索路径最前面的目录会把已封装的头文件置空，而强制包含的前导头文件（prelude）会导入这些模块，并重新定义原头文件中定义的宏。

**用法**：`clice modulize --partition <file> --out <dir> [--std <dir>] [--scope <glob,...>] [--workspace <dir>] [--configuration <tag>]`

所有信息都来自持久化索引，可通过 [`clice analyze modules --view interface`](./analyze.md#modules) 查看；请先运行 `clice index`。该命令会写入文件并输出构建所需的信息，但不会修改任何构建文件。

## 划分

划分文件通过 glob 模式匹配相对于工作区的路径（工作区外的文件使用绝对路径），将文件划分到各模块，以首个匹配项为准。未匹配任何 glob 模式的文件属于程序本身，仍作为头文件使用。

```json
{
  "modules": [
    { "name": "libc", "files": ["/opt/llvm/include/c++/v1/*.h"] },
    { "name": "std", "files": ["/opt/llvm/include/c++/v1/**"], "external": true },
    {
      "name": "libc",
      "files": ["/usr/include/**", "/opt/llvm/lib/clang/23/include/**"],
      "textual": true,
      "provides": "std.compat"
    },
    { "name": "llvm", "files": ["third_party/llvm/include/**"] },
    { "name": "fmt", "files": ["third_party/fmt/include/**"] }
  ]
}
```

- 未设置标志的模块会被封装：modulize 会生成其接口单元。
- `"external": true` 标记由已有接口代替的模块。仅支持 `std`：当 `--std` 指向 libc++ 的模块源文件目录（`share/libc++/v1`）时，会将其作为 `std.compat` 导入，并将 `std.cppm` 包含的标准头文件置空。
- `"textual": true` 让模块保持为头文件，就像 C 库和编译器自带的头文件在 `import std` 之外仍以头文件形式保留一样。程序只能经由已置空的头文件间接用到的那部分内容，会在前导头文件中直接包含，或重新定义相应的宏。
- `"provides": "std.compat"` 为以头文件形式保留的模块指定一个导出其名称的模块，因此只有后者未导出的内容才需要实际的头文件。

只需调整划分文件，既可以让每个库各自对应一个模块，也可以让所有库共用一个模块；分析范围必须涵盖划分文件指定的每个文件。

## 输出

`--out` 目录下的内容：

- 每个封装的模块对应一个 `<module>.cppm`，以及一个 `<module>.macros.h`；后者按定义顺序保存导入方所需的宏。
- `mirror/<module>/`：对于其他模块的文件所包含的每个头文件，按包含指令中使用的名称生成一个空文件；标准头文件对应的目录为 `mirror/std/`。
- `prelude.h`：仍需使用的 C 库头文件、`import std.compat;`、所有导入语句及所有宏头文件。

内容未变的文件会保留时间戳。上次运行生成而本次不再生成的文件（镜像文件、接口单元、宏头文件）会被删除；`--out` 下的其他内容不会改动。

标准输出会给出构建计划：模块源文件、镜像目录和前导头文件使用相对于 `--out` 的路径，libc++ 的源文件和各库的头文件搜索根目录则按查找到的路径列出：

```json
{
  "stdSources": ["share/libc++/v1/std.cppm", "share/libc++/v1/std.compat.cppm"],
  "modules": [
    {
      "name": "llvm",
      "source": "llvm.cppm",
      "imports": [],
      "includeRoots": ["third_party/llvm/include"],
      "mirrors": ["mirror/std"]
    }
  ],
  "mirrors": ["mirror/std", "mirror/llvm"],
  "prelude": "prelude.h",
  "warnings": []
}
```

构建时按顺序编译 `stdSources` 和各模块，分别使用对应库的头文件搜索根目录，并将各自的 `mirrors` 放在头文件搜索路径的最前面，同时为程序的每次编译添加 `mirrors` 和 `-include prelude.h`。

## 已知限制

- 模块单元必须使用完整 BMI 构建（clang 中使用 `-fno-modules-reduced-bmi`）：精简 BMI 会丢弃全局模块片段中未在模块辖域（purview）内按名称引用的声明，其中就包括偏特化。
- 如果头文件包含程序按名称引用的内部链接实体，就必须继续以文本方式包含，因为任何模块都无法导出该实体。前导头文件不会包含这类头文件；如果程序文件原先只通过库中已置空的头文件间接包含它，就需要自行包含。
- 其他文件通过相对于自身目录的路径（`"../foo.h"`）包含的头文件不能置空；modulize 会对此发出警告。
- 使用已封装库的每个翻译单元都必须通过模块访问该库；将同一组头文件的文本包含与模块导入混用，会因 clang 无法合并的重复声明而出错。
- 标准库模块来自 libc++。
