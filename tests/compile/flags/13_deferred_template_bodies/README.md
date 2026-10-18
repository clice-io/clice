# Deferred template bodies

A command that asks clang to defer template bodies has them deferred in every compile

clice parses a template's body where it is written, even on MSVC targets, where clang's default before C++20 defers it, so that hover, navigation and diagnostics work inside every template. Code written for MSVC's permissive lookup, such as a template using a name declared after it, then reports the errors a conforming compiler would. A command that passes `-fdelayed-template-parsing` (clang-cl: `/Zc:twoPhase-`), from the build or appended by a rule, compiles such code as the compiler does. The cost: the body of a template that nothing instantiates stays unparsed, and nothing inside it can be hovered or found.
