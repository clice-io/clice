# Compilation

## Incremental Parsing

每当你修改代码时，clice 都必须重新解析文件。clice 使用一种叫做 preamble 的机制实现增量编译以加快重新解析速度。preamble 可被视为 [Precompiled Header](https://clang.llvm.org/docs/PCHInternals.html) 的一种特殊情况（内嵌在源文件中）。在打开文件的时候，它会将文件开头的几个预处理指令（被叫做 preamble）构建成 PCH 缓存在磁盘上，后续在解析的时候则可以直接加载 PCH 文件，从而跳过前面几个预处理指令，这样可以大大减少要重新解析的代码量。

例如，对于如下的代码

```cpp
#include <iostream>

int main () {
    std::cout << "Hello world!" << std::endl;
}
```

`iostream` 这个头文件大概有 2w 行代码，clice 会先把 `#include <iostream>` 这一行代码构建成 PCH 文件，在完成之后在使用这个 PCH 文件来解析后面的代码。这样的话后续重新解析的代码量就只剩 5 行了，而不是原本的 2w 行，速度会变得非常快。除非你修改了 preamble 部分的代码，导致需要构建新的 preamble。

## Cancel Compilation
