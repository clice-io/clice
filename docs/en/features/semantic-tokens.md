# Semantic Tokens

## Lexical Tokens

Emitted from a lexical pass over the source file, independent of the AST.

- [x] Comments
- [x] Numbers, characters, strings
- [x] Keywords (including `override`, `final`)
- [x] Preprocessor directives (`#if`, `#define`, etc.)
- [x] Header names in `#include`
- [x] Macro names at `#define` site
- [ ] Literal prefixes and suffixes — highlight encoding prefixes and type suffixes as distinct tokens

  ```cpp
  R"(raw string)"       // R prefix
  u8"utf-8 string"      // u8 prefix
  L"wide string"        // L prefix
  0xFF                   // 0x prefix
  0b1010                 // 0b prefix
  42u                    // u suffix
  3.14f                  // f suffix
  1'000'000              // digit separators
  ```

- [ ] User-defined literal suffixes — highlight the suffix as a separate token linked to the literal operator

  ```cpp
  using namespace std::chrono_literals;
  auto d = 500ms;        // ms suffix → operator""ms
  auto s = "hello"s;     // s suffix → operator""s
  auto x = 3.14_deg;     // _deg suffix → operator""_deg
  ```

- [ ] Escape sequences within string/character literals

  ```cpp
  "hello\nworld"         // \n highlighted differently from surrounding text
  '\x41'                 // \x41 highlighted as escape
  ```

