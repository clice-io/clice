import re, os, sys, collections

# The llvm-project checkout: the argument, else ../llvm-project next to the
# repository (the upgrade-llvm skill's convention).
repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
checkout = (
    sys.argv[1]
    if len(sys.argv) > 1
    else os.path.join(os.path.dirname(repo), "llvm-project")
)
root = os.path.join(checkout, "clang-tools-extra", "clang-tidy")
if not os.path.isdir(root):
    sys.exit(
        f"no clang-tidy sources under {checkout}: pass the llvm-project checkout as the argument"
    )
# class -> check name from *Module.cpp registerCheck<Class>("name")
names = {}
for dirpath, _, files in os.walk(root):
    for f in files:
        if f.endswith("Module.cpp"):
            src = open(
                os.path.join(dirpath, f), encoding="utf-8", errors="replace"
            ).read()
            for m in re.finditer(r'registerCheck<\s*([\w:]+)\s*>\(\s*"([^"]+)"', src):
                names.setdefault(m.group(1).split("::")[-1], []).append(m.group(2))
signals = {
    "spelled": r"TK_IgnoreUnlessSpelledInSource",
    "asis": r"TK_AsIs",
    "filter": r"isInTemplateInstantiation|isTemplateInstantiation\(\)|isInstantiated\(\)|isInstantiationDependent|TSK_ImplicitInstantiation|isTemplateSpecializationKind|getTemplateSpecializationKind|->isTemplated\(\)|isDependentContext|isDependentType\(\)|isTemplateDecl\(\)|isTemplated\(\)",
    "joint": r"getTemplateInstantiationPattern|getInstantiatedFrom\w*|specializations\(\)|getSpecializedTemplate|getDescribedTemplate|getDescribedClassTemplate|getDescribedFunctionTemplate|getMemberSpecializationInfo|getPrimaryTemplate|getTemplateSpecializationArgs|getTemplateArgs\(\)|forallBases",
    "end_tu": r"onEndOfTranslationUnit|onStartOfTranslationUnit",
    "pp": r"registerPPCallbacks",
    "tu_anchor": r"addMatcher\(\s*(?:ast_matchers::)?translationUnitDecl\(\)",
    "container_root": r"addMatcher\(\s*(?:ast_matchers::)?(?:namespaceDecl|linkageSpecDecl|exportDecl)\(",
}
rows = []
for dirpath, _, files in os.walk(root):
    for f in files:
        if not f.endswith(".cpp") or f.endswith("Module.cpp"):
            continue
        path = os.path.join(dirpath, f)
        src = open(path, encoding="utf-8", errors="replace").read()
        classes = re.findall(r"\b(\w+Check)\b(?:::|\s*\()", src)
        cls = None
        for c in classes:
            if c in names:
                cls = c
                break
        if cls is None:
            m = re.match(r"(\w+)\.cpp", f)
            cls = m.group(1)
        check = ",".join(names.get(cls, ["?"]))
        found = {k: len(re.findall(v, src)) for k, v in signals.items()}
        # member containers as accumulated state: look in header
        hdr = path[:-4] + ".h"
        state = 0
        if os.path.exists(hdr):
            h = open(hdr, encoding="utf-8", errors="replace").read()
            state = len(
                re.findall(
                    r"^\s*(?:llvm::)?(?:DenseMap|DenseSet|SmallPtrSet|SmallVector|std::vector|std::map|std::set|std::unordered_map|std::unordered_set|StringMap|StringSet|SmallDenseMap|MapVector)<[^;]*>\s+\w+;",
                    h,
                    re.M,
                )
            )
        rows.append((check, os.path.relpath(path, root), found, state))
rows.sort()
c = collections.Counter()
for check, path, found, state in rows:
    kind = "spelled" if found["spelled"] else ("asis" if found["asis"] else "default")
    c[kind] += 1
print("total", len(rows), dict(c))
print("with filter:", sum(1 for r in rows if r[2]["filter"]))
print("with joint:", sum(1 for r in rows if r[2]["joint"]))
print("with end_tu:", sum(1 for r in rows if r[2]["end_tu"]))
print(
    "matching the TU itself (must be TU-level):",
    [r[0] for r in rows if r[2]["tu_anchor"]],
)
print(
    "rooted at a container (must be TU-level):",
    [r[0] for r in rows if r[2]["container_root"]],
)
print("with state members:", sum(1 for r in rows if r[3]))
print("unmapped names:", sum(1 for r in rows if r[0] == "?"))

# The allowlist must name every registration of a listed check's class: an
# alias (cert-*, hicpp-*) is the same check under another name.
inc = os.path.join(repo, "src", "compile", "tidy_tu_checks.inc")
listed = re.findall(r'TU_LEVEL_CHECK\("([^"]+)"', open(inc).read())
by_name = {n: cls for cls, ns in names.items() for n in ns}
for n in listed:
    cls = by_name.get(n)
    if cls is None:
        print("allowlist names an unknown check:", n)
        continue
    for sibling in names[cls]:
        if sibling not in listed:
            print("allowlist misses alias", sibling, "of", n)
