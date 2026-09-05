# Hover

Rich information cards for the symbol under the cursor.

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/hover/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Symbol Information

<!-- BEGIN GENERATED ITEMS: symbol_information -->

<!-- BEGIN CAPABILITY: supported -->

**Qualified name**

The hover card shows the enclosing namespace and class scope

````snap-hover
feature: hover
code: |
  namespace app::detail {

  struct Engine {
      void tick() {
          int co§(method_local)unt = 0;
      }
  };

  int wor§(ns_var)kers = 4;

  }

  int glo§(global_var)bal = 1;
snapshot: |
  global_var: { range: "16:4-16:10" }
  ### variable `global`

  ---
  Type: `int`\
  Value = `1`

  ---
  ```cpp
  int global = 1
  ```

  method_local: { range: "8:12-8:17" }
  ### variable `count`

  ---
  Type: `int`\
  Value = `0`

  ---
  ```cpp
  // In Engine::tick
  int count = 0
  ```

  ns_var: { range: "12:4-12:11" }
  ### variable `workers`

  ---
  Type: `int`\
  Value = `4`

  ---
  ```cpp
  // In namespace app::detail
  int workers = 4
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Symbol kind**

The card names what the symbol is: struct, enum, function, field, …

````snap-hover
feature: hover
code: |
  namespace ki§(namespace)nds {

  struct Poi§(struct)nt {
      int §(field)x;
  };

  union Pack§(union)et {
      int raw;
  };

  enum class Col§(enum)or {
      R§(enumerator)ed,
  };

  using Ali§(typedef)as = Point;

  int leng§(function)th(Point p) {
      return p.x;
  }

  }
snapshot: |
  enum: { range: "14:11-14:16" }
  ### enum `Color`

  ---
  ```cpp
  // In namespace kinds
  enum class Color : int {}
  ```

  enumerator: { range: "15:4-15:7" }
  ### enumerator `Red`

  ---
  Type: `enum kinds::Color`\
  Value = `0`

  ---
  ```cpp
  // In Color
  Red
  ```

  field: { range: "7:8-7:9" }
  ### field `x`

  ---
  Type: `int`\
  Offset: 0 bytes\
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In Point
  public: int x
  ```

  function: { range: "20:4-20:10" }
  ### function `length`

  ---
  → `int`\
  Parameters:\
  - `Point p`

  ---
  ```cpp
  // In namespace kinds
  int length(Point p)
  ```

  namespace: { range: "4:10-4:15" }
  ### namespace `kinds`

  ---
  ```cpp
  namespace kinds {}
  ```

  struct: { range: "6:7-6:12" }
  ### struct `Point`

  ---
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In namespace kinds
  struct Point {}
  ```

  typedef: { range: "18:6-18:11" }
  ### type `Alias`

  ---
  Type: `struct kinds::Point`

  ---
  ```cpp
  // In namespace kinds
  using Alias = Point
  ```

  union: { range: "10:6-10:12" }
  ### union `Packet`

  ---
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In namespace kinds
  union Packet {}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Access specifier**

Members show their public / protected / private access

````snap-hover
feature: hover
code: |
  class Account {
  public:
      int bal§(public_field)ance;

  protected:
      int lim§(protected_field)it;

  private:
      int p§(private_field)in;
  };
snapshot: |
  private_field: { range: "12:8-12:11" }
  ### field `pin`

  ---
  Type: `int`\
  Offset: 8 bytes\
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In Account
  private: int pin
  ```

  protected_field: { range: "9:8-9:13" }
  ### field `limit`

  ---
  Type: `int`\
  Offset: 4 bytes\
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In Account
  protected: int limit
  ```

  public_field: { range: "6:8-6:15" }
  ### field `balance`

  ---
  Type: `int`\
  Offset: 0 bytes\
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In Account
  public: int balance
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Definition rendering**

The card includes the symbol's source definition

````snap-hover
feature: hover
code: |
  namespace retry {

  constexpr int max_retries = 3;

  int back§(function_def)off(int attempt = 1) {
      return attempt * max_ret§(var_ref)ries;
  }

  }
snapshot: |
  function_def: { range: "8:4-8:11" }
  ### function `backoff`

  ---
  → `int`\
  Parameters:\
  - `int attempt = 1`

  ---
  ```cpp
  // In namespace retry
  int backoff(int attempt = 1)
  ```

  var_ref: { range: "9:21-9:32" }
  ### variable `max_retries`

  ---
  Type: `const int`\
  Value = `3`

  ---
  ```cpp
  // In namespace retry
  constexpr int max_retries = 3
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#710 -->

**Initializer truncation**

Huge initializers render truncated, not in full

The rendered definition omits the initializer, but the evaluated
`Value` field still spells out all 256 elements.

````snap-hover
feature: hover
code: |
  #define A(x) x, x, x, x
  #define B(x) A(A(A(A(x))))
  int a§(big_initializer)rr[] = {B(0)};
snapshot: |
  big_initializer: { range: "13:4-13:7" }
  ### variable `arr`

  ---
  Type: `int[256]`\
  Value = `{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}`

  ---
  ```cpp
  int arr[]
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2474 -->

**Virtual modifiers**

`virtual` / `override` / `final` show on method hover

Modifiers written in the source render (`virtual … = 0`, `override`,
`final`), but an overriding method that omits the redundant `virtual`
keyword gives no sign of its virtuality — the card lacks the
`virtual void draw() override` form the issue asks for.

````snap-hover
feature: hover
code: |
  struct Base {
      virtual void dr§(pure_virtual)aw() = 0;
  };

  struct Circle : Base {
      void dr§(override_method)aw() override;
  };

  struct Dot final : Circle {
      void dr§(final_method)aw() final;
  };
snapshot: |
  final_method: { range: "19:9-19:13" }
  ### method `draw`

  ---
  → `void`

  ---
  ```cpp
  // In Dot
  public: void draw() final
  ```

  override_method: { range: "15:9-15:13" }
  ### method `draw`

  ---
  → `void`

  ---
  ```cpp
  // In Circle
  public: void draw() override
  ```

  pure_virtual: { range: "11:17-11:21" }
  ### method `draw`

  ---
  → `void`

  ---
  ```cpp
  // In Base
  public: virtual void draw() = 0
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#436 -->

**Anonymous namespace scope**

`(anonymous namespace)` shows in the scope display

The cards render, but the anonymous segment is dropped from the
scope display: a top-level anonymous member shows no scope line at
all, and `outer::(anonymous)` shows just `outer`.

````snap-hover
feature: hover
code: |
  namespace {
  int hid§(anon_var)den = 1;
  }

  namespace outer {
  namespace {
  int nes§(nested_anon_var)ted = 2;
  }
  }

  int sum = hidden + outer::nested;
snapshot: |
  anon_var: { range: "10:4-10:10" }
  ### variable `hidden`

  ---
  Type: `int`\
  Value = `1`

  ---
  ```cpp
  int hidden = 1
  ```

  nested_anon_var: { range: "15:4-15:10" }
  ### variable `nested`

  ---
  Type: `int`\
  Value = `2`

  ---
  ```cpp
  // In namespace outer
  int nested = 2
  ```
````

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Type Information

<!-- BEGIN GENERATED ITEMS: type_information -->

<!-- BEGIN CAPABILITY: supported -->

**Variable types**

pointers, references, arrays

A variable's card pretty-prints its declared type, spelling the pointer,
reference and array declarators the way they read in source.