- [ ] Declarator vs operator disambiguation for `&`, `*`, `&&` — distinguish pointer/reference declarators from arithmetic/logical operators ([clangd#1421](https://github.com/clangd/clangd/issues/1421))

  ```cpp
  int* p;          // * is a declarator (pointer type)
  int x = a * b;   // * is an arithmetic operator
  int& r = x;      // & is a declarator (reference type)
  int y = a & b;   // & is a bitwise operator
  ```

## Token Types

### Declarations & References

- [x] Namespace and namespace alias
- [x] Class, struct, union, enum
- [x] Type alias (`typedef`, `using`)
- [x] Function, method, constructor, destructor, operator
- [x] Variable (local, global, parameter, field, binding)
- [x] Enum member
- [x] Template parameter (type and non-type)
- [x] Concept
- [x] Label (`goto` target)
- [ ] Structured binding variables — should highlight as local variables, not global ([clangd#485](https://github.com/clangd/clangd/issues/485))

  ```cpp
  auto [x, y] = point;  // x, y should be highlighted as local variables
  ```

- [ ] Deduction guides

  ```cpp
  template<typename T>
  vector(T, T) -> vector<T>;  // deduction guide should get a token
  ```

- [ ] `using enum` — the enum name should get a semantic token ([clangd#1283](https://github.com/clangd/clangd/issues/1283))

  ```cpp
  using enum Color;  // Color should be highlighted as enum type
  ```

- [ ] Explicit instantiation ([clangd#316](https://github.com/clangd/clangd/issues/316))

  ```cpp
  template class vector<int>;  // vector and int should be highlighted
  ```

- [ ] Member initializer list fields ([clangd#122](https://github.com/clangd/clangd/issues/122))

  ```cpp
  struct Widget {
      int width, height;
      Widget(int w, int h) : width(w), height(h) {}
      //                      ^^^^^    ^^^^^^ should be highlighted as fields
  };
  ```

- [ ] Lambda init-capture — should consistently get a token ([clangd#868](https://github.com/clangd/clangd/issues/868))

  ```cpp
  auto f = [val = compute()] {};  // val should be highlighted as local variable
  ```

- [ ] Using declarations — consistent token for the introduced name ([clangd#2619](https://github.com/clangd/clangd/issues/2619))

  ```cpp
  using std::vector;  // vector should get a semantic token
  ```

- [ ] `sizeof...` — the pack parameter should get a token ([clangd#213](https://github.com/clangd/clangd/issues/213))

  ```cpp
  template<typename... Ts>
  constexpr auto count = sizeof...(Ts);  // Ts should be highlighted as templateParameter
  ```

### Module Tokens

- [x] `import` / `export` / `module` keywords highlighted distinctly
- [x] Module names in import declarations
- [ ] Full AST-level token resolution for module declarations (currently lexical fallback)

### Attributes

- [ ] Attribute names should receive semantic tokens

  ```cpp
  [[nodiscard]] int compute();
  [[deprecated("use v2")]] void old_func();
  [[maybe_unused]] int x;
  ```

- [ ] Vendor-specific attribute namespaces

  ```cpp
  [[gnu::packed]] struct S {};     // gnu namespace
  [[clang::optnone]] void f();    // clang namespace
  ```

- [ ] Expressions inside attributes should be fully highlighted ([clangd#2209](https://github.com/clangd/clangd/issues/2209))

  ```cpp
  [[assume(ptr != nullptr)]];  // ptr, nullptr should get tokens
  ```

## Token Modifiers

### Implemented

- [x] Declaration vs definition distinction
- [x] Static (class static members, static local variables, static functions)
- [x] Readonly / const
- [x] Deprecated (`[[deprecated]]`)
- [x] Abstract (pure virtual methods)
- [x] Virtual
- [x] Default library (symbols from system headers)
- [x] Constructor / destructor marker
- [x] Templated

### Planned

- [ ] Scope modifiers — function scope, class scope, file scope, global scope ([clangd#352](https://github.com/clangd/clangd/issues/352))

  ```cpp
  int global;                    // globalScope
  static int file_local;         // fileScope
  struct Foo {
      int member;                // classScope
      void bar() {
          int local;             // functionScope
      }
  };
  ```

- [ ] Mutable reference / mutable pointer — mark arguments passed by non-const reference or pointer ([clangd#839](https://github.com/clangd/clangd/issues/839))

  ```cpp
  void modify(int& out);
  modify(x);  // x should have usedAsMutableReference modifier
  ```

- [ ] User-defined operator modifier — distinguish built-in vs user-defined operators ([clangd#1521](https://github.com/clangd/clangd/issues/1521))

  ```cpp
  Vec a, b;
  auto c = a + b;   // + should have userDefined modifier
  int x = 1 + 2;    // + is built-in, no modifier
  ```

- [ ] Object-like vs function-like macro distinction ([clangd#2649](https://github.com/clangd/clangd/issues/2649))

  ```cpp
  #define MAX_SIZE 1024          // object-like → Constant kind
  #define CHECK(x) assert(x)    // function-like → Function kind
  ```

- [ ] Context-dependent readonly — `const` on the value vs on the pointer level ([clangd#1585](https://github.com/clangd/clangd/issues/1585))

  ```cpp
  const int* p;        // p itself is NOT readonly (pointer can change)
  int* const q;        // q IS readonly (pointer is const)
  const int* const r;  // r IS readonly
  ```

## Conflict / Ambiguity

C++ allows structurally different entities to share the same name. When a single name refers to multiple entities of different kinds, the semantic token type cannot be unambiguously determined. These cases are marked with a special **conflict** modifier and displayed in a neutral color (e.g., gray).

- [ ] Overloaded names via `using` — a name may introduce both a type and a function

  ```cpp
  namespace N {
      struct Widget {};
      void Widget();     // legal in C++: function shares name with struct
  }
  using N::Widget;       // brings both → conflict: type or function?
  ```

- [ ] Dependent name ambiguity across instantiations — a dependent call may resolve to different kinds in different instantiations

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo();  // foo could be a method or a functor data member
  }

  struct A { void foo(); };          // A::foo is a method
  struct B { std::function<void()> foo; };  // B::foo is a variable

  process(a);  // T = A → foo is a method (function color)
  process(b);  // T = B → foo is a variable (variable color)
  // conflict: no single correct token type
  ```

- [ ] Injected class name — inside a class, the class name can refer to both the type and the constructor

  ```cpp
  struct Widget {
      Widget(int);
      Widget create() {
          return Widget(42);  // Widget here: type? constructor?
      }
  };
  ```

## Dependent Name Highlighting

Highlighting of names in uninstantiated template bodies that depend on template parameters.

- [ ] Dependent type names ([clangd#154](https://github.com/clangd/clangd/issues/154), [clangd#214](https://github.com/clangd/clangd/issues/214))

  ```cpp
  template<typename T>
  void process() {
      typename T::value_type val;  // value_type should be highlighted as type
      T::static_func();            // static_func should be highlighted
  }
  ```

- [ ] Dependent template names ([clangd#484](https://github.com/clangd/clangd/issues/484))

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.template get<int>();  // get should be highlighted as method
  }
  ```

- [ ] Heuristic-based coloring — when a dependent name has a likely target, use that target's kind ([clangd#297](https://github.com/clangd/clangd/issues/297))

  ```cpp
  template<typename T>
  void process(std::vector<T>& v) {
      v.push_back(val);  // push_back could be colored as method (vector's known member)
  }
  ```

- [ ] Members imported from dependent base via `using` ([clangd#686](https://github.com/clangd/clangd/issues/686))

  ```cpp
  template<typename T>
  struct Derived : Base<T> {
      using Base<T>::member;
      void foo() { member(); }  // member should get a token
  };
  ```

- [ ] Dependent names with overloaded targets — when the name resolves to an overload set of mixed kinds ([clangd#1057](https://github.com/clangd/clangd/issues/1057))

## Inactive Code Regions

- [ ] Dim inactive preprocessor branches (`#if 0`, false `#ifdef`, etc.) ([clangd#132](https://github.com/clangd/clangd/issues/132))

  ```cpp
  #ifdef _WIN32
      // active code (normal highlighting)
  #else
      // inactive code (dimmed / grayed out)
  #endif
  ```

- [ ] Correct inactive region boundaries with `#elif` chains ([clangd#602](https://github.com/clangd/clangd/issues/602))

  ```cpp
  #if defined(A)
      // ...
  #elif defined(B)
      // the #elif directive line itself should NOT be dimmed
  #else
      // ...
  #endif
  ```

- [ ] Preserve syntax highlighting within inactive regions ([clangd#1664](https://github.com/clangd/clangd/issues/1664))

  ```cpp
  #if 0
      int x = 42;     // should still have keyword/type coloring, just dimmed
      std::string s;   // not plain gray text
  #endif
  ```

- [ ] Don't treat inactive regions as comments — they should remain distinct from actual comment blocks ([clangd#1545](https://github.com/clangd/clangd/issues/1545))

- [ ] Unreachable code dimming — code after unconditional `return`, `throw`, infinite loops, etc. ([clangd#1828](https://github.com/clangd/clangd/issues/1828))

  ```cpp
  void foo() {
      return;
      int x = 42;  // unreachable — should be dimmed
  }
  ```

## Format String Highlighting

- [ ] `std::format` / `std::print` format string placeholders ([clangd#1709](https://github.com/clangd/clangd/issues/1709))

  ```cpp
  std::format("Hello, {}! You are {} years old.", name, age);
  //           ^^^^^^  ^^          ^^
  //           string  placeholder  placeholder (different color)
  ```

- [ ] Highlight invalid format specifiers as errors

## Protocol Support

- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [x] UTF-16 delta-encoded token positions
- [ ] Range-based semantic tokens (`textDocument/semanticTokens/range`) — only compute tokens for the visible viewport, critical for performance on large files
- [ ] Delta updates (`textDocument/semanticTokens/full/delta`) — send only changed tokens since the previous response, reducing payload size on incremental edits

## Token Correctness

Known issues from clangd that clice aims to get right from the start:

- [x] Constructors and destructors use the `method` token type, not `class` ([clangd#1509](https://github.com/clangd/clangd/issues/1509))
- [x] Destructor token covers the leading `~` character ([clangd#2078](https://github.com/clangd/clangd/issues/2078))
- [x] Destructor token modifiers match the declaration ([clangd#872](https://github.com/clangd/clangd/issues/872))
- [ ] Apply token modifiers to operands of overloaded operators ([clangd#2547](https://github.com/clangd/clangd/issues/2547))

  ```cpp
  void process(Vec& out);
  out += rhs;  // out should have usedAsMutableReference modifier (via operator+=)
  ```

- [ ] Don't mark `auto` parameter in a lambda/function as `typeParameter` ([clangd#1390](https://github.com/clangd/clangd/issues/1390))

  ```cpp
  auto f = [](auto x) {};  // auto here is NOT a template type parameter
  void g(auto y) {}         // same — abbreviated function template, not typeParameter
  ```

- [ ] Nested name specifier in pointer-to-member should get a token ([clangd#2235](https://github.com/clangd/clangd/issues/2235))

  ```cpp
  int Foo::*ptr;  // Foo should be highlighted as class
  ```

- [ ] `::new` — the `new` keyword should get a semantic token even with global scope prefix ([clangd#1627](https://github.com/clangd/clangd/issues/1627))

- [ ] `co_yield` / `co_await` should not lose highlighting when the coroutine return type is a template ([clangd#2437](https://github.com/clangd/clangd/issues/2437))

  ```cpp
  template<typename T>
  generator<T> gen(T val) {
      co_yield val;  // co_yield should still be highlighted as keyword
  }
  ```

## Changelog

| Date | Change | PR |
| ---- | ------ | -- |
| —    | Initial semantic token types and modifiers, full document tokens | — |
