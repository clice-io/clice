# LLVM Changelog

Breaking changes encountered during each LLVM upgrade, with upstream commit references.

# LLVM 21 → 22

Upgrade version: LLVM 22.1.4 (`llvmorg-22.1.4`)

The single largest source of breaking changes is `91cdd35008e9` ([#147835](https://github.com/llvm/llvm-project/pull/147835)), which simultaneously restructured the type system and NNS representation.

## Type System

| Change                                                                                  | Commit         | PR                                                          | Impact                                                                                                                                            |
| --------------------------------------------------------------------------------------- | -------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ElaboratedType` removed; keyword/qualifier merged into `TagType`, `TypedefType`, etc.  | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | All `getAs<ElaboratedType>()` must be removed. TypePrinter behavior changed: non-canonical TagType keyword printing ignores `SuppressTagKeyword`. |
| `InjectedClassNameType` now inherits `TagType`                                          | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | `getAs<TagType>()` now matches InjectedClassNameType. `getInjectedSpecializationType()` removed.                                                  |
| `DependentTemplateSpecializationType` removed, merged into `TemplateSpecializationType` | `ba9d1c41c41d` | [#158109](https://github.com/llvm/llvm-project/pull/158109) | All DTST visitors/handlers must migrate to TST.                                                                                                   |
| `TagDecl::getTypeForDecl()` marked `= delete`                                           | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | Use `ASTContext::getCanonicalTagType()` instead.                                                                                                  |
| `UsingType::getFoundDecl()` renamed to `getDecl()`                                      | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | Mechanical rename.                                                                                                                                |
| `PredefinedSugarType` introduced (`__size_t`, `__ptrdiff_t`)                            | `7c402b8b81d2` | [#149613](https://github.com/llvm/llvm-project/pull/149613) | `sizeof` return type changed from `unsigned long` to `__size_t` sugar. Hover needs to desugar to avoid exposing internal type names.              |
| Implicit var template specializations no longer retain written template args            | `1cb47c19f8ec` | [#156329](https://github.com/llvm/llvm-project/pull/156329) | `VarTemplateSpecializationDecl::getTemplateArgsAsWritten()` returns null for implicit specializations.                                            |

## NestedNameSpecifier

| Change                                 | Commit         | PR                                                          | Impact                                                                                                                      |
| -------------------------------------- | -------------- | ----------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| NNS changed from pointer to value type | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | `const NestedNameSpecifier*` → `NestedNameSpecifier`; `->` → `.`; `Kind::Identifier` and `Kind::NamespaceAlias` removed.    |
| `NamespaceBaseDecl` introduced         | `4a9eaad9e128` | [#149123](https://github.com/llvm/llvm-project/pull/149123) | `getAsNamespace()` → `getAsNamespaceAndPrefix().Namespace` (returns `NamespaceBaseDecl*`, covers both namespace and alias). |

## Driver / Frontend

| Change                                                          | Commit         | PR                                                          | Impact                                                           |
| --------------------------------------------------------------- | -------------- | ----------------------------------------------------------- | ---------------------------------------------------------------- |
| `Options.td/inc` moved from `clang/Driver/` to `clang/Options/` | `f63d33da0a51` | [#167374](https://github.com/llvm/llvm-project/pull/167374) | Include path replacement.                                        |
| `OPTION` macro gained 15th parameter (SubCommandIDsOffset)      | `fdbd17d1fb0d` | [#155026](https://github.com/llvm/llvm-project/pull/155026) | Custom `OPTION` macro definitions need `...` variadic parameter. |
| `Driver::GetResourcesPath` moved to `clang::GetResourcesPath`   | `d090311aa7df` | [#169599](https://github.com/llvm/llvm-project/pull/169599) | Requires `#include "clang/Options/OptionUtils.h"`.               |
| `CompilerInstance::createDiagnostics` VFS parameter removed     | `30633f308941` | [#158381](https://github.com/llvm/llvm-project/pull/158381) | Use `setVirtualFileSystem()` + `createFileManager()` instead.    |

## Other

| Change                                                                  | Commit         | PR                                                          | Impact                 |
| ----------------------------------------------------------------------- | -------------- | ----------------------------------------------------------- | ---------------------- |
| `llvm::sys::fs::make_absolute` → `llvm::sys::path::make_absolute`       | `f122484b998d` | [#161459](https://github.com/llvm/llvm-project/pull/161459) | Namespace replacement. |
| `ClangTidyModuleRegistry.h` deprecated, merged into `ClangTidyModule.h` | `1bcf74006bcf` | [#173231](https://github.com/llvm/llvm-project/pull/173231) | Include replacement.   |
