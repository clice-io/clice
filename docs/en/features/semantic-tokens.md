# Semantic Tokens

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/semantic_tokens/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture spec headers and run
     `node tools/docs/feature.ts update`. -->

clice classifies every token of a document with its own token-kind vocabulary,
which is richer than the standard LSP token types and consistent across all
clice replies. Clients that prefer standard LSP kinds can map them through
configuration.

## Lexical Tokens

Kinds derived from the token stream itself, independent of the AST.

<!-- BEGIN GENERATED ITEMS: lexical_tokens -->

<!-- BEGIN CAPABILITY: supported -->

**Comments**

line, block and doc comments, including multiline blocks

```snap-semantic_tokens
feature: semantic_tokens
code: |
  // A line comment.
  /* a one-line block comment */
  /*
   * a block comment
   * spanning several lines
   */
  /// a doc comment
  int after_comments = 0;

  /* first
  second */ int after_block = 1;
snapshot: |
  - { loc: "0:0", text: "/// # Comments — line, block and doc comments, including multiline blocks", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "4:0", text: "// A line comment.", kind: comment }
  - { loc: "5:0", text: "/* a one-line block comment */", kind: comment }
  - { loc: "6:0", text: "/*", kind: comment }
  - { loc: "7:0", text: " * a block comment", kind: comment }
  - { loc: "8:0", text: " * spanning several lines", kind: comment }
  - { loc: "9:0", text: " */", kind: comment }
  - { loc: "10:0", text: "/// a doc comment", kind: comment }
  - { loc: "11:0", text: "int", kind: primitive }
  - { loc: "11:4", text: "after_comments", kind: variable, modifiers: [definition] }
  - { loc: "11:21", text: "0", kind: number }
  - { loc: "13:0", text: "/* first", kind: comment }
  - { loc: "14:0", text: "second */", kind: comment }
  - { loc: "14:10", text: "int", kind: primitive }
  - { loc: "14:14", text: "after_block", kind: variable, modifiers: [definition] }
  - { loc: "14:28", text: "1", kind: number }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Literals**

numbers, characters and strings, including raw strings

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int decimal = 42;
  int hexadecimal = 0xFF;
  double floating = 3.14;
  char letter = 'x';
  const char* text = "hello";
  const char* raw = R"(no "escapes" in here)";
  int after_raw = 1;

  const char* multiline = R"(line1
  line2
  )"; int after_closing = 2;
snapshot: |
  - { loc: "0:0", text: "/// # Literals — numbers, characters and strings, including raw strings", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "4:0", text: "int", kind: primitive }
  - { loc: "4:4", text: "decimal", kind: variable, modifiers: [definition] }
  - { loc: "4:14", text: "42", kind: number }
  - { loc: "5:0", text: "int", kind: primitive }
  - { loc: "5:4", text: "hexadecimal", kind: variable, modifiers: [definition] }
  - { loc: "5:18", text: "0xFF", kind: number }
  - { loc: "6:0", text: "double", kind: primitive }
  - { loc: "6:7", text: "floating", kind: variable, modifiers: [definition] }
  - { loc: "6:18", text: "3.14", kind: number }
  - { loc: "7:0", text: "char", kind: primitive }
  - { loc: "7:5", text: "letter", kind: variable, modifiers: [definition] }
  - { loc: "7:14", text: "'x'", kind: character }
  - { loc: "8:0", text: "const", kind: keyword }
  - { loc: "8:6", text: "char", kind: primitive }
  - { loc: "8:12", text: "text", kind: variable, modifiers: [definition, readonly] }
  - { loc: "8:19", text: "\"hello\"", kind: string }
  - { loc: "9:0", text: "const", kind: keyword }
  - { loc: "9:6", text: "char", kind: primitive }
  - { loc: "9:12", text: "raw", kind: variable, modifiers: [definition, readonly] }
  - { loc: "9:18", text: "R\"(no \"escapes\" in here)\"", kind: string }
  - { loc: "10:0", text: "int", kind: primitive }
  - { loc: "10:4", text: "after_raw", kind: variable, modifiers: [definition] }
  - { loc: "10:16", text: "1", kind: number }
  - { loc: "12:0", text: "const", kind: keyword }
  - { loc: "12:6", text: "char", kind: primitive }
  - { loc: "12:12", text: "multiline", kind: variable, modifiers: [definition, readonly] }
  - { loc: "12:24", text: "R\"(line1", kind: string }
  - { loc: "13:0", text: "line2", kind: string }
  - { loc: "14:0", text: ")\"", kind: string }
  - { loc: "14:4", text: "int", kind: primitive }
  - { loc: "14:8", text: "after_closing", kind: variable, modifiers: [definition] }
  - { loc: "14:24", text: "2", kind: number }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keywords**

Including alternative operator spellings and the contextual `final` / `override`

```snap-semantic_tokens
feature: semantic_tokens
code: |
  bool logic(bool a, bool b) {
      return a §and b §or §not a;
  }

  struct Base {
      virtual void act();
      virtual ~Base();
  };

  struct Leaf §final : Base {
      void act() §override;
  };

  struct Last : Base {
      void act() §final;
  };
snapshot: |
  - { loc: "5:13", text: "and", kind: keyword }
  - { loc: "5:19", text: "or", kind: keyword }
  - { loc: "5:22", text: "not", kind: keyword }
  - { loc: "13:12", text: "final", kind: keyword }
  - { loc: "14:15", text: "override", kind: keyword }
  - { loc: "18:15", text: "final", kind: keyword }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Preprocessor directives**

`#if` chains keep directive kinds; disabled branches keep lexical kinds; pragma arguments stay plain

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int before_conditional = 0;

  #if 0
  int disabled_branch;
  #else
  int enabled_branch = 1;
  #endif

  #define FLAG
  #ifdef FLAG
  int flagged = 2;
  #endif

  #pragma pack(1)

  #
  #define STRINGIZE(x) #x
  const char* stringized = STRINGIZE(abc);
