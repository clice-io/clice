# 什么是 clice？

简而言之，clice 是一个使用**现代 C++** 编写而成的 C/C++ 语言服务器，但**不只是**语言服务器。它仍然基于 LLVM/Clang，但与同样基于 Clang 的 clangd/ccls 不同，进行了彻底的 redesign，旨在彻底解决这些语言服务器中由于架构问题而数年无法解决的问题。它完全使用 C++23 编写，并且本身就是现代 C++ 的最佳实践，所有主要的现代 C++ 特性以一种奇妙的方式组合在一起，这体现在：

- 完全使用 C++20 [coroutine](https://github.com/clice-io/kotatsu/tree/main/include/kotatsu/async) 编写，使用我们基于 libuv 的结构化并发异步库编写复杂异步逻辑。它是在主线程上运行事件循环，多进程（IPC）/多线程均有涉及的复杂并发程序。
- 完全使用 C++20 named module 和 C++23 `import std` 编写，并且完成了自举，三方库的模块包装是用 `clice modularize` 工具自动生成的，并且 clice 本身可以良好地运行在自身的代码库上，提供模块相关的语言服务。
- 完全使用基于 template 和 C++20 concept 的 [static reflection](https://github.com/clice-io/kotatsu/tree/main/include/kotatsu/codec) 技巧进行多种格式的序列化和反序列化。这包括 toml/json/bincode/blob/cli，支持 Rust macro like 式的注解式声明，无需编写任何样板代码。
- 完全面向数据的代码设计，除了实现 Clang 要求的回调接口，几乎不用虚函数。大量使用 `std::format`、`std::expected`、`std::ranges`、`std::views` 等现代 C++ API，不使用 legacy 的 C++98/11 的过时 API。
- 完全封闭可复现的构建流程，使用 Bazel 进行封闭构建。采用统一的 [xclang](https://github.com/clice-io/xclang) 工具链进行编译，提供统一的跨平台交叉编译体验。编译产物经过高度优化（ThinLTO），构建产物除了 system library 无任何依赖。

> 一直有关于 module 等现代 C++ 特性是否可以使用的讨论。现在 clice 告诉你，是的，当然，完全可用！它们可以用于编写 clice 这样拥有数十万行代码的复杂软件，这或许就是我们理想中现代 C++ 的模样。但是另一方面，现有的生态确实如此不方便，以至于为了走通这套流程，我花费了大量的精力来跟进适配它们。

## 语言服务器

接下来是 clice 的主要功能介绍：

- **多进程编译**：clice 采用类似现代浏览器设计的进程池来进行编译，clangd 则采用多线程。为什么采用多进程？这是因为 C++ 实在**太过复杂**，对于某些 [corner case](https://github.com/clice-io/clice/issues/1057) 编译器很容易崩溃或者产生内存泄漏。尤其是对于正在编辑的**不完整**代码，这里有广泛的下游用户验证编译器在完整代码上的正确性，但是验证不完整代码的人则非常少，编译器往往采用启发式的猜测尽力而为。对于 clangd，如果编译崩溃，则整个服务器都必须要重启。对于 clice，编译发生在隔离的子进程，一个 worker 崩溃并不影响其他服务，跳转高亮之类的仍然可以处理。并且也可以更好地管理内存使用和编译优先级调度，降低不必要的资源消耗。
- **模块和共享缓存**：clice 具有完整的 C++20 named module 支持。只需提供一份含有所有模块文件的 `compile_commands.json` 文件，clice 便会自己进行依赖扫描，然后按需 lazy 构建所需要的模块，无论你使用的是什么构建系统或者编译器。并且提供完整的模块补全、高亮、跳转、悬浮等功能支持。同时会将构建的 PCH 和 PCM 缓存在本地磁盘上，供不同会话之间复用。相比之下 clangd 只提供了十分有限的模块功能支持，并且不支持持久化缓存。
- **编译上下文**：clice 完全支持非自包含（non-self-contained）的文件，更一般地说，支持编译上下文（compilation context）这个概念。你可以显式选择这个文件的工作上下文，对于源文件来说是不同的编译命令，对于头文件来说则是被包含在不同的位置。并且在你打开非自包含文件的时候会做自动探测，从而提供开箱即用的非自包含文件使用体验。也支持更通用的编译分组概念，允许你为不同的编译配置进行分组，比如说可以在 debug 和 release 之间切换编译命令。
- **更好的模板支持**：clice 实现了一个 DFS 伪模板实例化器（pseudo instantiator），不需要具体的模板参数进行确定的实例化，就推断出很多依赖名的指向。如果模板被实例化过一次，还会额外使用这次实例化的结果来提供语言服务，例如 `ranges` 调用用 lambda 的 `auto` 参数补全。另外，clice 把模板看成一个鸭子类型（duck type）的接口，每一次实例化都是它的一个实现，就像虚函数和它的 override 一样：在模板里的依赖调用上跳转到实现，会列出它在每次实例化里实际调用的函数，查找引用和调用层级也是如此。
- **开箱即用的使用体验**：绝大多数项目只需要一份 `compile_commands.json` 即可使用，clice 内置了 50+ 工具链的环境探测，涵盖 nvcc、msys2、cygwin、zigcc 等等。会自动处理好实际使用的编译器和 Clang 不一致的问题。无需像 clangd 那样在配置文件中手动添加大量的编译选项。

除了这些主要的特性，clice 还解决了 clangd 长期存在并且无人问津的很多小 issue，涵盖语义高亮、折叠、悬浮等功能，感兴趣的读者欢迎立即使用，看看能收获哪些惊喜。总的来说，clice 在易用性、功能支持和性能上均超过 clangd。

## 为什么不改进 clangd？

首先为什么要编写一个新的项目而不是改进 clangd？这是最常被问到的问题。这里有多方面的原因，在我最早期的[文章](https://zhuanlan.zhihu.com/p/13394352064)中有一些阐述：

- **关于投入问题**：最根本的问题是 clangd 从 2024 年前后开始基本处于**实质上无人维护**的状态。这很令人惊讶，数十万开发者使用的工具怎么会无人维护呢？2024 年我准备给 clangd 提模板内代码补全改进的 [PR](https://github.com/clangd/clangd/issues/443#issuecomment-2254535078) 的时候意外发现了这个事实。据我了解，这与 Google 对 C++ 的策略转向有关，在这之前 clangd 主要由 Google 的数名员工维护，而在 2024 年前后，Google 对 C++（Clang）的投入开始减少，clangd 也被认为已经满足了 Google 内部的代码需求，从而原有的数名员工都被调离去做别的工作了。在那之后 clangd 缺乏实质上的长期维护者，只有 [Nathan Ridge](https://github.com/HighCommander4) 完全出于热爱（无任何薪酬）在业余时间解答问题，但是一个人的精力有限，几乎无法处理大型 PR。这也就是 clangd 的模块化 [PR](https://github.com/llvm/llvm-project/pull/66462) 拖了将近一年的原因，并且时至今日仍然无法正式地在大型项目使用，存在诸多缺陷，无人处理。2026 年，Clang 的 lead maintainer Aaron Ballman 意识到了这个问题，在社区的论坛上发了一个[求助帖](https://discourse.llvm.org/t/help-needed-with-clangd-maintenance/89820)，说 clangd 已经非常接近无人维护，积压了大量的 PR 和 issue，希望能找到新的社区维护者，否则 clangd 最终有**可能**会被从 LLVM 项目里移除。
- **关于代码规模**：正如上文所说的，clice 做了大量的架构创新，比如多进程编译。如果要在 clangd 里面实现这些功能，那接近于**重写** clangd。这样大规模的改动不可能有人 review 并接受。并且想要实现模块化自举，LLVM 的构建流程也会有巨大的修改。
- **关于 LLVM 仓库的限制**：clice 依赖大量的三方库来获取更好的性能和用户体验，比如 `libuv`、`simdjson` 和 `tomlplusplus` 等，LLVM 出于构建便捷的考虑，要求自包含，不可能引入这么多的三方库。要么将它们重写移植到 LLVM 中，要么忍受功能上的不便利和性能的低下。此外，语言服务器并不是一个**独立**的项目，它还包含各种编辑器的插件适配部分。为了方便维护，当然和语言服务器放在一个仓库比较好，而 LLVM 同样不太会接受这些代码（包含 Rust、Lua、TS 等语言）。所以从长期维护便捷性和我们的核心目标来说，独立的仓库势在必行。

于是，在 2024 年中，我开始研究如何设计和编写一个新的语言服务器，并在 2024 年底发布了[一个新 C++ language server 的设计与实现](https://zhuanlan.zhihu.com/p/13394352064)，确立了 clice 开发的核心目标，并正式开始开发 clice。
