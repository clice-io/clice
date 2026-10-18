# 编译

clice 使用编译数据库中对应的命令编译每个打开的文件，与构建时的做法相同。以下各节介绍这一过程在实际项目中涉及的事项：命令传入的编译选项、C 和 C++ 以外的语言、文件编码、磁盘上的文件变化，以及重启后仍然保留的缓存。

<!-- The cards in the sections below are generated from the documented cases
     in tests/compile/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 编译选项

文件编译命令中会改变编译内容的选项（例如 `-include`），在编辑器中的作用与构建时相同。

<!-- BEGIN GENERATED ITEMS: compile/flags -->

<!-- BEGIN CAPABILITY: supported -->

**强制包含**

命令通过 `-include` 强制包含的头文件，与文件自身包含的头文件作用相同

构建系统会通过强制包含头文件来提供配置宏、使用预编译头文件。这些头文件中的宏支持悬停和代码导航，修改头文件后，所有强制包含它们的文件都会反映相应更改。

```case
tests/compile/flags/01_forced_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**预编译头文件**

启用预编译头文件的构建目标中，各翻译单元共享同一个 PCH，由 clice 按构建中的命令构建

对于这类目标，CMake 会让其每个翻译单元使用它由 `cmake_pch.hxx.cxx` 构建出的预编译头文件。磁盘上的这个文件可能出自其他编译器，因此 clice 会用同一条命令自行构建一份：目标的头文件只需解析一次，所有打开的文件和索引都共用这次解析；修改其中某个头文件会重建该 PCH，并反映到每个翻译单元中。

```case
tests/compile/flags/07_precompiled_headers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 其他语言

clice 不处理构建过程中编译的其他语言文件（例如汇编源文件）：它只分析 C 族代码。

<!-- BEGIN GENERATED ITEMS: compile/languages -->

<!-- BEGIN CAPABILITY: supported -->

**汇编源文件**

构建过程中编译的汇编源文件，无论是否经过预处理，都留给汇编器处理：索引、Lint 和编辑器都会跳过它们

```case
tests/compile/languages/01_assembler
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 文件编码

文件都能正常使用，不受字节顺序标记（byte order mark）或换行符的影响。

<!-- BEGIN GENERATED ITEMS: compile/encoding -->

<!-- BEGIN CAPABILITY: supported -->

**字节顺序标记**

以带字节顺序标记的 UTF-8 格式保存的文件，与其他文件一样可正常使用

首行的位置计算会跳过字节顺序标记，与编辑器中的显示一致；文件中嵌入的数据则保留原始字节。

```case
tests/compile/encoding/01_byte_order_mark
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Windows 换行符**

使用 CRLF 换行符的文件返回的响应位置与其他文件一致

```case
tests/compile/encoding/02_line_endings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 磁盘上的文件变化

其他工具改写头文件后，无需在编辑器中打开该头文件，更改就能反映到包含它的文件中。

<!-- BEGIN GENERATED ITEMS: compile/staleness -->

<!-- BEGIN CAPABILITY: supported -->

**编辑器之外的更改**

无论包含头文件的文件是否已保存，下次针对这些文件发起请求时，都会反映该头文件在磁盘上的更改

检出操作、生成器和其他工具会在编辑器之外改写头文件。下次针对包含该头文件的文件发起请求时，clice 会使用头文件的新内容编译该文件，无论头文件位于包含链中的哪个位置；如果改写前后的字节相同，则不会产生额外开销。

```case
tests/compile/staleness/01_header_edits
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 缓存

clice 会将头文件的编译结果保存在磁盘上，因此重启后可以直接复用缓存。

<!-- BEGIN GENERATED ITEMS: compile/cache -->

<!-- BEGIN CAPABILITY: supported -->

**热重启**

预编译头文件在重启后仍会保留，服务器停止运行期间所做的修改也会反映到使用这些头文件的文件中

```case
tests/compile/cache/01_headers_restart
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
