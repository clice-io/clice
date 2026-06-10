// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Documentation from the AST of templates.
namespace docs_ast {
// doc
template <typename T> class X {};
// doc
template <typename T> void bar() {}
// doc
template <typename T> T baz;
void foo() {
  au$(01_auto_class_doc)to t = X<int>();
  X$(02_class_ref_doc)<int>();
  b$(03_function_doc)ar<int>();
  au$(04_auto_var_tmpl_doc)to T = ba$(05_var_tmpl_ref_doc)z<X<int>>;
  ba$(06_var_tmpl_assign_doc)z<int> = 0;
}
}

// Documentation from the most specialized declaration.
namespace docs_special {
// doc1
template <typename T> class $(07_primary_doc)X {};
// doc2
template <> class $(08_full_spec_doc)X<int> {};
// doc3
template <typename T> class $(09_partial_spec_doc)X<T*> {};
void foo() {
  X$(10_primary_ref_doc)<char>();
  X$(11_full_spec_ref_doc)<int>();
  X$(12_partial_spec_ref_doc)<int*>();
}
}
