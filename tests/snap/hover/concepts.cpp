// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Concept constraining a variable type.
namespace constrained_var {
template <class T> concept F = true;
§(01_concept_on_var)F auto x = 1;
}

// Constrained template parameter.
namespace constrained_tparam {
template<class T> concept Fooable = true;
template<Foo§(02_concept_on_tparam)able T>
void bar(T t) {}
}

// The constrained template parameter itself.
namespace constrained_tparam_decl {
template<class T> concept Fooable = true;
template<Fooable T§(03_constrained_tparam)T>
void bar(TT t) {}
}

// Constrained auto parameter.
namespace constrained_auto_param {
template<class T> concept Fooable = true;
void bar(Foo§(04_concept_on_auto_param)able auto t) {}
}

// Concept reference.
namespace concept_reference {
template<class T> concept Fooable = true;
auto X = Fooa§(05_concept_reference)ble<int>;
}