snapshot: |
  - { loc: "0:0", text: "/// # Preprocessor directives — `#if` chains keep directive kinds; disabled branches keep lexical kinds; pragma arguments stay plain", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "4:0", text: "int", kind: primitive }
  - { loc: "4:4", text: "before_conditional", kind: variable, modifiers: [definition] }
  - { loc: "4:25", text: "0", kind: number }
  - { loc: "6:0", text: "#if", kind: directive }
  - { loc: "6:4", text: "0", kind: number }
  - { loc: "7:0", text: "int", kind: primitive, modifiers: [inactive] }
  - { loc: "7:4", text: "disabled_branch;", kind: identifier, modifiers: [inactive] }
  - { loc: "8:0", text: "#else", kind: directive }
  - { loc: "9:0", text: "int", kind: primitive }
  - { loc: "9:4", text: "enabled_branch", kind: variable, modifiers: [definition] }
  - { loc: "9:21", text: "1", kind: number }
  - { loc: "10:0", text: "#endif", kind: directive }
  - { loc: "12:0", text: "#define", kind: directive }
  - { loc: "12:8", text: "FLAG", kind: macro, modifiers: [definition] }
  - { loc: "13:0", text: "#ifdef", kind: directive }
  - { loc: "13:7", text: "FLAG", kind: macro }
  - { loc: "14:0", text: "int", kind: primitive }
  - { loc: "14:4", text: "flagged", kind: variable, modifiers: [definition] }
  - { loc: "14:14", text: "2", kind: number }
  - { loc: "15:0", text: "#endif", kind: directive }
  - { loc: "17:0", text: "#pragma", kind: directive }
  - { loc: "17:13", text: "1", kind: number }
  - { loc: "19:0", text: "#", kind: directive }
  - { loc: "20:0", text: "#define", kind: directive }
  - { loc: "20:8", text: "STRINGIZE", kind: macro, modifiers: [definition] }
  - { loc: "21:0", text: "const", kind: keyword }
  - { loc: "21:6", text: "char", kind: primitive }
  - { loc: "21:12", text: "stringized", kind: variable, modifiers: [definition, readonly] }
  - { loc: "21:25", text: "STRINGIZE", kind: macro }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive regions**

Tokens in untaken branches keep their lexical kinds and carry the `inactive` modifier; unclassified tokens become plain `identifier` carriers, so even a lone `}` line dims

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int before = 0;

  #if 0
  int simple = 1;
  bare identifiers;
  call(arg);
  "string in dead code";
  // comment inside
  #ifdef NESTED
  int deeper = 2;
  #endif
  int tail = 3;
  #endif

  #if defined(MISSING)
  first_branch;
  #elif 0
  elif_branch;
  #else
  int taken = 4;
  #endif

  #if 0
  void edge() {
      inner(5);
  }
  #endif
snapshot: |
  - { loc: "0:0", text: "/// # Inactive regions — tokens in untaken branches keep their lexical kinds and carry the `inactive` modifier; unclassified tokens become plain `identifier` carriers, so even a lone `}` line dims", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "4:0", text: "int", kind: primitive }
  - { loc: "4:4", text: "before", kind: variable, modifiers: [definition] }
  - { loc: "4:13", text: "0", kind: number }
  - { loc: "6:0", text: "#if", kind: directive }
  - { loc: "6:4", text: "0", kind: number }
  - { loc: "7:0", text: "int", kind: primitive, modifiers: [inactive] }
  - { loc: "7:4", text: "simple", kind: identifier, modifiers: [inactive] }
  - { loc: "7:11", text: "=", kind: identifier, modifiers: [inactive] }
  - { loc: "7:13", text: "1", kind: number, modifiers: [inactive] }
  - { loc: "7:14", text: ";", kind: identifier, modifiers: [inactive] }
  - { loc: "8:0", text: "bare", kind: identifier, modifiers: [inactive] }
  - { loc: "8:5", text: "identifiers;", kind: identifier, modifiers: [inactive] }
  - { loc: "9:0", text: "call(arg);", kind: identifier, modifiers: [inactive] }
  - { loc: "10:0", text: "\"string in dead code\"", kind: string, modifiers: [inactive] }
  - { loc: "10:21", text: ";", kind: identifier, modifiers: [inactive] }
  - { loc: "11:0", text: "// comment inside", kind: comment, modifiers: [inactive] }
  - { loc: "12:0", text: "#ifdef", kind: directive, modifiers: [inactive] }
  - { loc: "12:7", text: "NESTED", kind: identifier, modifiers: [inactive] }
  - { loc: "13:0", text: "int", kind: primitive, modifiers: [inactive] }
  - { loc: "13:4", text: "deeper", kind: identifier, modifiers: [inactive] }
  - { loc: "13:11", text: "=", kind: identifier, modifiers: [inactive] }
  - { loc: "13:13", text: "2", kind: number, modifiers: [inactive] }
  - { loc: "13:14", text: ";", kind: identifier, modifiers: [inactive] }
  - { loc: "14:0", text: "#endif", kind: directive, modifiers: [inactive] }
  - { loc: "15:0", text: "int", kind: primitive, modifiers: [inactive] }
  - { loc: "15:4", text: "tail", kind: identifier, modifiers: [inactive] }
  - { loc: "15:9", text: "=", kind: identifier, modifiers: [inactive] }
  - { loc: "15:11", text: "3", kind: number, modifiers: [inactive] }
  - { loc: "15:12", text: ";", kind: identifier, modifiers: [inactive] }
  - { loc: "16:0", text: "#endif", kind: directive }
  - { loc: "18:0", text: "#if", kind: directive }
  - { loc: "19:0", text: "first_branch;", kind: identifier, modifiers: [inactive] }
  - { loc: "20:0", text: "#elif", kind: directive }
  - { loc: "20:6", text: "0", kind: number }
  - { loc: "21:0", text: "elif_branch;", kind: identifier, modifiers: [inactive] }
  - { loc: "22:0", text: "#else", kind: directive }
  - { loc: "23:0", text: "int", kind: primitive }
  - { loc: "23:4", text: "taken", kind: variable, modifiers: [definition] }
  - { loc: "23:12", text: "4", kind: number }
  - { loc: "24:0", text: "#endif", kind: directive }
  - { loc: "26:0", text: "#if", kind: directive }
  - { loc: "26:4", text: "0", kind: number }
  - { loc: "27:0", text: "void", kind: primitive, modifiers: [inactive] }
  - { loc: "27:5", text: "edge()", kind: identifier, modifiers: [inactive] }
  - { loc: "27:12", text: "{", kind: identifier, modifiers: [inactive] }
  - { loc: "28:4", text: "inner(", kind: identifier, modifiers: [inactive] }
  - { loc: "28:10", text: "5", kind: number, modifiers: [inactive] }
  - { loc: "28:11", text: ");", kind: identifier, modifiers: [inactive] }
  - { loc: "29:0", text: "}", kind: identifier, modifiers: [inactive] }
  - { loc: "30:0", text: "#endif", kind: directive }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Header names**

Quoted and angled `#include` filenames, including the split `# include` form

```snap-semantic_tokens
feature: semantic_tokens
code: |
  #include "inc/angled.h"
  #include <angled.h>
  # include "inc/angled.h"

  int after_includes = 0;
snapshot: |
  - { loc: "0:0", text: "/// # Header names — quoted and angled `#include` filenames, including the split `# include` form", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "3:0", text: "/// - flags: [\"-I${corpus}\"]", kind: comment }
  - { loc: "5:0", text: "#include", kind: directive }
  - { loc: "5:9", text: "\"inc/angled.h\"", kind: header }
  - { loc: "6:0", text: "#include", kind: directive }
  - { loc: "6:9", text: "<angled.h>", kind: header }
  - { loc: "7:0", text: "#", kind: directive }
  - { loc: "7:2", text: "include", kind: directive }
  - { loc: "7:10", text: "\"inc/angled.h\"", kind: header }
  - { loc: "9:0", text: "int", kind: primitive }
  - { loc: "9:4", text: "after_includes", kind: variable, modifiers: [definition] }
  - { loc: "9:21", text: "0", kind: number }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Inactive regions at the top of a file**