````snap-hover
feature: hover
code: |
  namespace variable_type {

  int target;

  int *§(01_pointer)ptr = &target;

  int &§(02_reference)ref = target;

  int §(03_array)numbers[4]{};

  }
snapshot: |
  01_pointer: { range: "11:5-11:8" }
  ### variable `ptr`

  ---
  Type: `int *`\
  Value = `&target`

  ---
  ```cpp
  // In namespace variable_type
  int *ptr = &target
  ```

  02_reference: { range: "13:5-13:8" }
  ### variable `ref`

  ---
  Type: `int &`

  ---
  ```cpp
  // In namespace variable_type
  int &ref = target
  ```

  03_array: { range: "15:4-15:11" }
  ### variable `numbers`

  ---
  Type: `int[4]`\
  Value = `{}`

  ---
  ```cpp
  // In namespace variable_type
  int numbers[4] {}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type aliases**

The desugared `aka` form

A sugared type shows its underlying type as `Alias (aka int)`. The
`show_aka` option turns the `aka` suffix off.

````snap-hover
feature: hover
code: |
  namespace aka_desugar {

  using Handle = int;
  using Alias = Handle;

  Handle §(01_alias)direct = 0;

  Alias §(02_alias_chain)chained = 0;

  }
snapshot: |
  default:
    01_alias: { range: "13:7-13:13" }
    ### variable `direct`

    ---
    Type: `Handle (aka int)`\
    Value = `0`

    ---
    ```cpp
    // In namespace aka_desugar
    Handle direct = 0
    ```

    02_alias_chain: { range: "15:6-15:13" }
    ### variable `chained`

    ---
    Type: `Alias (aka int)`\
    Value = `0`

    ---
    ```cpp
    // In namespace aka_desugar
    Alias chained = 0
    ```

  configured:
    01_alias: { range: "13:7-13:13" }
    ### variable `direct`

    ---
    Type: `Handle`\
    Value = `0`

    ---
    ```cpp
    // In namespace aka_desugar
    Handle direct = 0
    ```

    02_alias_chain: { range: "15:6-15:13" }
    ### variable `chained`

    ---
    Type: `Alias`\
    Value = `0`

    ---
    ```cpp
    // In namespace aka_desugar
    Alias chained = 0
    ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function signatures**

Return type, parameter names, defaults

A function's card lists its return type, each parameter with its name,
and any default argument.

````snap-hover
feature: hover
code: |
  namespace function_signature {

  int §(01_params)add(int lhs, int rhs);

  void §(02_defaults)configure(int width, bool visible = true);

  }
snapshot: |
  01_params: { range: "9:4-9:7" }
  ### function `add`

  ---
  → `int`\
  Parameters:\
  - `int lhs`
  - `int rhs`

  ---
  ```cpp
  // In namespace function_signature
  int add(int lhs, int rhs)
  ```

  02_defaults: { range: "11:5-11:14" }
  ### function `configure`

  ---
  → `void`\
  Parameters:\
  - `int width`
  - `bool visible = true`

  ---
  ```cpp
  // In namespace function_signature
  void configure(int width, bool visible = true)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template parameters**

type, template-template, non-type

Each template parameter kind reports its form: a type parameter, a
template-template parameter, and a non-type parameter with its default.

````snap-hover
feature: hover
code: |
  // Template type parameter.
  namespace type_param {
  template <typename §(01_type_param)T = int> void foo();
  }

  // Template template parameter.
  namespace template_template_param {
  template <template<typename> class §(02_template_template_param)T> void foo();
  }

  // Non-type template parameter.
  namespace non_type_param {
  template <int §(03_non_type_param)T = 5> void foo();
  }
snapshot: |
  01_type_param: { range: "12:19-12:20" }
  ### type `T`

  ---
  Type: `typename`

  ---
  ```cpp
  // In foo
  public: typename T = int
  ```

  02_template_template_param: { range: "17:35-17:36" }
  ### type `T`

  ---
  Type: `template <typename> class`

  ---
  ```cpp
  // In foo
  public: template <typename> class T
  ```

  03_non_type_param: { range: "22:14-22:15" }
  ### variable `T`

  ---
  Type: `int`

  ---
  ```cpp
  // In foo
  public: int T = 5
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`auto` deduction**

The type the placeholder resolves to

Hovering an `auto` placeholder shows the type substituted for it —
builtins, pointers, lambdas, template instantiations, and the
`/* not deduced */` marker inside an uninstantiated template.

````snap-hover
feature: hover
code: |
  namespace auto_deduction {

  struct Bar {};
  struct Pair { int first; int second; };
  template <typename T> struct Box {};

  void locals() {
    int n = 0;
    §(01_simple)auto a = 1;
    const §(02_const)auto b = 1;
    §(03_ref)auto& c = n;
    §(04_ptr)auto* d = &n;
    §(05_from_pointer)auto e = &n;
    §(06_lambda)auto f = []{};
    §(07_instantiation)auto g = Box<int>();
    §(08_structured)auto [x, y] = Pair{};
  }

  §(09_trailing_return)auto with_trailing() -> int { return 0; }

  §(10_fn_return)auto deduced_return() { return Bar(); }

  template <typename T> void undeduced() {
    §(11_undeduced)auto u = T();
  }

  }
snapshot: |
  01_simple: { range: "19:2-19:6" }
  ### type `auto`

  ---
  ```cpp
  int
  ```

  02_const: { range: "20:8-20:12" }
  ### type `auto`

  ---
  ```cpp
  int
  ```

  03_ref: { range: "21:2-21:6" }
  ### type `auto`

  ---
  ```cpp
  int
  ```

  04_ptr: { range: "22:2-22:6" }
  ### type `auto`

  ---
  ```cpp
  int
  ```

  05_from_pointer: { range: "23:2-23:6" }
  ### type `auto`

  ---
  ```cpp
  int *
  ```

  06_lambda: { range: "24:2-24:6" }
  ### type `auto`

  ---
  ```cpp
  class(lambda)
  ```

  07_instantiation: { range: "25:2-25:6" }
  ### type `auto`

  ---
  ```cpp
  Box<int>
  ```

  08_structured: { range: "26:2-26:6" }
  ### type `auto`

  ---
  ```cpp
  Pair
  ```

  09_trailing_return: { range: "29:0-29:4" }
  ### type `auto`

  ---
  ```cpp
  int
  ```

  10_fn_return: { range: "31:0-31:4" }
  ### type `auto`

  ---
  ```cpp
  Bar
  ```

  11_undeduced: { range: "34:2-34:6" }
  ### type `auto`

  ---
  ```cpp
  /* not deduced */
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`decltype` deduction**

value, reference and dependent forms

Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
type, including the reference the parenthesized-expression rule adds.

````snap-hover
feature: hover
code: |
  namespace decltype_deduction {

  int base = 0;

  void locals() {
    int n = 0;
    const int cn = 0;
    int& r = n;
    §(01_value)decltype(auto) a = 1;
    §(02_const)decltype(auto) b = cn;
    §(03_ref)decltype(auto) c = r;
    §(04_of_lvalue)decltype(n) d = n;
    §(05_of_paren)decltype((n)) e = n;
    §(06_of_rvalue)decltype(static_cast<int&&>(n)) f = static_cast<int&&>(n);
  }

  decltype(base) §(07_var_type)mirror = base;

  template <typename T> §(08_undeduced)decltype(auto) undeduced() { return T(); }

  template <typename T> struct Dependent {
    using kind = §(09_dependent)decltype(T::member);
  };

  }
snapshot: |
  01_value: { range: "18:2-18:10" }
  ### type `decltype`

  ---
  ```cpp
  int
  ```

  02_const: { range: "19:2-19:10" }
  ### type `decltype`

  ---
  ```cpp
  const int
  ```

  03_ref: { range: "20:2-20:10" }
  ### type `decltype`

  ---
  ```cpp
  int &
  ```

  04_of_lvalue: { range: "21:2-21:10" }
  ### type `decltype`

  ---
  ```cpp
  int
  ```

  05_of_paren: { range: "22:2-22:10" }
  ### type `decltype`

  ---
  ```cpp
  int &
  ```

  06_of_rvalue: { range: "23:2-23:10" }
  ### type `decltype`

  ---
  ```cpp
  int &&
  ```

  07_var_type: { range: "26:15-26:21" }
  ### variable `mirror`

  ---
  Type: `int`

  ---
  ```cpp
  // In namespace decltype_deduction
  decltype(base) mirror = base
  ```

  08_undeduced: { range: "28:22-28:30" }
  ### type `decltype`

  ---
  ```cpp
  /* not deduced */
  ```

  09_dependent: { range: "31:15-31:23" }
  ### type `decltype`

  ---
  ```cpp
  <dependent type>
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#435 -->

