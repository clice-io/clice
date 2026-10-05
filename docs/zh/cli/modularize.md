# 模块化

## 概述

`clice modularize` 将程序的第三方库转换为 C++20 具名模块（named modules），无需改动任何源文件。每个库仍以其头文件为准：生成的模块接口单元（module interface unit）会在全局模块片段（global module fragment）中包含这些头文件，并导出它们声明的所有命名空间作用域名称。程序保留原有的 `#include` 指令；位于头文件搜索路径最前面的目录会把已封装的头文件置空，而强制包含的前导头文件（prelude）会导入这些模块，并重新定义原头文件中定义的宏。

程序自身的代码也可以跟进：划分文件标记为改写的模块，其文件会被原地改写成模块单元，头文件变成分区（partition），包含变成导入。

**用法**：`clice modularize --partition <file> --out <dir> [--std <dir>] [--scope <glob,...>] [--workspace <dir>] [--configuration <tag>]`

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

- 未设置标志的模块会被封装：modularize 会生成其接口单元。
- `"external": true` 标记由已有接口代替的模块。仅支持 `std`：当 `--std` 指向 libc++ 的模块源文件目录（`share/libc++/v1`）时，会将其作为 `std.compat` 导入，并将 `std.cppm` 包含的标准头文件置空。
- `"textual": true` 让模块保持为头文件，就像 C 库和编译器自带的头文件在 `import std` 之外仍以头文件形式保留一样。程序只能经由已置空的头文件间接用到的那部分内容，会在前导头文件中直接包含，或重新定义相应的宏。
- `"provides": "std.compat"` 为以头文件形式保留的模块指定一个导出其名称的模块，因此只有后者未导出的内容才需要实际的头文件。
- `"rewrite": true` 标记要改写成具名模块的程序模块，见[改写程序](#rewriting-the-program)。`"primary"` 指定其主接口单元（primary interface unit）的位置；默认是包含该模块全部头文件的最深目录下的 `module.cppm`。

只需调整划分文件，既可以让每个库各自对应一个模块，也可以让所有库共用一个模块；分析范围必须涵盖划分文件指定的每个文件。

## 改写程序

被改写的模块成为一个具名模块，模块名即划分中的名称：

- 每个头文件变成一个分区，即同目录下的 `<stem>.cppm`，分区名取它在主接口单元所在目录下的路径（在 `src/` 下，`src/support/format.h` 是 `:support.format`）。其他模块的文件用到的头文件成为接口分区，由主接口重新导出；只有本模块文件用到的头文件成为实现分区。
- 每个源文件变成实现单元 `module <name>;`；它定义的 `main` 会移进 `extern "C++"`，以保持附属于全局模块。
- 包含同一模块的头文件，会改为导入对应分区，因此在模块内部，修改一个文件重新编译的范围与原先包含该头文件时相同；包含另一个被改写模块的头文件，会改为导入那个模块。源文件通过主接口看到本模块的接口分区，每个实现单元都会隐式导入主接口。
- 被封装库的头文件包含会被删除，由前导头文件导入对应模块；每个被改写的文件都在全局模块片段中首先包含前导头文件。仍保持为头文件的头文件，继续在那里包含。
- 独占一行的、对其他模块实体的声明（`class Foo;`）会被删除，改为导入该实体所属的模块；头文件中的匿名命名空间会被展开到外层命名空间。
- 头文件为其他文件定义的宏会移到同目录下的 `<stem>.macros.h`，内容是去掉包含指令后的预处理指令，所有用到这些宏的文件都会包含它。
- 如果源文件定义了另一个被改写模块的头文件所声明的实体，它会并入那个模块，因为定义必须附属于其声明所在的模块；未被改写的模块中包含了被改写头文件的源文件，会改为导入对应模块，仍是普通翻译单元。

改写哪些模块、模块划分得多粗，由划分文件决定：整个程序一个模块时，每个头文件都是分区，按文件导入；每个目录一个模块时，每个目录都有自己的接口，但修改一个模块的接口分区会导致该模块的所有导入方重新编译。改写之前可以用 [`clice analyze modules`](./analyze.md#modules) 权衡两者。

## 输出

`--out` 目录下的内容：

- 每个封装的模块对应一个 `<module>.cppm`，以及一个 `<module>.macros.h`；后者按定义顺序保存导入方所需的宏。
- `mirror/<module>/`：对于其他模块的文件所包含的每个头文件，按包含指令中使用的名称生成一个空文件；标准头文件对应的目录为 `mirror/std/`。
- `prelude.h`：仍需使用的 C 库头文件、`import std.compat;`、所有导入语句及所有宏头文件。

内容未变的文件会保留时间戳。上次运行写出而本次不再生成的文件会被删除；`--out/.modularize` 记录每次运行写出的文件，`--out` 下的其他内容不会改动。

被改写的文件会原地写入，被分区替代的头文件会被删除。

标准输出会给出构建计划。`wrapping` 列出模块源文件、镜像目录和前导头文件（使用相对于 `--out` 的路径），以及按查找到的路径列出的 libc++ 源文件和各库的头文件搜索根目录；`rewriting` 列出被改写模块的各个单元，使用相对于工作区的路径：

```json
{
  "wrapping": {
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
  },
  "rewriting": {
    "modules": [
      {
        "name": "app.core",
        "primary": "src/core/module.cppm",
        "interfaces": ["src/core/text.cppm"],
        "partitions": ["src/core/detail.cppm"],
        "sources": ["src/core/text.cpp"],
        "imports": []
      }
    ],
    "importers": ["src/main.cpp"],
    "macros": ["src/core/text.macros.h"],
    "removed": ["src/core/detail.h", "src/core/text.h"],
    "moved": [],
    "warnings": []
  }
}
```

构建时按顺序编译 `stdSources` 和各模块，分别使用对应库的头文件搜索根目录，并将各自的 `mirrors` 放在头文件搜索路径的最前面，同时为改写未触及的每次程序编译添加 `mirrors` 和 `-include prelude.h`。被改写的模块把主接口、`interfaces` 和 `partitions` 编译为模块接口单元，把 `sources` 编译为实现单元，并把工作区根目录加入头文件搜索路径以找到前导头文件；`moved` 以 `path=module` 的形式列出并入其他模块的源文件。

## 已知限制

- 模块单元必须使用完整 BMI 构建（clang 中使用 `-fno-modules-reduced-bmi`）：精简 BMI 会丢弃全局模块片段中未在模块辖域（purview）内按名称引用的声明，其中就包括偏特化。
- 如果头文件包含程序按名称引用的内部链接实体，就必须继续以文本方式包含，因为任何模块都无法导出该实体。前导头文件不会包含这类头文件；如果程序文件原先只通过库中已置空的头文件间接包含它，就需要自行包含。
- 其他文件通过相对于自身目录的路径（`"../foo.h"`）包含的头文件不能置空；modularize 会对此发出警告。
- 使用已封装库的每个翻译单元都必须通过模块访问该库；将同一组头文件的文本包含与模块导入混用，会因 clang 无法合并的重复声明而出错。
- 程序的每次编译都会导入所有模块、包含所有宏头文件，不管它原先包含过哪些库。
- 标准库模块来自 libc++。
- 只有索引中的文件会被改写：被索引的构建配置中没有任何编译用到的源文件（例如其他平台的源文件）会保留原有的包含指令。
- clang 不认为实现分区的全局模块片段对经由另一个分区间接导入它的单元可达，尽管标准规定该单元同样导入了它。因此被改写的文件会自行包含这类分区所包含的、仍保持为头文件的头文件，并再解析一遍。
- 位于预处理条件中的被改写头文件的包含，会变成无条件的导入。
- 仍保持为头文件的头文件不能包含被改写的头文件；modularize 会对此发出警告。