Untaken branches among the leading directives dim the same way

```snap-semantic_tokens
feature: semantic_tokens
code: |
  #define KEEP 1
  #if 0
  #define DEAD 2
  #endif

  int after = KEEP;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Literal prefixes and suffixes**

Encoding prefixes, type suffixes, digit separators and UDL suffixes as distinct tokens

```snap-semantic_tokens
feature: semantic_tokens
code: |
  using size_type = decltype(sizeof(0));
  constexpr size_type operator""_kb(unsigned long long n) {
      return n * 1024;
  }

  auto wide = L"wide string";
  auto utf8 = u8"utf-8 string";
  auto hex = 0xFF;
  auto binary = 0b1010;
  auto unsigned_suffix = 42u;
  auto float_suffix = 3.14f;
  auto separators = 1'000'000;
  auto udl = 4_kb;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Escape sequences**

Highlighted distinctly inside string and character literals

```snap-semantic_tokens
feature: semantic_tokens
code: |
  const char* escaped = "hello\nworld";
  char hex_escape = '\x41';
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1421 -->

**Declarator vs operator disambiguation**

`*`, `&`, `&&` as declarators vs arithmetic/logical operators

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int value = 1;
  int* pointer = &value;
  int& reference = value;
  int product = value * value;
  int masked = value & 1;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Primitive token type**

A distinct kind for built-in types instead of plain `keyword`

```snap-semantic_tokens
feature: semantic_tokens
code: |
  §int number = 0;
  §float ratio = 0.5f;
  §void act();
  §unsigned §long §long wide_number = 0;
  §__int128 extended_int = 0;
  §_Float16 extended_float = 0;
snapshot: |
  - { loc: "4:0", text: "int", kind: primitive }
  - { loc: "5:0", text: "float", kind: primitive }
  - { loc: "6:0", text: "void", kind: primitive }
  - { loc: "7:0", text: "unsigned", kind: primitive }
  - { loc: "7:9", text: "long", kind: primitive }
  - { loc: "7:14", text: "long", kind: primitive }
  - { loc: "8:0", text: "__int128", kind: primitive }
  - { loc: "9:0", text: "_Float16", kind: primitive }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Bracket token types**

Matching `()`, `[]`, `{}`, `<>` pairs as distinct kinds

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  struct Grid {
      T cells[4];
  };

  Grid<int> grid{{1, 2, 3, 4}};

  int first(Grid<int>& grid) {
      return grid.cells[0];
  }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Declarations & References

Names classified by the declaration they define or reference.

<!-- BEGIN GENERATED ITEMS: declarations_references -->

<!-- BEGIN CAPABILITY: supported -->

**Namespaces**

definitions, references, nested namespaces and namespace aliases

```snap-semantic_tokens
feature: semantic_tokens
code: |
  namespace §demo {
  namespace §inner {
  int value = 1;
  }
  }

  namespace demo::inner::§more {}

  namespace §alias = §demo::§inner;

  int use_alias = §alias::§value;
snapshot: |
  - { loc: "4:10", text: "demo", kind: namespace, modifiers: [definition] }
  - { loc: "5:10", text: "inner", kind: namespace, modifiers: [definition] }
  - { loc: "10:23", text: "more", kind: namespace, modifiers: [definition] }
  - { loc: "12:10", text: "alias", kind: namespace, modifiers: [definition] }
  - { loc: "12:18", text: "demo", kind: namespace }
  - { loc: "12:24", text: "inner", kind: namespace }
  - { loc: "14:16", text: "alias", kind: namespace }
  - { loc: "14:23", text: "value", kind: variable }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Types**

class, struct, union, enum and type aliases, at definitions and references

```snap-semantic_tokens
feature: semantic_tokens
code: |
  class §Widget {};
  struct §Point {};
  union §Storage {
      int i;
      float f;
  };
  enum §Flags { FlagA };
  enum class §Mode { Fast };

  typedef §Point §PointAlias;
  using §WidgetAlias = §Widget;

  §Widget* make_widget();
  §PointAlias origin;
  §Mode current = §Mode::Fast;
snapshot: |
  - { loc: "4:6", text: "Widget", kind: class, modifiers: [definition] }
  - { loc: "5:7", text: "Point", kind: struct, modifiers: [definition] }
  - { loc: "6:6", text: "Storage", kind: union, modifiers: [definition] }
  - { loc: "10:5", text: "Flags", kind: enum, modifiers: [definition] }
  - { loc: "11:11", text: "Mode", kind: enum, modifiers: [definition] }
  - { loc: "13:8", text: "Point", kind: struct }
  - { loc: "13:14", text: "PointAlias", kind: type, modifiers: [definition] }
  - { loc: "14:6", text: "WidgetAlias", kind: type, modifiers: [definition] }
  - { loc: "14:20", text: "Widget", kind: class }
  - { loc: "16:0", text: "Widget", kind: class }
  - { loc: "17:0", text: "PointAlias", kind: type }
  - { loc: "18:0", text: "Mode", kind: enum }
  - { loc: "18:15", text: "Mode", kind: enum }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functions and methods**

declarations, definitions and call sites

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int §twice(int value);

  int §twice(int value) {
      return value * 2;
  }

  struct Machine {
      void §start();
      static void §reset();
  };

  void drive(Machine machine) {
      machine.§start();
      Machine::§reset();
      int four = §twice(2);
  }
snapshot: |
  - { loc: "4:4", text: "twice", kind: function, modifiers: [declaration] }
  - { loc: "6:4", text: "twice", kind: function, modifiers: [definition] }
  - { loc: "11:9", text: "start", kind: method, modifiers: [declaration] }
  - { loc: "12:16", text: "reset", kind: method, modifiers: [declaration, static] }
  - { loc: "16:12", text: "start", kind: method }
  - { loc: "17:13", text: "reset", kind: method, modifiers: [static] }
  - { loc: "18:15", text: "twice", kind: function }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variables**

globals, locals, parameters, fields and enum members

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Holder {
      int §field;
      static int §shared;
  };

  enum class State { §Idle };

  int §global_value = 1;

  void touch(int §param) {
      int §local = §param + §global_value;
      Holder h;
      h.§field = §local;
      Holder::§shared = h.§field;
      State state = State::§Idle;
  }