**CTAD**

Deduced template arguments of a class placeholder

With class template argument deduction the variable's card shows the
deduced `Box<int>`, but hovering the class-name spelling still reports
the primary template without its arguments.

````snap-hover
feature: hover
code: |
  namespace ctad_arguments {

  template <typename T> struct Box {
    Box(T);
  };

  §(01_ctad_type)Box §(02_ctad_var)picked(42);

  }
snapshot: |
  01_ctad_type: { range: "15:0-15:3" }
  ### struct `Box`

  ---
  ```cpp
  // In namespace ctad_arguments
  template <typename T> struct Box {}
  ```

  02_ctad_var: { range: "15:4-15:10" }
  ### variable `picked`

  ---
  Type: `Box<int> (aka ctad_arguments::Box<int>)`

  ---
  ```cpp
  // In namespace ctad_arguments
  Box picked(42)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#230 -->

**Instantiation arguments**

Template parameters bound at a use site

A use of a template shows the substituted types (`Wrapper<int>`,
`identity<int>`, `int x`), but not an explicit `T = int` mapping of each
parameter to the argument it was bound to.

````snap-hover
feature: hover
code: |
  namespace instantiation_args {

  template <typename T> struct Wrapper {
    T value;
  };

  template <typename T> T identity(T x) {
    return x;
  }

  void demo() {
    §(01_type_use)Wrapper<int> holder;
    int r = §(02_call)identity(42);
  }

  }
snapshot: |
  01_type_use: { range: "20:2-20:9" }
  ### struct `Wrapper<int>`

  ---
  ```cpp
  // In namespace instantiation_args
  template <> struct Wrapper<int> {}
  ```

  02_call: { range: "21:10-21:18" }
  ### function `identity`

  ---
  → `int`\
  Parameters:\
  - `int x`

  ---
  ```cpp
  // In namespace instantiation_args
  template <> int identity<int>(int x)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#493 -->

**Lambda `auto` parameters**

Deduced parameter type

Hovering the `auto` parameter of a generic lambda yields no card; the
deduced parameter type is not shown.

```snap-hover
feature: hover
code: |
  namespace lambda_auto_params {

  auto printer = [](auto value) { return value; };

  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Sugared `auto`**

Alias sugar preserved through deduction

clangd tracks lost alias sugar through `auto` as clangd#709; clice
already keeps the alias spelling and appends its desugared form, so
`auto` deduced from an aliased return type reads as `Outer // aka: int`.

````snap-hover
feature: hover
code: |
  namespace sugared_auto {

  using Inner = int;
  using Outer = Inner;

  Outer make();

  void demo() {
    §(01_auto)auto value = make();
  }

  }
