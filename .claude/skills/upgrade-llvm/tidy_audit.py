import re, os, sys, json, collections

root = os.path.expanduser("~/workspace/llvm-project/clang-tools-extra/clang-tidy")
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
json.dump(rows, open("/tmp/tidy-audit/rows.json", "w"), indent=0)
c = collections.Counter()
for check, path, found, state in rows:
    kind = "spelled" if found["spelled"] else ("asis" if found["asis"] else "default")
    c[kind] += 1
print("total", len(rows), dict(c))
print("with filter:", sum(1 for r in rows if r[2]["filter"]))
print("with joint:", sum(1 for r in rows if r[2]["joint"]))
print("with end_tu:", sum(1 for r in rows if r[2]["end_tu"]))
print("with state members:", sum(1 for r in rows if r[3]))
print("unmapped names:", sum(1 for r in rows if r[0] == "?"))
