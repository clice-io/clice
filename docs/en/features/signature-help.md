# Signature Help

## Trigger Characters

Registered: `(`, `)`, `{`, `}`, `<`, `>`, `,`

| Character | Context            | Behavior                 |
| --------- | ------------------ | ------------------------ |
| `(`       | Function call      | Show overload signatures |
| `)`       | Close paren        | Update context           |
| `{`       | Brace init         | Show overload signatures |
| `}`       | Close brace        | Update context           |
| `<`       | Template args      | Show overload signatures |
| `>`       | Template close     | Update context           |
| `,`       | Argument separator | Update active parameter  |

- [ ] Avoid false triggers — don't fire inside comments, string literals, or when defining a function ([clangd#51](https://github.com/clangd/clangd/issues/51), [clangd#289](https://github.com/clangd/clangd/issues/289))

  ```cpp
  void foo(int x, int y) {  // should NOT trigger signature help
  //       ^^^^^^^^^^^^^ this is a definition, not a call
  ```

- [ ] `new` expression with braces should trigger signature help ([clangd#1967](https://github.com/clangd/clangd/issues/1967))

  ```cpp
  auto* w = new Widget{800, 600};
  //                   ^ should trigger signature help for Widget constructors
  ```

## Overload Signatures

<!-- BEGIN GENERATED ITEMS: Overload Signatures -->

- [x] Function overloads — every overload of the callee, each with its parameter list and return type

  <details>
  <summary>Example</summary>

  ```cpp
  void foo();
  void foo(int x);
  void foo(int x, int y);

  int main() {
      foo();
  }
  ```

  </details>

- [x] Active parameter tracking — the parameter under the cursor is bracketed; the point sits in the second argument

  <details>
  <summary>Example</summary>

  ```cpp
  void bar(int first, double second, char third);

  int main() {
      bar(1, 2.0, 'c');
  }
  ```

  </details>

- [x] Member function overloads — a non-const receiver lists both the const and non-const overloads; the trailing const qualifier is not rendered in the label

  <details>
  <summary>Example</summary>

  ```cpp
  struct Buffer {
      int at(int index);
      int at(int index) const;
  };

  int main() {
      Buffer b;
      b.at(0);
  }
  ```

  </details>

- [x] Default arguments in the label — parameters with defaults render their initializer in the signature

  <details>
  <summary>Example</summary>

  ```cpp
  void configure(int width, int height = 100, bool visible = true);

  int main() {
      configure(1);
  }
  ```

  </details>

- [x] C-style variadic function — named parameters are listed while the trailing ellipsis is elided from the label

  <details>
  <summary>Example</summary>

  ```cpp
  void record(int code, ...);

  int main() {
      record(0);
  }
  ```

  </details>

- [x] Variadic template pack — the parameter pack renders as the callee's uninstantiated signature

  <details>
  <summary>Example</summary>

  ```cpp
  template <typename... Args>
  void emit(Args... args);

  int main() {
      emit();
  }
  ```

  </details>

- [x] Active parameter past a shorter overload — with the cursor in the second argument, only overloads that declare a second parameter remain

  <details>
  <summary>Example</summary>

  ```cpp
  void draw();
  void draw(int x);
  void draw(int x, int y);

  int main() {
      draw(1, 2);
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [x] Template instantiation pattern resolution (shows template pattern, not instantiation)
- [ ] Filter const/non-const overload duplicates — don't show both when only one is viable ([clangd#50](https://github.com/clangd/clangd/issues/50))

  ```cpp
  struct Vec {
      int& operator[](size_t);
      const int& operator[](size_t) const;
  };
  Vec v;
  v[0];  // only show non-const overload (v is non-const)
  ```

- [ ] Prefer user-supplied constructors over compiler-generated ones ([clangd#1259](https://github.com/clangd/clangd/issues/1259))

- [ ] Filter dependent overload candidates by arity ([clangd#2342](https://github.com/clangd/clangd/issues/2342))

  ```cpp
  template<typename T>
  void process(T& obj) {
      obj.foo(1, 2);  // if T has foo(int) and foo(int,int), only show foo(int,int) as viable
  }
  ```

- [ ] Better heuristic resolution of dependent overloads ([clangd#1083](https://github.com/clangd/clangd/issues/1083))

- [ ] Strip C++23 explicit object parameter from displayed signatures ([clangd#2284](https://github.com/clangd/clangd/issues/2284))

  ```cpp
  struct S { void f(this S& self, int x); };
  S s;
  s.f(^  // show "(int x)", not "(this S& self, int x)"
  ```

## Special Call Contexts

<!-- BEGIN GENERATED ITEMS: Special Call Contexts -->

- [x] Constructors and aggregates — constructor calls render without a return arrow; aggregate initialization lists the fields in braces ([clangd#726](https://github.com/clangd/clangd/issues/726), [clangd#2541](https://github.com/clangd/clangd/issues/2541))

  <details>
  <summary>Example</summary>

  ```cpp
  struct Point {
      int x;
      int y;
  };

  struct Widget {
      Widget(int a, double b);
  };

  int main() {
      Point p{1, 2};
      Widget w(3, 4.0);
  }
  ```

  </details>

- [x] Function pointer calls — the prototype's parameter names show, not just the types

  <details>
  <summary>Example</summary>

  ```cpp
  int main() {
      void (*callback)(int code, double value) = nullptr;
      callback(5, 1.5);
  }
  ```

  </details>

- [x] Template argument lists — template parameters show as the signature; a class template points at its kind, not a return type ([clangd#299](https://github.com/clangd/clangd/issues/299), [clangd#1387](https://github.com/clangd/clangd/issues/1387))

  <details>
  <summary>Example</summary>

  ```cpp
  template <typename T, typename U>
  struct Pair {};

  Pair<int,  double> p;
  ```

  </details>

- [x] Nested calls — the inner call's help shows at the inner marker and the outer call's help at the outer marker

  <details>
  <summary>Example</summary>

  ```cpp
  int inner(int a);
  int outer(int b, int c);

  int main() {
      outer(inner(1), 2);
  }
  ```

  </details>

- [x] Functor call — invoking an object routes signature help to its operator() overload

  <details>
  <summary>Example</summary>

  ```cpp
  struct Adder {
      int operator()(int a, int b);
  };

  int main() {
      Adder add;
      add(1, 2);
  }
  ```

  </details>

- [x] Lambda call — calling a lambda variable offers the closure's operator() parameters

  <details>
  <summary>Example</summary>

  ```cpp
  int main() {
      auto square = [](int n) {
          return n * n;
      };
      square(3);
  }
  ```

  </details>

- [x] New expression — a new-expression's constructor arguments drive signature help

  <details>
  <summary>Example</summary>

  ```cpp
  struct Node {
      Node(int value, Node* next);
  };

  int main() {
      Node* n = new Node(0, nullptr);
  }
  ```

  </details>

<!-- END GENERATED ITEMS -->

- [ ] Inherited constructors — show base class constructors when calling from derived ([clangd#1363](https://github.com/clangd/clangd/issues/1363))

  ```cpp
  struct Base { Base(int x, int y); };
  struct Derived : Base { using Base::Base; };
  Derived d(^  // show Base(int x, int y)
  ```

- [ ] `operator[]` signature help ([clangd#2472](https://github.com/clangd/clangd/issues/2472))

  ```cpp
  std::map<std::string, int> m;
  m[^  // show operator[](const string& key)
  ```

- [ ] Lambda calls — show lambda name instead of `operator()` ([clangd#86](https://github.com/clangd/clangd/issues/86))

  ```cpp
  auto validate = [](int x, int max) -> bool { ... };
  validate(^  // show "validate(int x, int max) -> bool", not "operator()(int x, int max)"
  ```

- [ ] Function pointer calls — show parameter names ([clangd#1068](https://github.com/clangd/clangd/issues/1068), [clangd#1729](https://github.com/clangd/clangd/issues/1729))

  ```cpp
  void (*callback)(int status, const char* msg);
  callback(^  // show "(int status, const char* msg)"
  ```

- [ ] Constructor signature help during object initialization

- [ ] Macro function calls — show macro parameters, not the underlying expansion ([clangd#795](https://github.com/clangd/clangd/issues/795))

  ```cpp
  #define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)
  CHECK(^  // show "CHECK(cond, msg)", not "fail(const char*)"
  ```

## Parameter Display

- [ ] Forwarding function parameter resolution — show underlying constructor parameters for `std::make_unique`, `emplace_back`, etc. ([clangd#517](https://github.com/clangd/clangd/issues/517))

  ```cpp
  struct Widget { Widget(int width, int height); };
  std::make_unique<Widget>(^  // show "(int width, int height)"
  ```

- [ ] Parameter pack display ([clangd#638](https://github.com/clangd/clangd/issues/638))

  ```cpp
  template<typename... Args>
  void log(const char* fmt, Args&&... args);
  log("x=%d y=%d", ^  // show "fmt, args..." with active parameter on args
  ```

- [ ] Prettify standard library parameter names ([clangd#736](https://github.com/clangd/clangd/issues/736))

  ```
  // current:  push_back(const value_type& __x)
  // expected: push_back(const value_type& value)
  ```

- [ ] Preserve enum class scope in parameter types ([clangd#2475](https://github.com/clangd/clangd/issues/2475))

  ```cpp
  enum class Color { Red, Green, Blue };
  void paint(Color c);
  paint(^  // show "(Color c)", not "(c)" with scope stripped
  ```

- [ ] Show default parameter values

  ```cpp
  void open(std::string path, int mode = 0644);
  open("file", ^  // show "int mode = 0644" (active), user knows it can be omitted
  ```

## Documentation

- [ ] Documentation for the active parameter (from `@param` doc comments)

  ```cpp
  /// @param path The file system path.
  /// @param mode POSIX file permission bits.
  void open(std::string path, int mode);
  open("file", ^  // show documentation for mode parameter
  ```

- [ ] Respect `documentationFormat` capability ([clangd#945](https://github.com/clangd/clangd/issues/945))
- [ ] Propagate documentation through inherited constructors ([clangd#1936](https://github.com/clangd/clangd/issues/1936))
- [ ] Overload set count indicator

## Changelog

| Date       | Change                                                                      | PR                                                 |
| ---------- | --------------------------------------------------------------------------- | -------------------------------------------------- |
| 2025-08-23 | Function overload signatures, active parameter tracking, trigger characters | [#187](https://github.com/clice-io/clice/pull/187) |