snapshot: |
  - { loc: "5:8", text: "field", kind: field, modifiers: [definition] }
  - { loc: "6:15", text: "shared", kind: variable, modifiers: [declaration, static] }
  - { loc: "9:19", text: "Idle", kind: enumMember, modifiers: [definition, readonly] }
  - { loc: "11:4", text: "global_value", kind: variable, modifiers: [definition] }
  - { loc: "13:15", text: "param", kind: parameter, modifiers: [definition] }
  - { loc: "14:8", text: "local", kind: variable, modifiers: [definition] }
  - { loc: "14:16", text: "param", kind: parameter }
  - { loc: "14:24", text: "global_value", kind: variable }
  - { loc: "16:6", text: "field", kind: field }
  - { loc: "16:14", text: "local", kind: variable }
  - { loc: "17:12", text: "shared", kind: variable, modifiers: [static] }
  - { loc: "17:23", text: "field", kind: field }
  - { loc: "18:25", text: "Idle", kind: enumMember, modifiers: [readonly] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Templates**

Type and non-type template parameters, with the `templated` modifier on template names

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename §T, int §N>
  struct §Array {
      §T data[§N];
  };

  template <typename T>
  T §identity(T value);

  template <typename §T>
  §T §identity(§T value) {
      return value;
  }

  §Array<int, 4> arr;
  int result = §identity(3);
snapshot: |
  - { loc: "4:19", text: "T", kind: type, modifiers: [definition] }
  - { loc: "4:26", text: "N", kind: variable, modifiers: [definition, readonly] }
  - { loc: "5:7", text: "Array", kind: struct, modifiers: [definition, templated] }
  - { loc: "6:4", text: "T", kind: type }
  - { loc: "6:11", text: "N", kind: variable, modifiers: [readonly] }
  - { loc: "10:2", text: "identity", kind: function, modifiers: [declaration, templated] }
  - { loc: "12:19", text: "T", kind: type, modifiers: [definition] }
  - { loc: "13:0", text: "T", kind: type }
  - { loc: "13:2", text: "identity", kind: function, modifiers: [definition, templated] }
  - { loc: "13:11", text: "T", kind: type }
  - { loc: "17:0", text: "Array", kind: struct }
  - { loc: "18:13", text: "identity", kind: function }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Concepts**

Definitions and uses as template constraints

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  concept §Small = sizeof(T) <= 4;

  template <§Small T>
  void use_small(T value);

  template <typename T>
      requires §Small<T>
  void require_small(T value);
snapshot: |
  - { loc: "5:8", text: "Small", kind: concept, modifiers: [definition, templated] }
  - { loc: "7:10", text: "Small", kind: concept, modifiers: [templated] }
  - { loc: "11:13", text: "Small", kind: concept, modifiers: [templated] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Labels**

`goto` targets and label definitions

```snap-semantic_tokens
feature: semantic_tokens
code: |
  void retry(bool again) {
      goto §done;
  §done:
      if (again) {
          goto done;
      }
  }
snapshot: |
  - { loc: "5:9", text: "done", kind: label }
  - { loc: "6:0", text: "done", kind: label, modifiers: [definition] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Structured bindings**

Binding names at definition and use

The opening `[` deliberately carries no token; only the binding names
themselves are highlighted.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Pair {
      int first, second;
  };

  void unpack() {
      auto §[§a, §b] = Pair{1, 2};
      int sum = §a + §b;
  }
snapshot: |
  - { loc: "12:9", text: "[", kind: none }
  - { loc: "12:10", text: "a", kind: variable, modifiers: [definition] }
  - { loc: "12:13", text: "b", kind: variable, modifiers: [definition] }
  - { loc: "13:14", text: "a", kind: variable }
  - { loc: "13:18", text: "b", kind: variable }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#122 -->

**Member initializer lists**

Initialized fields highlighted as fields

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Widget {
      int width;
      int height;

      Widget(int w, int h) : §width(§w), §height(h) {}
  };
snapshot: |
  - { loc: "9:27", text: "width", kind: field }
  - { loc: "9:33", text: "w", kind: parameter }
  - { loc: "9:37", text: "height", kind: field }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2619 -->

**Using declarations**

The introduced name keeps its target's kind

```snap-semantic_tokens
feature: semantic_tokens
code: |
  namespace tools {
  inline int helper() {
      return 1;
  }
  struct Gadget {};
  }

  using tools::§helper;
  using tools::§Gadget;

  int used = §helper();
  §Gadget gadget;
snapshot: |
  - { loc: "12:13", text: "helper", kind: function }
  - { loc: "13:13", text: "Gadget", kind: struct }
  - { loc: "15:11", text: "helper", kind: function }
  - { loc: "16:0", text: "Gadget", kind: struct }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#868 -->

**Lambda init-captures**

The captured name highlighted as a variable

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int compute();

  auto fn = [§val = compute()] {
      return §val;
  };
snapshot: |
  - { loc: "7:11", text: "val", kind: variable, modifiers: [definition] }
  - { loc: "8:11", text: "val", kind: variable }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#213 -->

**`sizeof...`**

The pack parameter keeps its type-parameter token

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename... Ts>
  constexpr auto count = sizeof...(§Ts);
snapshot: |
  - { loc: "6:33", text: "Ts", kind: type }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#1283 -->

**`using enum`**

The enum name highlighted at the using site

```snap-semantic_tokens
feature: semantic_tokens
code: |
  enum class Color { Red };

  void paint() {
      using enum §Color;
      auto c = §Red;
  }
snapshot: |
  - { loc: "8:15", text: "Color", kind: enum }
  - { loc: "9:13", text: "Red", kind: enumMember, modifiers: [readonly] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deduction guides**

The guide name and the guided template highlighted

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  struct Vec {
      template <typename It>
      Vec(It first, It last);
  };

  template <typename It>
  §Vec(It, It) -> §Vec<int>;
snapshot: |
  - { loc: "11:0", text: "Vec", kind: function, modifiers: [declaration, templated] }
  - { loc: "11:15", text: "Vec", kind: struct }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#316 -->

**Explicit instantiation**

The instantiated template name and its written template arguments highlighted, on the extern declaration and the definition alike

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Widget {};

  template <typename T>
  struct Holder {
      T value;
  };

  extern template struct §Holder<§Widget>;

  template struct §Holder<§Widget>;
snapshot: |
  - { loc: "12:23", text: "Holder", kind: struct, modifiers: [templated] }
  - { loc: "12:30", text: "Widget", kind: struct }
  - { loc: "14:16", text: "Holder", kind: struct, modifiers: [templated] }
  - { loc: "14:23", text: "Widget", kind: struct }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#154 clangd#297 -->

**Dependent names**

Resolved through the primary template where one is known

Dependent members of a known template (`Box<T>`) resolve to the primary
template's declarations and keep their kinds. Members of a bare template
parameter have no candidate declaration and currently get no token;
heuristic coloring for such names remains open.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  struct Box {
      using value_type = int;
      static void reset();
      int size() const;
  };

  template <typename T>
  void resolved(Box<T> box) {
      typename Box<T>::§value_type item;
      Box<T>::§reset();
      box.§size();
  }

  template <typename T>
  void unresolved(T value) {
      typename T::§value_type item;
      T::§reset();
      value.§size();
  }
snapshot: |
  - { loc: "19:21", text: "value_type", kind: type }
  - { loc: "20:12", text: "reset", kind: method, modifiers: [static] }
  - { loc: "21:8", text: "size", kind: method, modifiers: [readonly] }
  - { loc: "26:16", text: "value_type", kind: none }
  - { loc: "27:7", text: "reset", kind: none }
  - { loc: "28:10", text: "size", kind: none }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Variable templates**

declarations, definitions, partial and full specializations

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T, typename U>
  extern int §pair_value;

  template <typename T, typename U>
  int §pair_value = 2;

  template <typename T>
  extern int §pair_value<T, int>;

  template <typename T>
  int §pair_value<T, int> = 4;

  template <>
  int §pair_value<int, int> = 5;
snapshot: |
  - { loc: "5:11", text: "pair_value", kind: variable, modifiers: [declaration, templated] }
  - { loc: "8:4", text: "pair_value", kind: variable, modifiers: [definition, templated] }
  - { loc: "11:11", text: "pair_value", kind: variable, modifiers: [declaration, templated] }
  - { loc: "14:4", text: "pair_value", kind: variable, modifiers: [definition, templated] }
  - { loc: "17:4", text: "pair_value", kind: variable, modifiers: [definition] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Out-of-line member definitions**

Qualified names keep method kinds and modifiers

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Gauge {
      int read() const;
      static void reset();
  };

  int §Gauge::§read() const {
      return 0;
  }

  void Gauge::§reset() {}
snapshot: |
  - { loc: "9:4", text: "Gauge", kind: struct }
  - { loc: "9:11", text: "read", kind: method, modifiers: [definition, readonly] }
  - { loc: "13:12", text: "reset", kind: method, modifiers: [definition, static] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Alias templates**

The alias name carries the type kind and the `templated` modifier

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  using §Ptr = T*;

  template <typename T>
  struct Box {};

  template <typename T>
  using §BoxPtr = §Box<T>*;

  §Ptr<int> pointer = nullptr;
snapshot: |
  - { loc: "5:6", text: "Ptr", kind: type, modifiers: [definition, templated] }
  - { loc: "11:6", text: "BoxPtr", kind: type, modifiers: [definition, templated] }
  - { loc: "11:15", text: "Box", kind: struct, modifiers: [templated] }
  - { loc: "13:0", text: "Ptr", kind: type, modifiers: [templated] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Template template parameters**

Declared and used as types

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  struct Holder {};

  template <template <typename> class §Container, typename T>
  struct Adaptor {
      §Container<T> value;
  };

  Adaptor<Holder, int> adaptor;
snapshot: |
  - { loc: "7:36", text: "Container", kind: type, modifiers: [definition, templated] }
  - { loc: "9:4", text: "Container", kind: type, modifiers: [templated] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Lambda captures**

by-copy and by-reference captures reference the captured variable; `this` stays a keyword

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct S {
      int field;

      int compute() {
          int local = 1;
          auto by_copy = [§local, §this] {
              return §local + this->§field;
          };
          auto by_reference = [&§local] {
              return §local;
          };
          return by_copy() + by_reference();
      }
  };
snapshot: |
  - { loc: "9:24", text: "local", kind: variable }
  - { loc: "9:31", text: "this", kind: keyword }
  - { loc: "10:19", text: "local", kind: variable }
  - { loc: "10:33", text: "field", kind: field }
  - { loc: "12:30", text: "local", kind: variable }
  - { loc: "13:19", text: "local", kind: variable }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Range-based for**

The loop variable at definition and use

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct List {
      int* begin();
      int* end();
  };

  void iterate(List items) {
      for (auto& §item : §items) {
          §item = 0;
      }
  }
snapshot: |
  - { loc: "10:15", text: "item", kind: variable, modifiers: [definition] }
  - { loc: "10:22", text: "items", kind: parameter }
  - { loc: "11:8", text: "item", kind: variable }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Enum underlying types**

The enum-base reference keeps its type kind

```snap-semantic_tokens
feature: semantic_tokens
code: |
  using Byte = unsigned char;

  enum class Flags : §Byte { A, B };

  Flags flags = Flags::§A;
snapshot: |
  - { loc: "6:19", text: "Byte", kind: type }
  - { loc: "8:21", text: "A", kind: enumMember, modifiers: [readonly] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Friend declarations**

Befriended names resolve to their targets; inline friends define

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Widget;
  void ping();

  struct Host {
      friend struct §Widget;
      friend void §ping();
      friend void §inline_friend() {}
  };
snapshot: |
  - { loc: "8:18", text: "Widget", kind: struct }
  - { loc: "9:16", text: "ping", kind: function, modifiers: [declaration] }
  - { loc: "10:16", text: "inline_friend", kind: function, modifiers: [definition] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**Dependent using declarations**

`using T::name` in a template body

The introduced name and its uses currently get no token; the reserved
dependent-name modifier is not emitted yet.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  struct Derived : T {
      using T::§value;

      int use() {
          return §value;
      }
  };
snapshot: |
  - { loc: "9:13", text: "value", kind: none }
  - { loc: "12:15", text: "value", kind: none }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Function explicit instantiation directives**

Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments and the parameter types

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Widget {};

  template <typename T>
  void convert(T value) {}

  extern template void §convert<§Widget>(§Widget);

  template void §convert<§Widget>(§Widget);
snapshot: |
  - { loc: "10:21", text: "convert", kind: none }
  - { loc: "10:29", text: "Widget", kind: none }
  - { loc: "10:37", text: "Widget", kind: none }
  - { loc: "12:14", text: "convert", kind: none }
  - { loc: "12:22", text: "Widget", kind: none }
  - { loc: "12:30", text: "Widget", kind: none }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial llvm#191658 -->

**Variable explicit instantiation directives**

Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments, even the declarator's type

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Widget {};

  template <typename T>
  T zero = T();

  extern template §Widget §zero<§Widget>;

  template §Widget §zero<§Widget>;
snapshot: |
  - { loc: "10:16", text: "Widget", kind: none }
  - { loc: "10:23", text: "zero", kind: none }
  - { loc: "10:28", text: "Widget", kind: none }
  - { loc: "12:9", text: "Widget", kind: none }
  - { loc: "12:16", text: "zero", kind: none }
  - { loc: "12:21", text: "Widget", kind: none }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Explicit instantiation member bodies**

A dependent name paints as its actual resolution: agreeing kinds keep the modifiers all instantiations share, disagreeing kinds paint a conflict

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct A {
      static void hit();
  };

  struct B {
      static int hit;
  };

  struct C {
      void hit();
  };

  template <typename T>
  struct D {
      void go() {
          (void)§T::§hit;
      }
  };

  template struct D<A>;
  template struct D<B>;

  template <typename T>
  struct E {
      void probe(T t) {
          t.§hit();
      }
  };

  template struct E<A>;
  template struct E<C>;
snapshot: |
  - { loc: "19:14", text: "T", kind: type }
  - { loc: "19:17", text: "hit", kind: conflict, modifiers: [static] }
  - { loc: "29:10", text: "hit", kind: method }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Modules

<!-- BEGIN GENERATED ITEMS: modules -->

<!-- BEGIN CAPABILITY: supported -->

**Module declarations**

The contextual `module` keyword, dotted module names and the private fragment

```snap-semantic_tokens
feature: semantic_tokens
code: |
  module;

  export module demo.core;

  export int exported_value = 1;

  module :private;

  int private_value = 2;

  #if 0
  module :private;
  #endif
snapshot: |
  - { loc: "0:0", text: "/// # Module declarations — the contextual `module` keyword, dotted module names and the private fragment", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "4:0", text: "module", kind: keyword }
  - { loc: "6:0", text: "export", kind: keyword }
  - { loc: "6:7", text: "module", kind: keyword }
  - { loc: "6:14", text: "demo", kind: module }
  - { loc: "6:19", text: "core", kind: module }
  - { loc: "8:0", text: "export", kind: keyword }
  - { loc: "8:7", text: "int", kind: primitive }
  - { loc: "8:11", text: "exported_value", kind: variable, modifiers: [definition] }
  - { loc: "8:28", text: "1", kind: number }
  - { loc: "10:0", text: "module", kind: keyword }
  - { loc: "10:8", text: "private", kind: keyword }
  - { loc: "12:0", text: "int", kind: primitive }
  - { loc: "12:4", text: "private_value", kind: variable, modifiers: [definition] }
  - { loc: "12:20", text: "2", kind: number }
  - { loc: "14:0", text: "#if", kind: directive }
  - { loc: "14:4", text: "0", kind: number }
  - { loc: "15:0", text: "module", kind: identifier, modifiers: [inactive] }
  - { loc: "15:7", text: ":", kind: identifier, modifiers: [inactive] }
  - { loc: "15:8", text: "private", kind: keyword, modifiers: [inactive] }
  - { loc: "15:15", text: ";", kind: identifier, modifiers: [inactive] }
  - { loc: "16:0", text: "#endif", kind: directive }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Module partitions**

Partition names in the module declaration

```snap-semantic_tokens
feature: semantic_tokens
code: |
  export module demo.core:part;

  export int partition_value = 1;
snapshot: |
  - { loc: "0:0", text: "/// # Module partitions — partition names in the module declaration", kind: comment }
  - { loc: "1:0", text: "///", kind: comment }
  - { loc: "2:0", text: "/// - status: supported", kind: comment }
  - { loc: "4:0", text: "export", kind: keyword }
  - { loc: "4:7", text: "module", kind: keyword }
  - { loc: "4:14", text: "demo", kind: module }
  - { loc: "4:19", text: "core", kind: module }
  - { loc: "4:24", text: "part", kind: module }
  - { loc: "6:0", text: "export", kind: keyword }
  - { loc: "6:7", text: "int", kind: primitive }
  - { loc: "6:11", text: "partition_value", kind: variable, modifiers: [definition] }
  - { loc: "6:29", text: "1", kind: number }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`module` and `import` as identifiers**

Contextual keywords keep their semantic kinds outside module declarations

```snap-semantic_tokens
feature: semantic_tokens
code: |
  void f() {
      struct §module {};
      §module §m;
      int §import = 1;
      int §module = 2;
  }
snapshot: |
  - { loc: "5:11", text: "module", kind: struct, modifiers: [definition] }
  - { loc: "6:4", text: "module", kind: struct }
  - { loc: "6:11", text: "m", kind: variable, modifiers: [definition] }
  - { loc: "7:8", text: "import", kind: variable, modifiers: [definition] }
  - { loc: "8:8", text: "module", kind: variable, modifiers: [definition] }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token Modifiers

<!-- BEGIN GENERATED ITEMS: token_modifiers -->

<!-- BEGIN CAPABILITY: supported -->

**Declaration vs definition**

The modifier distinguishes the two

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int §measure(int value);

  int §measure(int value) {
      return value;
  }

  struct §Sensor;

  struct §Sensor {};
snapshot: |
  - { loc: "4:4", text: "measure", kind: function, modifiers: [declaration] }
  - { loc: "6:4", text: "measure", kind: function, modifiers: [definition] }
  - { loc: "10:7", text: "Sensor", kind: struct, modifiers: [declaration] }
  - { loc: "12:7", text: "Sensor", kind: struct, modifiers: [definition] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Static**

class-level members and static locals

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Counter {
      static int §total;
      static void §bump();
      int §current;
  };

  void count() {
      static int §calls = 0;
      Counter::§bump();
      Counter::§total = §calls;
  }
snapshot: |
  - { loc: "5:15", text: "total", kind: variable, modifiers: [declaration, static] }
  - { loc: "6:16", text: "bump", kind: method, modifiers: [declaration, static] }
  - { loc: "7:8", text: "current", kind: field, modifiers: [definition] }
  - { loc: "11:15", text: "calls", kind: variable, modifiers: [definition, static] }
  - { loc: "12:13", text: "bump", kind: method, modifiers: [static] }
  - { loc: "13:13", text: "total", kind: variable, modifiers: [static] }
  - { loc: "13:21", text: "calls", kind: variable, modifiers: [static] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Readonly**

Const and constexpr values, const methods and enum members

Readonly is currently value-based: a pointer to const counts as
readonly even though the pointer itself can change.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  enum class Level { §High };

  const int §limit = 10;
  constexpr int §bound = 4;

  struct Gauge {
      int §read() const;
      void §write(int value);
  };

  void probe(const int& §in, const int* §pointee_const, int* const §self_const) {
      Gauge gauge;
      gauge.§read();
      gauge.§write(§limit);
  }
snapshot: |
  - { loc: "7:19", text: "High", kind: enumMember, modifiers: [definition, readonly] }
  - { loc: "9:10", text: "limit", kind: variable, modifiers: [definition, readonly] }
  - { loc: "10:14", text: "bound", kind: variable, modifiers: [definition, readonly] }
  - { loc: "13:8", text: "read", kind: method, modifiers: [declaration, readonly] }
  - { loc: "14:9", text: "write", kind: method, modifiers: [declaration] }
  - { loc: "17:22", text: "in", kind: parameter, modifiers: [definition, readonly] }
  - { loc: "17:37", text: "pointee_const", kind: parameter, modifiers: [definition, readonly] }
  - { loc: "17:63", text: "self_const", kind: parameter, modifiers: [definition, readonly] }
  - { loc: "19:10", text: "read", kind: method, modifiers: [readonly] }
  - { loc: "20:10", text: "write", kind: method }
  - { loc: "20:16", text: "limit", kind: variable, modifiers: [readonly] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Virtual and abstract**

Virtual methods, pure virtual methods and abstract classes

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct §Shape {
      virtual int §area();
      virtual int §perimeter() = 0;
      virtual ~Shape();
  };

  struct §Square : Shape {
      int §perimeter() override;
  };

  int measure(Shape& shape) {
      return shape.§area() + shape.§perimeter();
  }
snapshot: |
  - { loc: "4:7", text: "Shape", kind: struct, modifiers: [definition, abstract] }
  - { loc: "5:16", text: "area", kind: method, modifiers: [declaration, virtual] }
  - { loc: "6:16", text: "perimeter", kind: method, modifiers: [declaration, abstract, virtual] }
  - { loc: "10:7", text: "Square", kind: struct, modifiers: [definition] }
  - { loc: "11:8", text: "perimeter", kind: method, modifiers: [declaration, virtual] }
  - { loc: "15:17", text: "area", kind: method, modifiers: [virtual] }
  - { loc: "15:32", text: "perimeter", kind: method, modifiers: [abstract, virtual] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Deprecated**

`[[deprecated]]` declarations and their uses

```snap-semantic_tokens
feature: semantic_tokens
code: |
  [[deprecated("use next_api")]] void §old_api();
  void next_api();

  void migrate() {
      §old_api();
  }
snapshot: |
  - { loc: "4:36", text: "old_api", kind: function, modifiers: [declaration, deprecated] }
  - { loc: "8:4", text: "old_api", kind: function, modifiers: [deprecated] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Default library**

Symbols declared in system headers

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int before_includes = 0;

  #include <syslib.h>

  int used = §system_helper();
snapshot: |
  - { loc: "8:11", text: "system_helper", kind: function, modifiers: [defaultLibrary] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#352 -->

**Scope modifiers**

function, class, file and global scope

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int global_scope;
  static int file_scope;

  struct Foo {
      int class_scope;

      void bar() {
          int function_scope = 0;
      }
  };
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#839 -->

**Mutable reference and pointer**

Arguments passed by non-const reference or pointer

```snap-semantic_tokens
feature: semantic_tokens
code: |
  void modify(int& out);
  void modify_through(int* out);
  void inspect(const int& in);

  void run() {
      int value = 0;
      modify(value);
      modify_through(&value);
      inspect(value);
  }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**Deduced**

Mark deduced types such as `auto` and `decltype`

```snap-semantic_tokens
feature: semantic_tokens
code: |
  auto deduced_int = 1;
  decltype(deduced_int) same_type = 2;
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1521 -->

**User-defined operators**

Distinguish overloaded operators from built-in ones

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Vec {
      Vec operator+(const Vec& other) const;
  };

  Vec add(Vec a, Vec b) {
      return a + b;
  }

  int add(int a, int b) {
      return a + b;
  }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Conflict & Ambiguity

C++ allows structurally different entities to share one name. When a single
written name refers to entities of different kinds at once, no single token
type is correct; such names receive the dedicated **conflict** token type,
which clients typically display in a neutral color.

<!-- BEGIN GENERATED ITEMS: conflict_ambiguity -->

<!-- BEGIN CAPABILITY: supported -->

**Type vs function**

A name naming both renders as `conflict`

```snap-semantic_tokens
feature: semantic_tokens
code: |
  namespace shop {
  struct §Widget {};
  void §Widget();
  }

  using shop::§Widget;
snapshot: |
  - { loc: "5:7", text: "Widget", kind: struct, modifiers: [definition] }
  - { loc: "6:5", text: "Widget", kind: function, modifiers: [declaration] }
  - { loc: "9:12", text: "Widget", kind: conflict }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Type vs variable**

A name naming both renders as `conflict`

```snap-semantic_tokens
feature: semantic_tokens
code: |
  namespace mixed {
  struct Thing {};
  int Thing;
  }

  using mixed::§Thing;
snapshot: |
  - { loc: "9:13", text: "Thing", kind: conflict }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Same-kind overload sets**

A name naming only functions is no conflict

```snap-semantic_tokens
feature: semantic_tokens
code: |
  namespace ops {
  void apply();
  void apply(int level);
  }

  using ops::§apply;

  void run() {
      §apply();
      §apply(1);
  }
snapshot: |
  - { loc: "9:11", text: "apply", kind: function }
  - { loc: "12:4", text: "apply", kind: function }
  - { loc: "13:4", text: "apply", kind: function }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Injected class name**

The class name used as a constructor call inside the class

The written name renders as the class; the constructor reference it
implies paints nothing extra — the `(` stays token-free.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Widget {
      Widget(int size);

      Widget create() {
          return §Widget§()(42);
      }
  };
snapshot: |
  - { loc: "11:15", text: "Widget", kind: struct }
  - { loc: "11:21", text: "(", kind: none }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Token Correctness

Shapes clice pins deliberately, including issues clangd got wrong.

<!-- BEGIN GENERATED ITEMS: token_correctness -->

<!-- BEGIN CAPABILITY: supported clangd#1509 clangd#2078 clangd#872 -->

**Constructors and destructors**

Method tokens with the constructor/destructor modifier

A destructor name renders as two tokens: the `~` carries the method
kind and the declaration/definition modifiers, the class name after it
stays a reference to the class.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Session {
      §Session();
      §~§Session();
  };

  Session::§Session() {}

  Session::§~Session() {}

  void destroy(Session* session) {
      session->§~Session();
  }
snapshot: |
  - { loc: "10:4", text: "Session", kind: method, modifiers: [declaration, constructorOrDestructor] }
  - { loc: "11:4", text: "~", kind: method, modifiers: [declaration, constructorOrDestructor] }
  - { loc: "11:5", text: "Session", kind: struct }
  - { loc: "14:9", text: "Session", kind: method, modifiers: [definition, constructorOrDestructor] }
  - { loc: "16:9", text: "~", kind: method, modifiers: [definition, constructorOrDestructor] }
  - { loc: "19:13", text: "~", kind: method, modifiers: [constructorOrDestructor] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Anonymous parameters**

Unnamed parameters produce no tokens

The punctuation after an unnamed parameter's type stays token-free.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  void take_one(int§) {}
  void take_two(int§, char* c) {}
snapshot: |
  - { loc: "6:17", text: ")", kind: none }
  - { loc: "7:17", text: ",", kind: none }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Operator names**

The `operator` keyword and call-site punctuation stay plain

An operator's written name is keyword plus punctuation, so no name
token is painted: `operator` keeps its keyword classification and
call sites emit nothing on the operator symbol.

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Value {
      Value& §operator=(const Value& other);
      Value §operator+(const Value& other) const;
  };

  void combine(Value a, Value b) {
      a §= b;
      Value c = a §+ b;
  }
snapshot: |
  - { loc: "9:11", text: "operator", kind: keyword }
  - { loc: "10:10", text: "operator", kind: keyword }
  - { loc: "14:6", text: "=", kind: none }
  - { loc: "15:16", text: "+", kind: none }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Destructors of class templates**

The `~` shape holds under templates

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  struct Holder {
      §~§Holder();
  };

  template <typename T>
  Holder<T>::§~§Holder() {}
snapshot: |
  - { loc: "6:4", text: "~", kind: method, modifiers: [declaration, constructorOrDestructor] }
  - { loc: "6:5", text: "Holder", kind: struct, modifiers: [templated] }
  - { loc: "10:11", text: "~", kind: method, modifiers: [definition, constructorOrDestructor] }
  - { loc: "10:12", text: "Holder", kind: struct, modifiers: [templated] }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Conversion operators**

Written as keywords, converting uses paint nothing extra

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Ratio {
      §operator double() const;
      explicit §operator bool() const;
  };

  double to_double(Ratio ratio) {
      if (§ratio) {
          return §ratio;
      }
      return double(§ratio);
  }
snapshot: |
  - { loc: "5:4", text: "operator", kind: keyword }
  - { loc: "6:13", text: "operator", kind: keyword }
  - { loc: "10:8", text: "ratio", kind: parameter }
  - { loc: "11:15", text: "ratio", kind: parameter }
  - { loc: "13:18", text: "ratio", kind: parameter }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Pseudo-destructor on a template parameter**

The `~` paints nothing; the type name keeps its kind

```snap-semantic_tokens
feature: semantic_tokens
code: |
  template <typename T>
  void reset(T* value) {
      value->§~§T();
  }
snapshot: |
  - { loc: "6:11", text: "~", kind: none }
  - { loc: "6:12", text: "T", kind: type }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Defaulted and deleted members**

special-member names keep their definition tokens

```snap-semantic_tokens
feature: semantic_tokens
code: |
  struct Session {
      §Session() = default;
      §Session(const Session&) = delete;
      §~§Session() = default;
  };
snapshot: |
  - { loc: "5:4", text: "Session", kind: method, modifiers: [definition, constructorOrDestructor] }
  - { loc: "6:4", text: "Session", kind: method, modifiers: [definition, constructorOrDestructor] }
  - { loc: "7:4", text: "~", kind: method, modifiers: [definition, constructorOrDestructor] }
  - { loc: "7:5", text: "Session", kind: struct }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Attributes

<!-- BEGIN GENERATED ITEMS: attributes -->

<!-- BEGIN CAPABILITY: unsupported clangd#2209 -->

**Attribute names**

Standard and vendor attributes, and expressions inside them

```snap-semantic_tokens
feature: semantic_tokens
code: |
  [[nodiscard]] int compute();
  [[deprecated("use v2")]] void old_func();
  [[maybe_unused]] int counter = 0;

  struct [[gnu::packed]] Packed {};
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Macros

Tokens inside macro definition bodies keep their lexical kinds; highlighting
them from their expansions belongs to a future expansion-preview feature.

<!-- BEGIN GENERATED ITEMS: macros -->

<!-- BEGIN CAPABILITY: supported -->

**Macro definition and expansion**

```snap-semantic_tokens
feature: semantic_tokens
code: |
  #define SQUARE(x) ((x) * (x))

  [[maybe_unused]] static int squared = SQUARE(4);
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Expansion sites and arguments**

Expansion names are macros, written arguments keep their semantics, definition bodies stay lexical

```snap-semantic_tokens
feature: semantic_tokens
code: |
  int value = 1;

  #define ID(x) x
  #define CALL §helper()

  void helper();

  int copied = §ID(§value);

  void run() {
      §CALL;
  }
snapshot: |
  - { loc: "7:13", text: "helper", kind: none }
  - { loc: "11:13", text: "ID", kind: macro }
  - { loc: "11:16", text: "value", kind: variable }
  - { loc: "14:4", text: "CALL", kind: macro }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2649 -->

**Object-like vs function-like macros**

Distinct highlighting for the two forms

```snap-semantic_tokens
feature: semantic_tokens
code: |
  #define MAX_SIZE 1024
  #define CHECK(x) ((x) ? 1 : 0)

  int checked = CHECK(MAX_SIZE);
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Known Gaps

Curated issues without a fixture yet:

- `auto` parameters must not be highlighted as template type parameters
  ([clangd#1390](https://github.com/clangd/clangd/issues/1390))
- Nested name specifier in a pointer-to-member should get a token
  ([clangd#2235](https://github.com/clangd/clangd/issues/2235))
- `::new` should keep the `new` keyword highlighted
  ([clangd#1627](https://github.com/clangd/clangd/issues/1627))
- `co_yield` / `co_await` lose highlighting when the coroutine return type is
  a template ([clangd#2437](https://github.com/clangd/clangd/issues/2437))
- Token modifiers should apply to operands of overloaded operators
  ([clangd#2547](https://github.com/clangd/clangd/issues/2547))
- Dependent template names (`obj.template get<int>()`), members imported from
  a dependent base via `using`, and dependent names with mixed-kind overload
  sets ([clangd#484](https://github.com/clangd/clangd/issues/484),
  [clangd#686](https://github.com/clangd/clangd/issues/686),
  [clangd#1057](https://github.com/clangd/clangd/issues/1057))

## Inactive Code Regions

Every token inside an untaken preprocessor branch carries the `inactive`
modifier while keeping its lexical kind, so editors dim the region by
styling the modifier without losing the syntax colors underneath. Tokens
without a classification in dead code — bare identifiers and plain
punctuation — are emitted as the unstyled `identifier` type, giving the
whole region token coverage. The clice VS Code extension renders the
regions dimmed out of the box; other editors style the modifier directly
(e.g. `@lsp.mod.inactive` in Neovim).

- [x] Dim inactive preprocessor branches ([clangd#132](https://github.com/clangd/clangd/issues/132))
- [x] Correct inactive boundaries with `#elif` chains ([clangd#602](https://github.com/clangd/clangd/issues/602))
- [x] Preserve syntax highlighting within inactive regions ([clangd#1664](https://github.com/clangd/clangd/issues/1664))
- [x] Keep inactive regions distinct from comments ([clangd#1545](https://github.com/clangd/clangd/issues/1545))
- [ ] Unreachable code dimming ([clangd#1828](https://github.com/clangd/clangd/issues/1828))

## Format String Highlighting

- [ ] `std::format` / `std::print` placeholder highlighting ([clangd#1709](https://github.com/clangd/clangd/issues/1709))
- [ ] Highlight invalid format specifiers as errors

## Protocol Support

- [x] Full document semantic tokens (`textDocument/semanticTokens/full`)
- [x] UTF-16 delta-encoded token positions
- [ ] Range-based semantic tokens (`textDocument/semanticTokens/range`) — only
      compute tokens for the visible viewport, critical for large files
- [ ] Delta updates (`textDocument/semanticTokens/full/delta`) — send only
      changes since the previous response
