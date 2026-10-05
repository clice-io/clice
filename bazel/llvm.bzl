"""libclang's libraries with their ThinLTO bitcode compiled to native code.

xclang's libclang archives hold ThinLTO bitcode, so every link of them redoes
LLVM's code generation: minutes per program. Builds that do not ship link the
archives compiled once each, by a cacheable action per archive;
--config=release links the bitcode as clice is released.
"""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

# Archives can hold several members of one name (clangDriver has two
# AMDGPU.cpp.o). Bitcode starts with BC, or with the wrapper Darwin targets put
# it in; other members stay as they are. The sections are those lld's LTO
# would give, for --gc-sections to drop what LTO drops. The members compile
# in parallel: the largest archives would otherwise end every cold build alone
# on one core for minutes.
_COMPILE = r"""
set -eu
clang=$PWD/$1 ar=$PWD/$2 in=$PWD/$3 out=$PWD/$4
jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
"$ar" x "$in"
"$ar" t "$in" | tr -d '\r' | sort | uniq -c | while read -r count member; do
    [ "$count" -gt 1 ] || continue
    rm -f "$member"
    for k in $(seq "$count"); do "$ar" xN "$k" "$in" "$member" && mv "$member" "${k}_$member"; done
done
for f in *; do
    case $(od -An -tx1 -N4 "$f" | tr -d ' \r\n') in
        4243c0de | dec0170b) printf '%s\0' "$f" ;;
    esac
done | xargs -0 -r -n 1 -P "$jobs" "$BASH" -c \
    '"$0" -c -O2 -ffunction-sections -fdata-sections -x ir "$1" -o "$1.o" && mv "$1.o" "$1"' "$clang"
"$ar" rcs "$out" *
"""

def _all_cores(_os, _inputs):
    # What the actions of a runner have: the action runs alone when it asks
    # for more than the machine has.
    return {"cpu": 4}

def _native_libraries_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    cc_info = cc_common.merge_cc_infos(cc_infos = [dep[CcInfo] for dep in ctx.attr.deps])
    linker_inputs = []
    for linker_input in cc_info.linking_context.linker_inputs.to_list():
        libraries = []
        for library in linker_input.libraries:
            archive = library.static_library or library.pic_static_library
            native = ctx.actions.declare_file("%s/%s" % (ctx.label.name, archive.basename))
            ctx.actions.run_shell(
                command = _COMPILE,
                arguments = [cc_toolchain.compiler_executable, cc_toolchain.ar_executable, archive.path, native.path],
                inputs = depset([archive], transitive = [cc_toolchain.all_files]),
                outputs = [native],
                mnemonic = "LlvmNativeArchive",
                resource_set = _all_cores,
                progress_message = "Compiling the bitcode of %s to native code" % archive.basename,
            )
            libraries.append(cc_common.create_library_to_link(
                actions = ctx.actions,
                feature_configuration = feature_configuration,
                cc_toolchain = cc_toolchain,
                static_library = native,
                alwayslink = library.alwayslink,
            ))
        linker_inputs.append(cc_common.create_linker_input(
            owner = ctx.label,
            libraries = depset(libraries),
            user_link_flags = linker_input.user_link_flags,
            additional_inputs = depset(linker_input.additional_inputs),
        ))
    return [CcInfo(
        compilation_context = cc_info.compilation_context,
        linking_context = cc_common.create_linking_context(linker_inputs = depset(linker_inputs)),
    )]

native_libraries = rule(
    implementation = _native_libraries_impl,
    attrs = {
        "deps": attr.label_list(providers = [CcInfo], doc = "libclang's libraries, with their dependencies"),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
    doc = "The libraries of deps and their dependencies, every archive compiled to native code.",
)
