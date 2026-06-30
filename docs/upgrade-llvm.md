# LLVM 升级指南

本文档记录将 clice 依赖的 LLVM 预构建包升级到新版本的完整流程。

## 前置条件

- 拥有 `clice-io/clice` 和 `clice-io/clice-llvm` 仓库的写权限
- 本地安装了 pixi 环境
- 已配置 `gh` CLI 并有 GitHub token

## Step 1: 触发 LLVM 构建

在 GitHub Actions 手动触发 `build-llvm` 工作流：

```bash
gh workflow run build-llvm.yml \
  --field llvm_version="22.1.4" \
  --field skip_clice_build=true \
  --field skip_upload=true \
  --field skip_pr=true
```

- `skip_clice_build=true`：新版 LLVM API 可能有破坏性变更，clice 编译大概率失败，先跳过
- `skip_upload=true`：产物未经裁剪，暂不上传到 clice-llvm
- `skip_pr=true`：manifest 还没准备好，暂不创建 PR

等待所有 14 个矩阵构建完成（约 2-3 小时），记下 workflow run ID。

## Step 2: 下载本地平台产物

从 Step 1 的 run 下载当前开发机对应的 artifact：

```bash
# 查看可用的 artifact 列表
gh run view <RUN_ID>

# 下载对应平台产物（以 Linux x64 RelWithDebInfo 为例）
gh run download <RUN_ID> -n x64-linux-gnu-releasedbg.tar.xz -D .llvm-download
mkdir -p .llvm
tar -xf .llvm-download/x64-linux-gnu-releasedbg.tar.xz -C .llvm
```

用下载的 LLVM 配置 clice 构建：

```bash
pixi run cmake-config RelWithDebInfo ON -- "-DLLVM_INSTALL_PATH=.llvm/build-install"
pixi run cmake-build RelWithDebInfo
```

此时大概率编译失败——这正是 Step 3 要解决的。

## Step 3: 本地适配 API 变更

根据编译错误逐个修复 LLVM API 变更。常见的破坏性变更类型：

- **头文件路径变更**：如 `clang/Driver/Options.h` → `clang/Options/Options.h`
- **命名空间迁移**：如 `clang::driver::options` → `clang::options`
- **类型系统变更**：如 ElaboratedType 移除、NestedNameSpecifier 从指针变为值类型
- **函数签名变更**：如 `createDiagnostics` 参数变化、`TraverseTypeLoc` 新增参数
- **类型合并/拆分**：如 DependentTemplateSpecializationType 合并到 TemplateSpecializationType

修复策略：

1. 先处理头文件和命名空间变更（机械替换）
2. 再处理类型系统和签名变更（需要理解语义）
3. 更新测试期望值（AST 结构变化会导致测试输出变化）
4. 确保 `pixi run unit-test RelWithDebInfo` 全部通过

## Step 4: 创建 PR

基于 main 新建分支，提交所有变更：

```bash
git checkout -b chore/upgrade-llvm-XX
git add -A
git commit -m "chore: upgrade LLVM to XX.Y.Z"
git push -u origin chore/upgrade-llvm-XX
gh pr create --title "chore: upgrade LLVM to XX.Y.Z" --body "..."
```

此时 PR 的 CI 会失败（config/llvm-manifest.json 里还是旧版本的 hash），这是预期的。

## Step 5: 运行 Release LLVM 工作流

手动触发 `release-llvm` 工作流，它会：

1. **discover**：在三个平台（Ubuntu/macOS/Windows）上用 clice 编译测试，找出哪些 LLVM 静态库可以安全移除
2. **create-release**：在 clice-llvm 仓库创建新版本的 release
3. **repackage**：下载 Step 1 的 14 个 artifact，应用裁剪 manifest（用空 archive 替换不需要的 .a），用 xz -9e 极限压缩后上传到 release
4. **finalize**：汇总所有 artifact 的 SHA256 生成 `llvm-manifest.json` 并上传

```bash
gh workflow run release-llvm.yml \
  --field source_run_id="<STEP1_RUN_ID>" \
  --field llvm_version="22.1.4"
```

等待完成后，在 https://github.com/clice-io/clice-llvm/releases 确认所有 14 个 artifact + manifest 都已上传。

## Step 6: 更新 manifest 并等待 CI

从 release-llvm 工作流的产物中下载生成的 manifest：

```bash
# 找到 release-llvm 的 run ID
gh run list --workflow release-llvm.yml --limit 1

# 下载 manifest
gh run download <RELEASE_RUN_ID> -n llvm-manifest-final -D .
cp llvm-manifest.json config/llvm-manifest.json
```

同时更新 `cmake/package.cmake` 中的版本号：

```bash
python3 scripts/update-llvm-version.py \
  --version "22.1.4" \
  --manifest-src llvm-manifest.json \
  --manifest-dest config/llvm-manifest.json \
  --package-cmake cmake/package.cmake
```

提交并推送：

```bash
git add config/llvm-manifest.json cmake/package.cmake
git commit -m "chore: update llvm-manifest.json to XX.Y.Z"
git push
```

现在 CI 会从 clice-llvm release 下载预构建 LLVM，所有平台的构建和测试应当通过。

## Step 7: 合并

CI 全绿后，合并 PR。升级完成。

## LLVM API Changelog

记录每次 LLVM 升级中影响 clice 的 breaking changes，方便后续 sync 追踪。

### LLVM 21 → 22

