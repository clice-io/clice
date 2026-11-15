# 模块化

## 概述

`clice modularize` 将程序的第三方库转换为 C++20 具名模块（named modules），无需改动任何源文件。每个库仍以其头文件为准：生成的模块接口单元（module interface unit）在全局模块片段（global module fragment）中包含这些头文件，并导出它们声明的所有命名空间作用域名称。程序保留原有的 `#include` 指令；位于头文件搜索路径首位的目录会将已封装的头文件置为空文件（除非传入 `--no-mirrors`），而强制包含的前导头文件（prelude）会导入这些模块并包含 `macros.h`——后者重放原头文件定义的宏，不含任何声明。

程序自身的代码也可以一起转换：划分文件标记为改写的模块会将文件就地改写为模块单元，头文件变为分区（partition），包含指令变为导入声明。

**用法**：`clice modularize --partition <file> --out <dir> [--std <dir>] [--no-mirrors] [--scope <glob>...] [--workspace <dir>] [--configuration <tag>]`

所有信息都取自持久化索引，与 [`clice analyze modules interface`](./analyze.md#modules) 报告的内容一致：请先运行 `clice index`。该命令会写出文件并打印构建所需的信息，从不修改构建文件。

输出结果是一个 JSON 对象；若无法制定方案（划分文件无法解析或其中的划分被拒绝、索引缺失部分翻译单元、要写出的文件会覆盖已有文件），则输出 `{"error": "...", "stale": [...]}` 并以退出码 1 结束；若命令行参数无法解析、缺少 `--partition` 或 `--out`，或无法读取项目及其索引，则同样输出该对象并以退出码 2 结束。

## 划分

划分文件通过 glob 模式匹配相对于项目根目录的路径（根目录之外使用绝对路径），将文件归入各模块，以首个匹配为准。未被任何 glob 认领的文件属于程序自身，仍保留为头文件。

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

- 不带标志的模块会被封装：modularize 会生成其接口单元。
- `"external": true` 标记由现成接口充当的模块。仅支持 `std`：用 `--std` 指定 libc++ 模块源码目录（`share/libc++/v1`）后，它会作为 `std.compat` 导入，`std.cppm` 包含的标准头文件则会被置空。
- `"textual": true` 让模块继续保留为头文件，就像 C 标准库和编译器自带的头文件在 `import std` 之外仍保持头文件形式一样。程序原先仅通过已置空头文件间接访问到的那部分内容，会由该模块自身的单元 `<module>.cppm` 在这些头文件之上导出，宏则由 `macros.h` 重放。
- `"provides": "std.compat"` 指明导出某个文本模块中名称的模块，因此只有未被后者导出的内容才需要保留真实头文件。
- `"rewrite": true` 标记待改写为具名模块的程序模块，参见[改写程序](#rewriting-the-program)。`"primary"` 指定其主接口单元（primary interface unit）的位置；默认为包含该模块全部头文件的最深目录下的 `module.cppm`，若该模块没有头文件则取包含其全部文件的最深目录。

无论是每个库各建一个模块，还是所有库合为一个模块，都只需调整划分文件即可；分析范围必须涵盖划分文件所指明的全部文件。

## 改写程序

被改写的模块会成为一个具名模块，沿用划分中的模块名：

- 每个头文件变为同目录下的分区 `<stem>.cppm`，分区名取它在主接口所在目录下的相对路径（`src/support/format.h` 对应 `:support.format`，当主接口位于 `src/` 下时）。被其他模块的文件使用的头文件成为接口分区，由主接口重新导出；仅供本模块自身文件使用的头文件成为实现分区。
- 每个源文件变为实现单元 `module <name>;`；其中定义的 `main` 会移入 `extern "C++"`，以继续归属全局模块。
- 包含同一模块的头文件会改为导入对应分区，因此在模块内部，编辑触发的重新构建范围与原先包含头文件时相同；包含另一个被改写模块的头文件会改为导入那个模块。源文件通过每个实现单元隐式导入的主接口访问本模块的接口分区。
- 对已封装头文件的包含会被移除。主接口旁以其命名的分区 `<stem>_prelude.cppm`（如 `:module_prelude`，对应主接口 `module.cppm`）会为模块内的所有单元导入 `std.compat` 和各生成模块，而不暴露给模块的导入方：主接口重新导出该分区以带给各实现单元，其他每个分区也都会导入它。
- 模块单元只为仍需包含或移入其中的内容开启全局模块片段，即仍保留为头文件的头文件，以及对范围外实体的声明——它们可能引用生成模块所声明的名称，因此放在前导头文件之后；未开启全局模块片段但用到了库中宏的单元，会在模块声明之后包含 `macros.h`。仍保持为普通翻译单元而非模块单元的源文件，则在开头包含前导头文件。
- 独占一行的其他模块实体前置声明（如 `class Foo;`）会被移除，改为导入该实体所属的模块；头文件中的匿名命名空间则会拆解并入外层命名空间。
- 头文件为其他文件定义的宏会移至同目录下的 `<stem>.macros.h`（保留除包含指令外的预处理指令），由所有使用这些宏的文件包含。
- 若源文件定义了另一个被改写模块的头文件所声明的实体，该源文件会并入那个模块，因为定义必须归属声明所在的模块；未被改写的模块中若源文件包含了被改写的头文件，则会改为导入对应模块，自身仍保持为普通翻译单元。

改写哪些模块以及粒度多粗由划分文件决定：整个程序合为一个模块时，每个头文件都是分区，按文件逐个导入；每个目录各建一个模块时，每个目录都有独立接口，但修改任一接口分区都会重新构建该模块的所有导入方。改写前可用 [`clice analyze modules`](./analyze.md#modules) 权衡这两种方案。

## 合并构建配置

单次封装只涵盖头文件在一种构建配置下声明和定义的内容：当前平台的 C 标准库、库配置头文件设定的宏，以及库仅为单一目标平台声明的名称。面向多个平台或多种配置构建的程序，需基于各配置的索引分别向独立目录运行一次 modularize，再将结果合并。

**用法**：`clice modularize merge <file> --out <dir> [--no-mirrors]`

```json
{
  "configurations": [
    {
      "name": "linux-x64",
      "condition": "defined(__linux__) && defined(__x86_64__)",
      "out": "linux-x64"
    },
    {
      "name": "windows-x64",
      "condition": "defined(_WIN32) && defined(__x86_64__)",
      "out": "windows-x64"
    }
  ]
}
```

`out` 是对应配置运行时指定的 `--out` 目录，若非绝对路径则相对于合并文件解析。每个条件仅在所属配置的编译中成立。合并后的 `--out` 目录结构如下：

- 每个配置一个 `<name>/` 目录：存放该配置的前导头文件、`macros.h` 与各宏头文件，以及每个模块单元的全局模块片段 `<module>.fragment.h`。
- `<module>.cppm` 包含条件成立的配置所对应的片段，并导出所有配置共有的名称；仅部分配置导出的名称放在对应条件的 `#if` 块内。若没有任何条件成立，编译会在 `#error` 处报错终止。
- `prelude.h` 和 `macros.h` 会包含条件成立的配置下的同名文件。
- `mirror/` 存放任一配置置空的头文件；传入 `--no-mirrors` 时不生成该目录。

所有配置必须封装同一批模块；除非指定 `--no-mirrors`，否则在是否置空标准头文件上也必须一致，因为镜像目录对所有配置共同生效。合并操作打印的构建计划不含 `stdSources` 和 `includeRoots`，因为它们是各配置所在机器上的本地路径。

## 输出

`--out` 目录下的生成内容：

- 每个被封装模块对应一个 `<module>.cppm` 和一个按定义顺序收录导入方所需宏的 `<module>.macros.h`。保留为头文件的模块同样有这两个文件：其模块单元导出其他模块文件仅通过已置空头文件间接访问到的名称，连同其所包含头文件中的别名与 using 声明；其宏头文件和标准库的宏头文件一样标为系统头文件，与它重放的原头文件一致。
- `mirror/<module>/`：为其他模块文件包含的每个头文件按包含时所写的路径生成一个空文件；标准头文件对应 `mirror/std/`。
- `macros.h`：汇总所有宏头文件，不含其他内容，因此可在任意位置包含，甚至放在模块声明之前。
- `prelude.h`，供非模块单元的普通翻译单元使用：依次包含保留为头文件的模块中定义了内部链接实体的头文件、`import std.compat;`、所有模块导入语句，最后是 `macros.h`。

指定 `--no-mirrors` 时不会置空任何头文件：仍包含已封装头文件的文件——保留为头文件的头文件，或另一个封装模块的全局模块片段——会在导入模块之余重新解析该头文件。Clang 会合并这两份声明，但若一个模块的全局模块片段包含了它所导入模块的头文件则会冲突。将所有第三方库封装成单个模块时，只有程序中仍保留为头文件的头文件才会包含已封装的头文件。

内容未变的文件会保留原有时间戳。上次运行写出而本次不再生成的文件会被清理；`--out/.modularize` 记录了单次运行写出的文件清单，`--out` 下的其他文件不会改动。

被改写的文件会就地写回，被分区替代的原头文件会被删除。

标准输出会打印构建计划。`wrapping` 给出模块源文件、镜像目录、`macros.h` 和前导头文件（均为相对于 `--out` 的路径），以及实际找到的 libc++ 源码与各库的头文件搜索根目录；`rewriting` 给出相对于项目根目录的各改写模块单元：

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
    "macros": "macros.h",
    "prelude": "prelude.h",
    "warnings": []
  },
  "rewriting": {
    "modules": [
      {
        "name": "app.core",
        "primary": "src/core/module.cppm",
        "interfaces": ["src/core/module_prelude.cppm", "src/core/text.cppm"],
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

构建系统按顺序编译 `stdSources` 和各个模块，编译每个模块时带上对应库的头文件搜索根目录并将各自的 `mirrors` 置于头文件搜索路径最前，同时为改写未触及的每次程序编译加上 `mirrors` 和 `-include prelude.h`。对于被改写的模块，将其主接口、`interfaces` 和 `partitions` 作为模块接口单元编译，将 `sources` 作为实现单元编译，并将项目根目录加入头文件搜索路径以便找到 `macros.h` 和前导头文件；`moved` 列出并入其他模块的源文件，格式为 `path=module`。

## 示例

### 封装库

将库的头文件封装成供程序导入的模块，无需修改任何源文件。

<!-- BEGIN GENERATED ITEMS: modularize/libraries -->

<!-- BEGIN CAPABILITY: supported -->

**封装单个库**

库的头文件变为一个包含它们并导出其所声明名称的模块接口单元，程序原有的包含指令保持不变

程序仍然包含 `<alpha/alpha.h>`：由于 `wrap/mirror/alpha` 排在头文件搜索路径首位，读到的头文件为空；强制包含的 `wrap/prelude.h` 则会导入 `alpha`，并定义原头文件所定义的宏。

```case
tests/modularize/libraries/01_one_library
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 改写程序模块

程序中标记为改写的模块会变为具名模块，其文件就地改写，并保留原有的字节顺序标记（BOM）。

<!-- BEGIN GENERATED ITEMS: modularize/program -->

<!-- BEGIN CAPABILITY: supported -->

**改写程序模块**

程序中的一个目录变为具名模块：头文件变为分区，源文件变为实现单元，包含该头文件的文件改为导入该模块

原头文件被移除，由分区 `app.core:text` 取代，主接口 `app/core/module.cppm` 重新导出该分区。

```case
tests/modularize/program/01_one_module
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 已知限制

- 模块单元必须使用完整 BMI 构建（clang 下传入 `-fno-modules-reduced-bmi`）：精简 BMI 会丢弃全局模块片段中未被模块辖域（purview）按名引用的声明，包括偏特化。
- 含有被程序按名引用的内部链接实体的头文件必须保留为文本头文件，因为模块无法导出这类实体。引用它的被改写文件会直接包含它——若它属于保留为头文件的模块，则改用其他模块文件包含的入口头文件（例如用 `<stdio.h>` 代替内部的 `<bits/types/FILE.h>`），前导头文件包含这类头文件时也是如此；未被改写的文件若原先仅通过已封装库被置空的头文件间接引入该头文件，则会补充直接包含。
- 被其他文件以相对于自身目录的路径（如 `"../foo.h"`）包含的头文件无法置空；modularize 会对此发出警告。
- 若两个封装模块的头文件都用到了它们共同包含的某个头文件中的同一个 `static` 函数或变量（如 simdjson 和 CRoaring 都调用 `<emmintrin.h>` 的内建函数），两个模块会各自持有一份副本，而 clang 会为两者生成相同的修饰名（mangled name），导致同时使用两侧代码的导入方可能编译失败。modularize 会针对被其他文件引用的实体对此类模块对发出警告（仅被所在头文件自身代码调用的辅助函数在索引中属于该文件局部）；请将它们封装为同一个模块。
- 索引中每个翻译单元对同一头文件只保留一次解析结果：若某个翻译单元在不同宏下将同一头文件读取了两次（如 `<stddef.h>` 先在 `__need_size_t` 下读取，随后又完整读取），则该翻译单元包含它的所有位置都按完整读取计算。
- 不使用镜像目录时，若一个模块的全局模块片段包含了它所导入模块的头文件，会因 clang 无法合并的重复声明而报错；请将这类库封装为同一个模块。
- 程序的每次编译都会导入所有模块并重放所有宏头文件，无论它原先包含了哪些库。
- 标准库模块采用 libc++ 的实现。
- 只有索引收录的文件会被改写：当前已索引配置下没有任何编译涉及的源文件（如其他平台的源文件）会保留原有的包含指令。
- 尽管标准规定通过其他分区间接导入实现分区的单元同样导入了该分区，Clang 却不认为该实现分区的全局模块片段对该单元可达。因此被改写的文件还会自行显式导入这类分区。
- 位于预处理条件分支内的被改写头文件包含，会变为无条件的导入声明。
- 头文件变为分区后只编译一次：声明随包含方变化，或检测包含方所定义宏的头文件，只会保留一种解析结果。`clice analyze modules obstacles` 会列出此类需先行清理的头文件。
- 对分析范围外文件的包含保留在原位；若位于文件开头的预处理指令区之后，它会落入模块辖域，使其中声明的实体归属当前模块。
- 若保留为头文件的头文件包含了被改写的头文件，或要写出的文件会覆盖已有文件，modularize 会在写入任何内容前报错退出。
- 编译器能在改写后的代码树上直接报出的问题交由构建阶段发现：同一模块或两个被改写模块的头文件互相包含、变为分区的头文件中含有 `static` 实体，以及被改写的源文件定义了仍保留为头文件的头文件所声明的实体。
- 对分析范围外实体的前置声明会移入全局模块片段，并按其限定名包裹各层命名空间；其中的内联命名空间（例如由宏展开开启的内联命名空间）不会还原。
