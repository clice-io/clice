# 编译

clice 使用编译数据库中对应的命令编译每个打开的文件，与实际构建一致。以下各节介绍这一过程在真实项目中涉及的各个方面：编译命令传入的选项、C 和 C++ 以外的语言、文件编码、磁盘上的文件变更，以及重启后依然保留的缓存。

<!-- The cards in the sections below are generated from the documented cases
     in tests/compile/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 编译选项

文件编译命令中会改变编译内容的选项（如 `-include`），在编辑器中的效果与构建时一致。

<!-- BEGIN GENERATED ITEMS: compile/flags -->

<!-- BEGIN CAPABILITY: supported -->

**强制包含**

命令通过 `-include` 强制包含的头文件与文件自身包含的头文件行为一致

构建系统常通过强制包含头文件来引入配置宏和预编译头文件。其中的宏支持悬停和代码导航，修改头文件后，所有强制包含它的文件都会随之更新。

```case
tests/compile/flags/01_forced_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预编译头文件**

启用预编译头文件的目标下各翻译单元共用同一份 PCH，由 clice 按构建命令生成

对于这类目标，CMake 会让其中每个翻译单元使用它从 `cmake_pch.hxx.cxx` 构建出的预编译头文件。磁盘上的预编译文件可能出自其他编译器，因此 clice 会按同一条命令自行构建一份（只构建一次，保留全部函数体），供打开的文件和索引共同使用：这次构建本身就是对 `cmake_pch.hxx.cxx` 的编译，同时也会为它建立索引。编辑该目标的任一头文件都会重建这份 PCH，并更新到每个翻译单元。

```case
tests/compile/flags/07_precompiled_headers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**延迟解析模板体**

命令要求 clang 延迟解析模板体时，每次编译都会延迟解析

clice 默认在模板体定义处直接解析它，即使在 MSVC 目标（clang 在 C++20 之前默认延迟解析）上也是如此，以便悬停、代码导航和诊断在每个模板内部都能正常工作。依赖 MSVC 宽松名字查找编写的代码（例如模板引用了在其后才声明的名称）此时会报出符合标准的编译器所报的错误。若编译命令传入了 `-fdelayed-template-parsing`（clang-cl 下为 `/Zc:twoPhase-`），无论来自构建本身还是由规则追加，clice 都会像原编译器一样编译这类代码。代价是：未被任何代码实例化的模板，其函数体始终不会解析，内部内容无法悬停或查找，但代码补全在其中依然可用。

```case
tests/compile/flags/19_deferred_template_bodies
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**分层检查（layering check）**

构建的分层检查所拒绝的包含指令不显示错误

Bazel 的 `layering_check`，以及所有让 Clang 检查模块映射（module map）所声明的使用关系（`-fmodules-decluse`）的构建，都会拒绝包含目标的依赖均未提供的头文件。clice 编译文件时视同没有任何模块映射列出它：这类包含指令在编辑器中编译不会报错，只有构建时才会报告。

```case
tests/compile/flags/30_layering_check
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他语言

构建过程中编译的其他语言文件（如汇编源文件）不会处理：clice 只分析 C 族代码。

<!-- BEGIN GENERATED ITEMS: compile/languages -->

<!-- BEGIN CAPABILITY: supported -->

**汇编源文件**

构建中编译的汇编源文件无论是否经过预处理，均交由汇编器处理：索引、Lint 和编辑器都会跳过它们

```case
tests/compile/languages/01_assembler
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文件编码

无论文件是否带有字节顺序标记（byte order mark），也无论使用何种换行符，功能表现都完全一致。

<!-- BEGIN GENERATED ITEMS: compile/encoding -->

<!-- BEGIN CAPABILITY: supported -->

**字节顺序标记**

保存为带字节顺序标记的 UTF-8 文件可与其他文件一样正常使用

位置计算会跳过该标记，与编辑器显示文件的方式一致；编译则按编译器的方式读取文件，因此文件内嵌入的数据保留原始字节。

```case
tests/compile/encoding/01_byte_order_mark
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Windows 换行符**

使用 CRLF 换行符的文件与其他文件一样在相同位置返回响应

```case
tests/compile/encoding/02_line_endings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**其他编码**

以非 UTF-8 编码保存的文件按 UTF-8 读取，clice 会点名提示一次

这类文件中超出 ASCII 范围的字节不是合法 UTF-8，编辑器会将每个这样的字节显示为替换字符。clice 加载项目时会把发现的非 UTF-8 文件列出一次；将它们另存为 UTF-8 即可修复。

```case
tests/compile/encoding/03_legacy_bytes
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 磁盘上的文件变更

其他工具改写头文件后，无需在编辑器中打开该头文件，变更即可反映到包含它的文件中。

<!-- BEGIN GENERATED ITEMS: compile/staleness -->

<!-- BEGIN CAPABILITY: supported -->

**编辑器之外的更改**

磁盘上发生更改的头文件会在包含它的文件下次收到请求时生效，无论后者是否已保存

分支切换、代码生成器和其他工具常在编辑器之外改写头文件。下次针对包含该头文件的文件发起请求时，clice 会按新内容编译该文件，无论该头文件位于包含链的哪一层；若重写后的字节内容未变，则不产生任何开销。

```case
tests/compile/staleness/01_header_edits
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 缓存

clice 为头文件和模块编译出的产物会与索引一同保存在磁盘上，使服务器重启后直接热启动。

<!-- BEGIN GENERATED ITEMS: compile/cache -->

<!-- BEGIN CAPABILITY: supported -->

**热重启**

预编译头文件在重启后依然保留，服务器停止期间发生的修改仍会反映到使用这些头文件的文件中

```case
tests/compile/cache/01_headers_restart
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