snapshot: |
  01_auto: { range: "16:2-16:6" }
  ### type `auto`

  ---
  ```cpp
  Outer // aka: int
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2156 -->

**Type formatting**

clang-format applied to rendered types

Long or nested types are printed by the compiler's default type printer;
they are not re-wrapped or aligned through clang-format.

```snap-hover
feature: hover
code: |
  namespace clang_format_types {

  template <typename A, typename B, typename C, typename D>
  struct Tuple {};

  Tuple<int, long, unsigned, char> wide;

  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2219 -->

**Anonymous struct typedef**

The classic C `typedef struct {…} Name`

Compiled as C11: clangd renders a misleading `struct Point` for the
alias of an anonymous struct; clice names the struct after its typedef,
so both the alias and a variable of it report a clean `Point` card.

````snap-hover
feature: hover
code: |
  /// A 2-D point.
  typedef struct {
    int x, y;
  } §(01_typedef)Point;

  Point §(02_var)origin = {.y = 2, .x = 1};
snapshot: |
  01_typedef: { range: "16:2-16:7" }
  ### type `Point`

  ---
  Type: `Point`\
  A 2-D point.

  ---
  ```cpp
  typedef struct Point Point
  ```

  02_var: { range: "18:6-18:12" }
  ### variable `origin`

  ---
  Type: `Point`

  ---
  ```cpp
  Point origin = {.y = 2, .x = 1}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Concept constraints**

The constraint behind a parameter or `auto` placeholder

The constrained-parameter and concept-reference cards carry the
constraint, but hovering the placeholder of a constrained `Addable auto`
variable shows only the deduced type — the constraint is dropped.

````snap-hover
feature: hover
code: |
  namespace concept_constraints {

  template <typename T>
  concept Addable = requires(T a) { a + a; };

  template <§(01_concept_name)Addable §(02_param_name)U>
  void sum(U a, U b);

  auto flag = §(03_concept_ref)Addable<int>;

  Addable §(04_constrained_auto)auto §(05_constrained_var)total = 1;

  }
snapshot: |
  01_concept_name: { range: "13:10-13:17" }
  ### concept `Addable`

  ---
  ```cpp
  // In namespace concept_constraints
  template <typename T>
  concept Addable = requires(T a) { a + a; }
  ```

  02_param_name: { range: "13:18-13:19" }
  ### type `U`

  ---
  Type: `class`

  ---
  ```cpp
  // In sum
  public: Addable U
  ```

  03_concept_ref: { range: "16:12-16:19" }
  ### concept `Addable`

  ---
  Value = `true`

  ---
  ```cpp
  // In namespace concept_constraints
  template <typename T>
  concept Addable = requires(T a) { a + a; }
  ```

  04_constrained_auto: { range: "18:8-18:12" }
  ### type `auto`

  ---
  ```cpp
  int
  ```

  05_constrained_var: { range: "18:13-18:18" }
  ### variable `total`

  ---
  Type: `int`\
  Value = `1`

  ---
  ```cpp
  // In namespace concept_constraints
  Addable auto total = 1
  ```
````

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Layout Information

<!-- BEGIN GENERATED ITEMS: layout_information -->

<!-- BEGIN CAPABILITY: supported -->

**Field layout**

size, offset, alignment and padding show on field hover

The corpus pins an x86-64 target, so the bit numbers are stable.

````snap-hover
feature: hover
code: |
  struct Header {
      char t§(plain_field)ag;
      int len§(padded_field)gth;
  };

  struct Flags {
      int rea§(bitfield)dy : 1;
      int e§(bitfield_padding)nd : 1;
  };
snapshot: |
  bitfield: { range: "15:8-15:13" }
  ### field `ready`

  ---
  Type: `int`\
  Offset: 0 bytes\
  Size: 1 bit, alignment 4 bytes

  ---
  ```cpp
  // In Flags
  public: int ready : 1
  ```

  bitfield_padding: { range: "16:8-16:11" }
  ### field `end`

  ---
  Type: `int`\
  Offset: 0 bytes and 1 bit\
  Size: 1 bit (+30 bits padding), alignment 4 bytes

  ---
  ```cpp
  // In Flags
  public: int end : 1
  ```

  padded_field: { range: "11:8-11:14" }
  ### field `length`

  ---
  Type: `int`\
  Offset: 4 bytes\
  Size: 4 bytes, alignment 4 bytes

  ---
  ```cpp
  // In Header
  public: int length
  ```

  plain_field: { range: "10:9-10:12" }
  ### field `tag`

  ---
  Type: `char`\
  Offset: 0 bytes\
  Size: 1 byte (+3 bytes padding), alignment 1 byte

  ---
  ```cpp
  // In Header
  public: char tag
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1763 -->

**Type-level layout**

Hovering the type itself shows its size, alignment and padding

Size and alignment show on the type card today; the total padding
does not yet.

````snap-hover
feature: hover
code: |
  namespace layout {

  struct Wid§(struct_size)get {
      int id;
      double value;
  };

  }
snapshot: |
  struct_size: { range: "10:7-10:13" }
  ### struct `Widget`

  ---
  Size: 16 bytes, alignment 8 bytes

  ---
  ```cpp
  // In namespace layout
  struct Widget {}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1771 -->

**Vtable offset**

Virtual methods show their table slot

The method card renders without any vtable fact today.

````snap-hover
feature: hover
code: |
  struct Shape {
      virtual void dr§(first_virtual)aw();
      virtual void mo§(second_virtual)ve();
  };
snapshot: |
  first_virtual: { range: "8:17-8:21" }
  ### method `draw`

  ---
  → `void`

  ---
  ```cpp
  // In Shape
  public: virtual void draw()
  ```

  second_virtual: { range: "9:17-9:21" }
  ### method `move`

  ---
  → `void`

  ---
  ```cpp
  // In Shape
  public: virtual void move()
  ```
````

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Expression Context

<!-- BEGIN GENERATED ITEMS: expression_context -->

<!-- BEGIN CAPABILITY: supported -->

**Constant evaluation**

constexpr, enumerators, sizeof

When an initializer is a constant expression, the card evaluates it and
shows the resulting value.

````snap-hover
feature: hover
code: |
  namespace constant_value {

  constexpr int square(int n) { return n * n; }
  int §(01_constexpr_call)from_call = square(5);

  int §(02_sizeof)from_sizeof = sizeof(int);

  enum Color { Red = -1, Green = 5 };
  Color picked = §(03_enumerator)Green;

  template <int A, int B> struct Sum { static constexpr int value = A + B; };
  int §(04_static_member)from_member = Sum<3, 4>::value;

  }
snapshot: |
  01_constexpr_call: { range: "13:4-13:13" }
  ### variable `from_call`

  ---
  Type: `int`\
  Value = `25 (0x19)`

  ---
  ```cpp
  // In namespace constant_value
  int from_call = square(5)
  ```

  02_sizeof: { range: "15:4-15:15" }
  ### variable `from_sizeof`

  ---
  Type: `int`\
  Value = `4`

  ---
  ```cpp
  // In namespace constant_value
  int from_sizeof = sizeof(int)
  ```

  03_enumerator: { range: "18:15-18:20" }
  ### enumerator `Green`

  ---
  Type: `enum constant_value::Color`\
  Value = `5`

  ---
  ```cpp
  // In Color
  Green = 5
  ```

  04_static_member: { range: "21:4-21:15" }
  ### variable `from_member`

  ---
  Type: `int`\
  Value = `7`

  ---
  ```cpp
  // In namespace constant_value
  int from_member = Sum<3, 4>::value
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call arguments**

Which parameter each argument binds to

Hovering an argument at a call site shows the parameter it is passed to,
naming the parameter it binds.

````snap-hover
feature: hover
code: |
  namespace callee_arguments {

  void configure(int width, int& out, int flags = 0);

  void demo() {
    int w = 1024;
    int result = 0;
    configure(§(01_by_name)w, §(02_by_ref)result, §(03_literal)3);
  }

  }
snapshot: |
  01_by_name: { range: "17:12-17:13" }
  ### variable `w`

  ---
  Type: `int`\
  Value = `1024 (0x400)`\
  Passed as width

  ---
  ```cpp
  // In demo
  int w = 1024
  ```

  02_by_ref: { range: "17:15-17:21" }
  ### variable `result`

  ---
  Type: `int`\
  Value = `0`\
  Passed by reference as out

  ---
  ```cpp
  // In demo
  int result = 0
  ```

  03_literal: { range: "17:23-17:24" }
  ### `literal`

  ---
  Passed as flags
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pass semantics**

By value, by reference, by const reference

The argument card states how the value reaches the callee: copied by
value, or bound to a mutable or const reference parameter.

````snap-hover
feature: hover
code: |
  namespace pass_semantics {

  void by_value(int x);
  void by_ref(int& x);
  void by_const_ref(const int& x);

  void demo() {
    int n = 0;
    by_value(§(01_value)n);
    by_ref(§(02_ref)n);
    by_const_ref(§(03_const_ref)n);
  }

  }
snapshot: |
  01_value: { range: "18:11-18:12" }
  ### variable `n`

  ---
  Type: `int`\
  Value = `0`\
  Passed as x

  ---
  ```cpp
  // In demo
  int n = 0
  ```

  02_ref: { range: "19:9-19:10" }
  ### variable `n`

  ---
  Type: `int`\
  Value = `0`\
  Passed by reference as x

  ---
  ```cpp
  // In demo
  int n = 0
  ```

  03_const_ref: { range: "20:15-20:16" }
  ### variable `n`

  ---
  Type: `int`\
  Value = `0`\
  Passed by const reference as x

  ---
  ```cpp
  // In demo
  int n = 0
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Implicit conversions**

Argument converted to the parameter type

When an argument reaches a parameter through an implicit conversion, the
card notes the target type, for both built-in and user-defined
conversions.

````snap-hover
feature: hover
code: |
  namespace implicit_conversion {

  struct Wrapper {
    Wrapper(int value);
  };

  void take_float(float x);
  void take_wrapper(Wrapper w);

  void demo() {
    int n = 0;
    take_float(§(01_arithmetic)n);
    take_wrapper(§(02_user_defined)n);
  }

  }
snapshot: |
  01_arithmetic: { range: "19:13-19:14" }
  ### variable `n`

  ---
  Type: `int`\
  Value = `0`\
  Passed as x (converted to float)

  ---
  ```cpp
  // In demo
  int n = 0
  ```

  02_user_defined: { range: "20:15-20:16" }
  ### variable `n`

  ---
  Type: `int`\
  Value = `0`\
  Passed as w (converted to Wrapper)

  ---
  ```cpp
  // In demo
  int n = 0
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1016 -->

**String literals**

The length reported on hover

A string-literal card reports the array type and its size in bytes
(`const char[6]`, `Size: 6 bytes` — the length plus the null
terminator), not an explicit character count.

```snap-hover
feature: hover
code: |
  namespace string_length {

  const char *greeting = §(01_string)"hello";

  }
snapshot: |
  01_string: { range: "11:23-11:30" }
  ### `string-literal`

  ---
  Type: `const char[6]`\
  Size: 6 bytes
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1669 -->

**Numeric literals**

Type and value of an integer or float literal

Hovering a numeric literal yields no card, unlike character and string
literals, whose type and value are shown.

```snap-hover
feature: hover
code: |
  namespace numeric_literal_type {

  auto count = 42;
  auto ratio = 3.14;

  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1622 -->

**Record variables**

Enclosing constant value leaks in

Hovering a record-typed argument of a constant-evaluable call currently
reports that call's value (`Value = 7`) on the variable — a value that
is not the record's own.

````snap-hover
feature: hover
code: |
  namespace record_value_misleading {

  struct Tag {};

  constexpr int rank(Tag) {
    return 7;
  }

  void demo() {
    Tag t;
    int r = rank(§(01_record_arg)t);
  }

  }
snapshot: |
  01_record_arg: { range: "19:15-19:16" }
  ### variable `t`

  ---
  Type: `Tag`\
  Value = `7`\
  Passed by value

  ---
  ```cpp
  // In demo
  Tag t
  ```
````

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Documentation

<!-- BEGIN GENERATED ITEMS: documentation -->

<!-- BEGIN CAPABILITY: supported -->

**Doxygen `///` comments**

Extracted from the declaration and rendered on hover

Applies to plain functions, primary templates and their specializations;
a reference resolves to the most specialized declaration's comment.

````snap-hover
feature: hover
code: |
  namespace docs {
  /// Adds two integers.
  int §(01_function)add(int a, int b);

  /// A box holding a value.
  template <typename T> struct §(02_primary_def)Box {};

  /// A box of pointers.
  template <typename T> struct §(03_spec_def)Box<T*> {};

  void use() {
      Box§(04_primary_ref)<int> b;
      Box§(05_spec_ref)<int*> p;
  }
  }
snapshot: |
  01_function: { range: "12:4-12:7" }
  ### function `add`

  ---
  → `int`\
  Parameters:\
  - `int a`
  - `int b`

  Adds two integers.

  ---
  ```cpp
  // In namespace docs
  int add(int a, int b)
  ```

  02_primary_def: { range: "15:29-15:32" }
  ### struct `Box`

  ---
  A box holding a value.

  ---
  ```cpp
  // In namespace docs
  template <typename T> struct Box {}
  ```

  03_spec_def: { range: "18:29-18:32" }
  ### struct `Box<T *>`

  ---
  A box of pointers.

  ---
  ```cpp
  // In namespace docs
  template <typename T> struct Box<T *> {}
  ```

  04_primary_ref: { range: "21:4-21:7" }
  ### struct `Box<int>`

  ---
  A box holding a value.

  ---
  ```cpp
  // In namespace docs
  template <> struct Box<int> {}
  ```

  05_spec_ref: { range: "22:4-22:7" }
  ### struct `Box<int *>`

  ---
  A box of pointers.

  ---
  ```cpp
  // In namespace docs
  template <> struct Box<int *> {}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Synthesized accessor docs**

Trivial getters/setters get a generated one-line description

A trivial getter or setter with no comment of its own gets a synthesized
"Trivial accessor/setter for `field`." line in its hover card.

````snap-hover
feature: hover
code: |
  namespace accessors {
  struct Widget {
      int width;
      int §(01_getter)getWidth() { return width; }
      void §(02_setter)setWidth(int w) { width = w; }
  };
  }
snapshot: |
  01_getter: { range: "13:8-13:16" }
  ### method `getWidth`

  ---
  → `int`\
  Trivial accessor for `width`.

  ---
  ```cpp
  // In Widget
  public: int getWidth()
  ```

  02_setter: { range: "14:9-14:17" }
  ### method `setWidth`

  ---
  → `void`\
  Parameters:\
  - `int w`

  Trivial setter for `width`.

  ---
  ```cpp
  // In Widget
  public: void setWidth(int w)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1320 -->

**`@copydoc` tags**

Copy another symbol's documentation onto this one

A `@copydoc target` tag should copy `target`'s documentation into this
symbol's hover card. clice does not resolve the tag yet — the card shows
the literal `@copydoc base_func()` text.

````snap-hover
feature: hover
code: |
  namespace copydoc {
  /// Detailed documentation.
  void §(01_base)base_func();

  /// @copydoc base_func()
  void §(02_wrapper)wrapper();
  }
snapshot: |
  01_base: { range: "11:5-11:14" }
  ### function `base_func`

  ---
  → `void`\
  Detailed documentation.

  ---
  ```cpp
  // In namespace copydoc
  void base_func()
  ```

  02_wrapper: { range: "14:5-14:12" }
  ### function `wrapper`

  ---
  → `void`\
  @copydoc base_func()

  ---
  ```cpp
  // In namespace copydoc
  void wrapper()
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2504 -->

**Inherited override docs**

An override with no comment shows the base method's documentation

Hovering an overriding method that carries no comment of its own should
surface the documentation from the method it overrides. clice does not
inherit it yet — the override's card carries no description.

````snap-hover
feature: hover
code: |
  namespace inherit_docs {
  struct Base {
      /// Renders the widget.
      virtual void draw();
  };
  struct Circle : Base {
      void §(01_override)draw() override;
  };
  }
snapshot: |
  01_override: { range: "15:9-15:13" }
  ### method `draw`

  ---
  → `void`

  ---
  ```cpp
  // In Circle
  public: void draw() override
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2506 -->

**Overload doc sharing**

A later overload with no comment reuses the first overload's documentation

Consecutive overloads often document only the first; a later undocumented
overload should reuse that shared description. clice does not share it
yet — the later overload's card carries no description.

````snap-hover
feature: hover
code: |
  namespace overloads {
  /// Opens a file.
  void §(01_first)open(const char* path);
  void §(02_second)open(const char* path, int flags);
  }
snapshot: |
  01_first: { range: "11:5-11:9" }
  ### function `open`

  ---
  → `void`\
  Parameters:\
  - `const char * path`

  Opens a file.

  ---
  ```cpp
  // In namespace overloads
  void open(const char *path)
  ```

  02_second: { range: "12:5-12:9" }
  ### function `open`

  ---
  → `void`\
  Parameters:\
  - `const char * path`
  - `int flags`

  ---
  ```cpp
  // In namespace overloads
  void open(const char *path, int flags)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1936 -->

**Inherited constructor docs**

`using Base::Base;` surfaces the base constructor's documentation

A constructor pulled in with `using Base::Base;` should carry the base
constructor's documentation on hover. There is no hover surface for it:
the name in the using-declaration resolves to the class, not the
inherited constructor.

```snap-hover
feature: hover
code: |
  namespace inherited_ctor {
  struct Base {
      /// Constructs from a value.
      Base(int value);
  };
  struct Derived : Base {
      using Base::§(01_inherited_ctor)Base;
  };
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#974 -->

**Banner comments**

A section banner separated by a blank line must not attach to the next declaration

A `// ==== Section ====` banner followed by a blank line should not be
misattributed as documentation for the declaration below it. clice
currently attaches it anyway — the banner text appears in the card.

````snap-hover
feature: hover
code: |
  namespace banners {
  // ==== Section Banner ====

  void §(01_after_banner)foo();
  }
snapshot: |
  01_after_banner: { range: "12:5-12:8" }
  ### function `foo`

  ---
  → `void`\
  ==== Section Banner ====

  ---
  ```cpp
  // In namespace banners
  void foo()
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration vs definition comments**

The declaration's doc wins over a definition-site comment

clangd tracks this as clangd#829; clice already prefers the
declaration's `///` documentation over the definition's plain `//` note,
showing it at both the declaration and the definition site.

````snap-hover
feature: hover
code: |
  namespace decldef {
  /// Public API documentation.
  void §(01_at_decl)process(int x);

  // Internal implementation note.
  void §(02_at_def)process(int x) { (void)x; }
  }
snapshot: |
  01_at_decl: { range: "10:5-10:12" }
  ### function `process`

  ---
  → `void`\
  Parameters:\
  - `int x`

  Public API documentation.

  ---
  ```cpp
  // In namespace decldef
  void process(int x)
  ```

  02_at_def: { range: "13:5-13:12" }
  ### function `process`

  ---
  → `void`\
  Parameters:\
  - `int x`

  Public API documentation.

  ---
  ```cpp
  // In namespace decldef
  void process(int x)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2057 -->

**Whitespace and newlines**

A markdown table in a comment keeps its line breaks

A markdown table written across several `///` lines should render as a
table with its line breaks preserved. clice currently flattens the lines
onto one line, so the table does not render.

````snap-hover
feature: hover
code: |
  namespace tables {
  /// | Column A | Column B |
  /// |----------|----------|
  /// | 1        | 2        |
  void §(01_table)table_fn();
  }
snapshot: |
  01_table: { range: "13:5-13:13" }
  ### function `table_fn`

  ---
  → `void`\
  | Column A | Column B | |----------|----------| | 1 | 2 |

  ---
  ```cpp
  // In namespace tables
  void table_fn()
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1040 -->

**Comment indentation**

Indented lines in a comment render without spurious extra indentation

A doc comment whose body contains an indented block should render with
correct indentation. clice currently strips the leading indentation, so
an indented code block loses its offset and the blank line collapses.

````snap-hover
feature: hover
code: |
  namespace indented {
  /// Summary line.
  ///
  ///     step_one();
  ///     step_two();
  void §(01_indented)run();
  }
snapshot: |
  01_indented: { range: "14:5-14:8" }
  ### function `run`

  ---
  → `void`\
  Summary line.\
  step_one();\
  step_two();

  ---
  ```cpp
  // In namespace indented
  void run()
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1226 -->

**Template keyword from a macro**

The docstring should survive the expansion

When the `template` keyword is produced by a macro expansion, the
declaration's doc comment should still appear on hover. clice currently
drops it — the card carries no description.

````snap-hover
feature: hover
code: |
  int anchor = 0;

  #define TEMPLATE template

  /// A documented template function.
  TEMPLATE <typename T> void §(01_macro_template)run(T value);
snapshot: |
  01_macro_template: { range: "14:27-14:30" }
  ### function `run`

  ---
  → `void`\
  Parameters:\
  - `T value`

  ---
  ```cpp
  template <typename T> void run(T value)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2148 -->

**Comment suppression option**

A config switch to hide misattributed doc comments

A stray comment picked up by the association heuristic — a section
banner separated from the code by a blank line, for example — always
reaches the hover card: clice has no config option to suppress doc
comments whose attachment is a guess.

```snap-hover
feature: hover
code: |
  namespace suppression {
  // TODO: tidy this file up.

  int §(01_misattributed_note)counter;
  }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macro Hover

<!-- BEGIN GENERATED ITEMS: macro_hover -->

<!-- BEGIN CAPABILITY: supported -->

**Definition text at every site**

`#define`, use, `#ifdef` and `#undef` all show the macro's definition

A macro's hover card carries its `#define` text wherever the name
appears: the definition itself, a use, an `#ifdef` guard and an `#undef`.

````snap-hover
feature: hover
code: |
  int anchor = 0;

  #define §(01_define_site)LIMIT 64

  int use = §(02_use_site)LIMIT;

  #ifdef §(03_ifdef_site)LIMIT
  int guarded = 1;
  #endif

  #undef §(04_undef_site)LIMIT
snapshot: |
  01_define_site: { range: "9:8-9:13" }
  ### macro `LIMIT`

  ---
  ```cpp
  #define LIMIT 64
  ```

  02_use_site: { range: "11:10-11:15" }
  ### macro `LIMIT`

  ---
  ```cpp
  #define LIMIT 64

  // Expands to
  64
  ```

  03_ifdef_site: { range: "13:7-13:12" }
  ### macro `LIMIT`

  ---
  ```cpp
  #define LIMIT 64
  ```

  04_undef_site: { range: "17:7-17:12" }
  ### macro `LIMIT`

  ---
  ```cpp
  #define LIMIT 64
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Fully-expanded preview**

A function-like macro use shows its arguments substituted through the body

Hovering a function-like macro invocation shows the `#define` text and a
preview of the fully-expanded result with the call's arguments spliced in.

````snap-hover
feature: hover
code: |
  int x = 1, y = 2;

  #define MAX(a, b) ((a) > (b) ? (a) : (b))

  int z = §(01_expansion)MAX(x, y);
snapshot: |
  01_expansion: { range: "11:8-11:11" }
  ### macro `MAX`

  ---
  ```cpp
  #define MAX(a, b) ((a) > (b) ? (a) : (b))

  // Expands to
  ( ( x ) > ( y ) ? ( x ) : ( y ) )
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Command-line macros**

`-D` definitions hover with a synthesized `#define`

A macro defined on the command line (`-DFROM_CLI=7`) shows a synthesized
`#define FROM_CLI 7` in its hover card, then its expansion.

````snap-hover
feature: hover
code: |
  int cli = §(01_cli_use)FROM_CLI;
snapshot: |
  01_cli_use: { range: "8:10-8:18" }
  ### macro `FROM_CLI`

  ---
  ```cpp
  #define FROM_CLI 7

  // Expands to
  7
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Nested macro in arguments**

A macro named inside another invocation's arguments

The recorded expansion starts at the outer invocation, so hovering an
inner macro named inside the arguments shows only its definition, not an
expansion preview.

````snap-hover
feature: hover
code: |
  int anchor = 0;

  #define ECHO(x) x
  #define INNER_VAL 99

  int nested = ECHO(§(01_nested_arg)INNER_VAL);
snapshot: |
  01_nested_arg: { range: "13:18-13:27" }
  ### macro `INNER_VAL`

  ---
  ```cpp
  #define INNER_VAL 99
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2642 -->

**Use before definition**

Hovering a macro name that appears before its `#define`

A macro name used in an `#if` above its own `#define` should still hover
with the macro's definition. clice currently returns no hover at the
pre-definition use; a use after the `#define` works normally.

````snap-hover
feature: hover
code: |
  int anchor = 0;

  #if §(01_before_def)COUNT > 0
  int positive = 1;
  #endif

  #define COUNT 3

  int use = §(02_after_def)COUNT;
snapshot: |
  01_before_def: NO HOVER

  02_after_def: { range: "17:10-17:15" }
  ### macro `COUNT`

  ---
  ```cpp
  #define COUNT 3

  // Expands to
  3
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**`#define` inside the preamble**

Hover on a leading directive

A `#define` in the file's preamble region (the leading run of directives
before the first declaration) is not part of the live parse's
preprocessor record, so hovering its name yields nothing. Every other
macro fixture opens with a declaration precisely to push its directives
past the preamble boundary.

```snap-hover
feature: hover
code: |
  #define §(01_preamble_define)EARLY 1

  int use = EARLY;
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Special Hover Targets

<!-- BEGIN GENERATED ITEMS: special_hover_targets -->

<!-- BEGIN CAPABILITY: partial clangd#959 -->

**Members on type hover**

Hovering an enum or struct type lists its members

The card names the type (and a struct's layout), but the member list is
not expanded — the body renders as `{}`.

````snap-hover
feature: hover
code: |
  namespace members {

  enum Col§(enum_type)or {
      Red,
      Green,
      Blue,
  };

  struct Poi§(struct_type)nt {
      int x;
      int y;
  };

  }
snapshot: |
  enum_type: { range: "10:5-10:10" }
  ### enum `Color`

  ---
  ```cpp
  // In namespace members
  enum Color {}
  ```

  struct_type: { range: "16:7-16:12" }
  ### struct `Point`

  ---
  Size: 8 bytes, alignment 4 bytes

  ---
  ```cpp
  // In namespace members
  struct Point {}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#2020 -->

**Typedef underlying struct**

Hovering an alias expands the aliased definition

The card resolves the alias to its underlying type name, but does not
expand that struct's definition or member list.

````snap-hover
feature: hover
code: |
  namespace aliases {

  struct Widget {
      int id;
      double value;
  };

  using Han§(alias_using)dle = Widget;

  typedef Widget Wid§(alias_typedef)get_t;

  }
snapshot: |
  alias_typedef: { range: "17:15-17:23" }
  ### type `Widget_t`

  ---
  Type: `struct aliases::Widget`

  ---
  ```cpp
  // In namespace aliases
  typedef Widget Widget_t
  ```

  alias_using: { range: "15:6-15:12" }
  ### type `Handle`

  ---
  Type: `struct aliases::Widget`

  ---
  ```cpp
  // In namespace aliases
  using Handle = Widget
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1862 -->

**Keyword documentation**

Hovering a language keyword shows its description

Hovering a keyword such as `const` or `virtual` produces no card.

```snap-hover
feature: hover
code: |
  namespace keywords {

  co§(const_kw)nst int limit = 42;

  struct Widget {
      vir§(virtual_kw)tual void draw();
  };

  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1862 -->

**Attribute documentation**

Hovering an attribute shows its description

The attribute's own documentation renders in the card, for both GNU
`__attribute__` spellings and C++ `[[...]]` attributes.

````snap-hover
feature: hover
code: |
  namespace attr_docs {
  void foo(int * __attribute__((non§(gnu_attribute)null, noescape)) );

  [[nodi§(std_attribute)scard]] int compute();
  }
snapshot: |
  gnu_attribute: { range: "12:30-12:37" }
  ### `nonnull`

  ---
  The \`\`nonnull\`\` attribute indicates that some function parameters must not be null, and can be used in several different ways. It's original usage (\`from GCC <https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#Common-Function-Attributes>\`\_) is as a function (or Objective-C method) attribute that specifies which parameters of the function are nonnull in a comma-separated list. For example:\
  .. code-block:: c\
  extern void * my_memcpy (void \*dest, const void \*src, size_t len) \_\_attribute\_\_((nonnull (1, 2)));\
  Here, the \`\`nonnull\`\` attribute indicates that parameters 1 and 2 cannot have a null value. Omitting the parenthesized list of parameter indices means that all parameters of pointer type cannot be null:\
  .. code-block:: c\
  extern void * my_memcpy (void \*dest, const void \*src, size_t len) \_\_attribute\_\_((nonnull));\
  Clang also allows the \`\`nonnull\`\` attribute to be placed directly on a function (or Objective-C method) parameter, eliminating the need to specify the parameter index ahead of type. For example:\
  .. code-block:: c\
  extern void * my_memcpy (void \*dest \_\_attribute\_\_((nonnull)),\
  const void \*src \_\_attribute\_\_((nonnull)), size_t len);\
  Note that the \`\`nonnull\`\` attribute indicates that passing null to a non-null parameter is undefined behavior, which the optimizer may take advantage of to,\
  e.g., remove null checks. The \`\`\_Nonnull\`\` type qualifier indicates that a pointer cannot be null in a more general manner (because it is part of the type system) and does not imply undefined behavior, making it more widely applicable.

  ---
  ```cpp
  __attribute__((nonnull))
  ```

  std_attribute: { range: "14:2-14:11" }
  ### `nodiscard`

  ---
  Clang supports the ability to diagnose when the results of a function call expression are discarded under suspicious circumstances. A diagnostic is generated when a function or its return type is marked with \`\`[[nodiscard]]\`\` (or \`\`\_\_attribute\_\_((warn_unused_result))\`\`) and the function call appears as a potentially-evaluated discarded-value expression that is not explicitly cast to\
  \`\`void\`\`.\
  A string literal may optionally be provided to the attribute, which will be reproduced in any resulting diagnostics. Redeclarations using different forms of the attribute (with or without the string literal or with different string literal contents) are allowed. If there are redeclarations of the entity with differing string literals, it is unspecified which one will be used by Clang in any resulting diagnostics.\
  .. code-block:: c++\
  struct [[nodiscard]] error_info { /\*...\*/ };\
  error_info enable_missile_safety_mode();\
  void launch_missiles();\
  void test_missiles() { enable_missile_safety_mode(); // diagnoses launch_missiles();\
  } error_info &foo();\
  void f() { foo(); } // Does not diagnose, error_info is a reference.\
  Additionally, discarded temporaries resulting from a call to a constructor marked with \`\`[[nodiscard]]\`\` or a constructor of a type marked\
  \`\`[[nodiscard]]\`\` will also diagnose. This also applies to type conversions that use the annotated \`\`[[nodiscard]]\`\` constructor or result in an annotated type.\
  .. code-block:: c++\
  struct [[nodiscard]] marked_type {/\*..\*/ };\
  struct marked_ctor { [[nodiscard]] marked_ctor();\
  marked_ctor(int);\
  };\
  struct S { operator marked_type() const;\
  [[nodiscard]] operator int() const;\
  };\
  void usages() { marked_type(); // diagnoses.\
  marked_ctor(); // diagnoses.\
  marked_ctor(3); // Does not diagnose, int constructor isn't marked nodiscard.\
  S s;\
  static_cast\<marked_type>(s); // diagnoses (int)s; // diagnoses }

  ---
  ```cpp
  [[nodiscard("")]]
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Include directive hover**

Hovering an `#include` shows the resolved header path

The card resolves the quoted header to its file on disk.

````snap-hover
feature: hover
code: |
  #include "own_h§(include_path)eader.h"

  int use = own_header_value;
snapshot: |
  include_path: { range: "7:9-7:23" }
  ### `own_header.h`

  ---
  ```cpp
  ${WS}/own_header.h
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`this` expression**

Hovering `this` shows the pointed-to class type

Works in a plain class and inside a class template.

````snap-hover
feature: hover
code: |
  namespace this_hover {

  struct Widget {
      Widget* self() {
          return th§(plain_this)is;
      }
  };

  template <typename T>
  struct Box {
      const Box* self() const {
          return th§(template_this)is;
      }
  };

  }
snapshot: |
  plain_this: { range: "10:15-10:19" }
  ### `this`

  ---
  ```cpp
  this_hover::Widget *
  ```

  template_this: { range: "17:15-17:19" }
  ### `this`

  ---
  ```cpp
  const this_hover::Box<T> *
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Predefined identifiers**

`__func__` hover shows the current function name

The value resolves in a concrete function; inside a template only the
approximate type is known.

```snap-hover
feature: hover
code: |
  namespace predefined {

  void current() {
      const char* name = __f§(func_name)unc__;
  }

  template <int N>
  void generic() {
      const char* name = __f§(func_dependent)unc__;
  }

  }
snapshot: |
  func_dependent: { range: "15:23-15:31" }
  ### variable `__func__`

  ---
  Type: `const char[]`\
  Name of the current function (predefined variable)

  func_name: { range: "10:23-10:31" }
  ### variable `__func__`

  ---
  Type: `const char[8]`\
  Value = `"current"`\
  Name of the current function (predefined variable)
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**No hover on meaningless tokens**

Builtin keywords and empty bodies yield no card

Hovering a builtin type keyword or the inside of an empty body
produces no card at all, so editors show nothing rather than noise.
(Numeric and bool literals also have no card today, but that is a
tracked gap — see the numeric-literal item — not a promise.)

```snap-hover
feature: hover
code: |
  namespace negatives {

  §(builtin_type)int counter = 0;

  void noop() {§(empty_braces)}

  }
snapshot: |
  builtin_type: NO HOVER

  empty_braces: NO HOVER
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2662 -->

**GTK-Doc and kernel-doc**

Recognize GObject Introspection annotations

GTK-Doc / kernel-doc comment syntax and GObject Introspection
annotations are not parsed into the hover card.

```snap-hover
feature: hover
code: |
  /**
   * gtk_widget_show:
   * @widget: (transfer none): a #GtkWidget
   *
   * Flags a widget to be displayed.
   */
  void gtk_widget_show(GtkWidget *widget);
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2669 -->

**LaTeX math in Doxygen**

Render `@f$ ... @f$` formulas

Doxygen LaTeX math formulas are shown verbatim, not rendered as math.

```snap-hover
feature: hover
code: |
  /// The area of a circle is @f$ A = \pi r^2 @f$.
  double circle_area(double r);
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: presentation -->

<!-- BEGIN CAPABILITY: supported -->

**Markdown rendering**

Cards render as markdown, or plain text via `parse_comment_as_markdown = false`

````snap-hover
feature: hover
code: |
  /// Computes the answer. Tests primality of `p`.
  constexpr int an§(function)swer(int p) {
      return p + 41;
  }

  int va§(variable)lue = answer(1);

  struct Layout {
      char first;
      int seco§(field)nd;
  };
snapshot: |
  default:
    field: { range: "18:8-18:14" }
    ### field `second`

    ---
    Type: `int`\
    Offset: 4 bytes\
    Size: 4 bytes, alignment 4 bytes

    ---
    ```cpp
    // In Layout
    public: int second
    ```

    function: { range: "10:14-10:20" }
    ### function `answer`

    ---
    → `int`\
    Parameters:\
    - `int p`

    Computes the answer. Tests primality of `p`.

    ---
    ```cpp
    constexpr int answer(int p)
    ```

    variable: { range: "14:4-14:9" }
    ### variable `value`

    ---
    Type: `int`\
    Value = `42 (0x2a)`

    ---
    ```cpp
    int value = answer(1)
    ```

  configured:
    field: { range: "18:8-18:14" }
    field second

    Type: int
    Offset: 4 bytes
    Size: 4 bytes, alignment 4 bytes

    // In Layout
    public: int second

    function: { range: "10:14-10:20" }
    function answer

    → int
    Parameters:
    - int p
    Computes the answer. Tests primality of `p`.

    constexpr int answer(int p)

    variable: { range: "14:4-14:9" }
    variable value

    Type: int
    Value = 42 (0x2a)

    int value = answer(1)
````

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module-Related

<!-- BEGIN GENERATED ITEMS: module_related -->

<!-- BEGIN CAPABILITY: unsupported -->

**Import statement hover**

Hovering `import` shows the module's info

Hovering an `import` declaration does not yet describe the imported
module.

```snap-hover
feature: hover
code: |
  export module app;

  import utils;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module name hover**

Hovering a module name lists its owning files

Hovering a module name does not yet list the files or partitions that
declare it.

```snap-hover
feature: hover
code: |
  export module math;

  export module math:algebra;
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Hover Correctness

Robustness on inputs that have broken other tooling.

<!-- BEGIN GENERATED ITEMS: hover_correctness -->

<!-- BEGIN CAPABILITY: supported -->

**MSVC inheritance model**

`MSInheritanceAttr` does not corrupt record hover

clangd tracks this as clangd#1643 and clangd#2212; under an MSVC target
the implicit inheritance attribute does not leak into the record or
method card.

````snap-hover
feature: hover
code: |
  namespace ms {

  struct Wid§(struct_hover)get {
      int value;
      void up§(method_hover)date();
  };

  int Widget::* member = &Widget::value;

  }
snapshot: |
  method_hover: { range: "13:9-13:15" }
  ### method `update`

  ---
  → `void`

  ---
  ```cpp
  // In Widget
  public: void update()
  ```

  struct_hover: { range: "11:7-11:13" }
  ### struct `Widget`

  ---
  ```cpp
  // In namespace ms
  struct Widget {}
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Most-vexing-parse**

Object init and function declaration hover distinctly

clangd tracks this as clangd#2225; clice reads the direct-init as a
variable and the vexing form as a function declaration.

````snap-hover
feature: hover
code: |
  namespace mvp {

  struct Timer {
      Timer();
      Timer(int);
  };

  int seconds = 5;

  void demo() {
      Timer act§(object_init)ive(seconds);
      Timer emp§(vexing_decl)ty();
  }

  }
snapshot: |
  object_init: { range: "17:10-17:16" }
  ### variable `active`

  ---
  Type: `Timer`

  ---
  ```cpp
  // In demo
  Timer active(seconds)
  ```

  vexing_decl: { range: "18:10-18:15" }
  ### function `empty`

  ---
  → `Timer`

  ---
  ```cpp
  // In namespace mvp
  Timer empty()
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Large unsigned enum constant**

Hovering a `0xFFFF...ULL` enumerator does not crash

clangd crashes on this (clangd#2381); clice renders the full unsigned
value without overflow.

````snap-hover
feature: hover
code: |
  namespace big_enum {

  enum class Flags : unsigned long long {
      Ma§(max_value)x = 0xFFFFFFFFFFFFFFFFULL,
  };

  }
snapshot: |
  max_value: { range: "10:4-10:7" }
  ### enumerator `Max`

  ---
  Type: `enum big_enum::Flags`\
  Value = `18446744073709551615`

  ---
  ```cpp
  // In Flags
  Max = 0xFFFFFFFFFFFFFFFFULL
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Call with default arguments**

Hovering a call that omits defaults does not crash

clangd crashes on this (clangd#551); clice renders the callee signature
with its default arguments.

````snap-hover
feature: hover
code: |
  namespace defaults {

  int compute(int a, int b = 10, int c = 20);

  int result = comp§(call_site)ute(1);

  }
snapshot: |
  call_site: { range: "11:13-11:20" }
  ### function `compute`

  ---
  → `int`\
  Parameters:\
  - `int a`
  - `int b = 10`
  - `int c = 20`

  ---
  ```cpp
  // In namespace defaults
  int compute(int a, int b = 10, int c = 20)
  ```
````

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Macro-shadowed symbol**

A function-like macro over a same-named function

clangd tracks this as clangd#2490; at the call site the function-like
macro is active, and clice's card shows that macro and its expansion.

````snap-hover
feature: hover
code: |
  namespace shadow {

  int lookup(int key) {
      return key;
  }

  }

  #define lookup(key) ((key) + 100)

  int value = loo§(shadowed_use)kup(5);
snapshot: |
  shadowed_use: { range: "17:12-17:18" }
  ### macro `lookup`

  ---
  ```cpp
  #define lookup(key) ((key) + 100)

  // Expands to
  ( ( 5 ) + 100 )
  ```
````

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