核心变更几乎全部来自 `91cdd35008e9` ([clang] Improve nested name specifier AST representation, [#147835](https://github.com/llvm/llvm-project/pull/147835))，这是一个超大型重构，同时改了类型系统和 NNS 表示。

#### 类型系统

| 变更                                                                            | Commit         | PR                                                          | 影响                                                                                                                                 |
| ------------------------------------------------------------------------------- | -------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `ElaboratedType` 移除，keyword/qualifier 嵌入 `TagType`、`TypedefType` 等       | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | 所有 `getAs<ElaboratedType>()` 调用需删除；TypePrinter 行为变化：non-canonical TagType 的 keyword 打印不受 `SuppressTagKeyword` 控制 |
| `InjectedClassNameType` 改为继承 `TagType`                                      | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | `getAs<TagType>()` 现在也匹配 InjectedClassNameType；`getInjectedSpecializationType()` 被移除                                        |
| `DependentTemplateSpecializationType` 移除，合并到 `TemplateSpecializationType` | `ba9d1c41c41d` | [#158109](https://github.com/llvm/llvm-project/pull/158109) | 所有 DTST visitor/handler 需迁移到 TST                                                                                               |
| `TagDecl::getTypeForDecl()` 标记为 `= delete`                                   | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | 改用 `ASTContext::getCanonicalTagType()`                                                                                             |
| `UsingType::getFoundDecl()` 重命名为 `getDecl()`                                | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | 机械替换                                                                                                                             |
| `PredefinedSugarType` 引入（`__size_t`、`__ptrdiff_t`）                         | `7c402b8b81d2` | [#149613](https://github.com/llvm/llvm-project/pull/149613) | `sizeof` 返回类型从 `unsigned long` 变为 `__size_t` sugar；hover 需 desugar 避免暴露内部类型名                                       |
| 变量模板隐式特化不再保留 written template args                                  | `1cb47c19f8ec` | [#156329](https://github.com/llvm/llvm-project/pull/156329) | `VarTemplateSpecializationDecl::getTemplateArgsAsWritten()` 对隐式特化返回 null                                                      |

#### NestedNameSpecifier

| 变更                     | Commit         | PR                                                          | 影响                                                                                                                |
| ------------------------ | -------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| NNS 从指针类型改为值类型 | `91cdd35008e9` | [#147835](https://github.com/llvm/llvm-project/pull/147835) | `const NestedNameSpecifier*` → `NestedNameSpecifier`；`->` → `.`；`Kind::Identifier` 和 `Kind::NamespaceAlias` 移除 |
| `NamespaceBaseDecl` 引入 | `4a9eaad9e128` | [#149123](https://github.com/llvm/llvm-project/pull/149123) | `getAsNamespace()` → `getAsNamespaceAndPrefix().Namespace`（返回 `NamespaceBaseDecl*`，同时覆盖 alias）             |

#### Driver / Frontend

| 变更                                                      | Commit         | PR                                                          | 影响                                                  |
| --------------------------------------------------------- | -------------- | ----------------------------------------------------------- | ----------------------------------------------------- |
| `Options.td/inc` 从 `clang/Driver/` 移到 `clang/Options/` | `f63d33da0a51` | [#167374](https://github.com/llvm/llvm-project/pull/167374) | include 路径替换                                      |
| `OPTION` 宏增加第 15 个参数 (SubCommandIDsOffset)         | `fdbd17d1fb0d` | [#155026](https://github.com/llvm/llvm-project/pull/155026) | 自定义 `OPTION` 宏定义需加 `...` 可变参数             |
| `Driver::GetResourcesPath` 移到 `clang::GetResourcesPath` | `d090311aa7df` | [#169599](https://github.com/llvm/llvm-project/pull/169599) | 需 `#include "clang/Options/OptionUtils.h"`           |
| `CompilerInstance::createDiagnostics` VFS 参数移除        | `30633f308941` | [#158381](https://github.com/llvm/llvm-project/pull/158381) | 改用 `setVirtualFileSystem()` + `createFileManager()` |

#### 其他

| 变更                                                              | Commit         | PR                                                          | 影响         |
| ----------------------------------------------------------------- | -------------- | ----------------------------------------------------------- | ------------ |
| `llvm::sys::fs::make_absolute` → `llvm::sys::path::make_absolute` | `f122484b998d` | [#161459](https://github.com/llvm/llvm-project/pull/161459) | 命名空间替换 |
| `ClangTidyModuleRegistry.h` 废弃，合并到 `ClangTidyModule.h`      | `1bcf74006bcf` | [#173231](https://github.com/llvm/llvm-project/pull/173231) | include 替换 |

## 注意事项

- **产物大小限制**：GitHub Release 单文件上限 2GB。macOS LTO 产物最大，目前用 xz -9e 极限压缩控制在 ~1.7GB。如果未来超限，需要从上游减少不必要的依赖。
- **裁剪安全性**：discover 阶段通过逐个删除 .a 并重新编译 clice 来验证安全性。clang-tidy 模块因 force-link 机制不能删除。共享库（.so/.dylib）直接置零，因为 clice 只需要静态链接。
- **版本缓存**：cmake 下载逻辑会在 `.llvm/.llvm-version` 写入版本标记，版本变更时自动清理重装，不会误用旧版本。
- **私有头文件**：clice 依赖 Clang 的私有 Sema 头文件（TreeTransform.h 等），这些头文件在 `build-llvm.py` 构建时从源码复制到安装目录。用户必须使用我们打包的 LLVM。
