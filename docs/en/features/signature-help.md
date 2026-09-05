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

<!-- BEGIN GENERATED ITEMS: overload_signatures -->

<!-- BEGIN CAPABILITY: supported -->

**Function overloads**

Every overload of the callee, each with its parameter list and return type

```snap-signature_help
feature: signature_help
code: |
  void foo();
  void foo(int x);
  void foo(int x, int y);

  int main() {
      foo(§(pos));
  }
snapshot: |
  pos:
  - foo(⟦int x⟧, int y) -> void
  - foo(⟦int x⟧) -> void
  - foo() -> void
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Active parameter tracking**

The parameter under the cursor is bracketed; the point sits in the second argument

```snap-signature_help
feature: signature_help
code: |
  void bar(int first, double second, char third);

  int main() {
      bar(1, §(pos)2.0, 'c');
  }
snapshot: |
  pos:
  - bar(int first, ⟦double second⟧, char third) -> void
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Member function overloads**

A non-const receiver lists both the const and non-const overloads; the trailing const qualifier is not rendered in the label

```snap-signature_help
feature: signature_help
code: |
  struct Buffer {
      int at(int index);
      int at(int index) const;
  };

  int main() {
      Buffer b;
      b.at(§(pos)0);
  }
snapshot: |
  pos:
  - at(⟦int index⟧) -> int
  - at(⟦int index⟧) -> int
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default arguments in the label**

Parameters with defaults render their initializer in the signature

```snap-signature_help
feature: signature_help
code: |
  void configure(int width, int height = 100, bool visible = true);

  int main() {
      configure(§(pos)1);
  }
snapshot: |
  pos:
  - configure(⟦int width⟧, int height = 100, bool visible = true) -> void
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C-style variadic function**

Named parameters are listed while the trailing ellipsis is elided from the label

```snap-signature_help
feature: signature_help
code: |
  void record(int code, ...);

  int main() {
      record(§(pos)0);
  }
snapshot: |
  pos:
  - record(⟦int code⟧) -> void
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variadic template pack**

The parameter pack renders as the callee's uninstantiated signature

```snap-signature_help
feature: signature_help
code: |
  template <typename... Args>
  void emit(Args... args);

  int main() {
      emit(§(pos));
  }
snapshot: |
  pos:
  - emit(⟦Args ...args⟧) -> void
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Active parameter past a shorter overload**

With the cursor in the second argument, only overloads that declare a second parameter remain

```snap-signature_help
feature: signature_help
code: |
  void draw();
  void draw(int x);
  void draw(int x, int y);

  int main() {
      draw(1, §(pos)2);
  }
snapshot: |
  pos:
  - draw(int x, ⟦int y⟧) -> void
```

<!-- END CAPABILITY -->

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

<!-- BEGIN GENERATED ITEMS: special_call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 clangd#2541 -->

**Constructors and aggregates**

Constructor calls render without a return arrow; aggregate initialization lists the fields in braces

```snap-signature_help
feature: signature_help
code: |
  struct Point {
      int x;
      int y;
  };

  struct Widget {
      Widget(int a, double b);
  };

  int main() {
      Point p{1, §(aggregate)2};
      Widget w(§(ctor)3, 4.0);
  }
snapshot: |
  aggregate:
  - Point{int x, ⟦int y⟧}

  ctor:
  - Widget(⟦int a⟧, double b)
  - Widget(⟦const Widget &⟧)
  - Widget(⟦Widget &&⟧)
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function pointer calls**

The prototype's parameter names show, not just the types

```snap-signature_help
feature: signature_help
code: |
  int main() {
      void (*callback)(int code, double value) = nullptr;
      callback(§(pos)5, 1.5);
  }
snapshot: |
  pos:
  - (⟦int code⟧, double value) -> void
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#299 clangd#1387 -->

**Template argument lists**

Template parameters show as the signature; a class template points at its kind, not a return type

```snap-signature_help
feature: signature_help
code: |
  template <typename T, typename U>
  struct Pair {};

  Pair<int, §(pos) double> p;
snapshot: |
  pos:
  - Pair<typename T, ⟦typename U⟧> -> struct
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested calls**

The inner call's help shows at the inner marker and the outer call's help at the outer marker

```snap-signature_help
feature: signature_help
code: |
  int inner(int a);
  int outer(int b, int c);

  int main() {
      outer(inner(§(deep)1), §(shallow)2);
  }
snapshot: |
  deep:
  - inner(⟦int a⟧) -> int

  shallow:
  - outer(int b, ⟦int c⟧) -> int
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functor call**

Invoking an object routes signature help to its operator() overload

```snap-signature_help
feature: signature_help
code: |
  struct Adder {
      int operator()(int a, int b);
  };

  int main() {
      Adder add;
      add(§(pos)1, 2);
  }
snapshot: |
  pos:
  - operator()(⟦int a⟧, int b) -> int
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda call**

Calling a lambda variable offers the closure's operator() parameters

```snap-signature_help
feature: signature_help
code: |
  int main() {
      auto square = [](int n) {
          return n * n;
      };
      square(§(pos)3);
  }
snapshot: |
  pos:
  - operator()(⟦int n⟧) -> int
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**New expression**

A new-expression's constructor arguments drive signature help

```snap-signature_help
feature: signature_help
code: |
  struct Node {
      Node(int value, Node* next);
  };

  int main() {
      Node* n = new Node(§(pos)0, nullptr);
  }
snapshot: |
  pos:
  - Node(⟦int value⟧, Node *next)
  - Node(⟦const Node &⟧)
  - Node(⟦Node &&⟧)
```

<!-- END CAPABILITY -->

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
