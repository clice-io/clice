"""xclang's libclang package as a Bazel repository, and its LTO archives as
native ones.

The package holds ThinLTO bitcode: every link of it redoes LLVM's codegen
(minutes). Builds that do not ship link archives converted to native objects
once per archive, as cacheable actions; --define=llvm=bitcode links the
package as released.
"""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")

# SHA256SUMS of the xclang release.
_SHA256 = {
    "23.1.2.4": {
        "aarch64-apple-darwin": "dc0021738f91acd7b752289b3da33685b5e8bf2be67c14e49fecc8b6ba1a196a",
        "aarch64-unknown-linux-gnu": "2cae39780465bf5eb3872c1920b1757129eee7c94be55ac1cfe95abed6e1cbc6",
        "aarch64-w64-mingw32": "21e939877aee160ace81ff0e332cbffd3e47333010479bca8937f64d5818502a",
        "x86_64-apple-darwin": "13f61218404aed2a55f2dbc86af0fb6fdb95c4b98a50857f636d21de0c7d5e78",
        "x86_64-unknown-linux-gnu": "a306dd638c244488bdbd6efe6b70a823e2498b0afe4a7820fc4d7ac47e2446a2",
        "x86_64-w64-mingw32": "bd528c43b039fc463b6befa488f067bfc8cb7a9d7f933dd79716aa2160a7ede7",
    },
}

def _host_triple(rctx):
    arch = "aarch64" if rctx.os.arch in ("aarch64", "arm64") else "x86_64"
    os = rctx.os.name.lower()
    if os.startswith("windows"):
        return arch + "-w64-mingw32"
    if os.startswith("mac"):
        return arch + "-apple-darwin"
    return arch + "-unknown-linux-gnu"

def _libclang_impl(rctx):
    version = rctx.attr.version
    triple = _host_triple(rctx)
    rctx.download_and_extract(
        url = "https://github.com/clice-io/xclang/releases/download/%s/libclang-%s-%s.tar.xz" % (version, version, triple),
        sha256 = _SHA256[version][triple],
        stripPrefix = "libclang",
    )
    rctx.file("BUILD.bazel", rctx.read(rctx.attr.build_file))

libclang_repository = repository_rule(
    implementation = _libclang_impl,
    attrs = {
        "build_file": attr.label(mandatory = True),
        "version": attr.string(mandatory = True),
    },
)

# Archives can hold several members of one name (clangDriver has two
# AMDGPU.cpp.o); bitcode starts with BC, or with the wrapper Darwin targets put
# it in. The sections are those lld's LTO would give, for --gc-sections.
_CONVERT = r"""
set -eu
clang=$1 ar=$2 in=$PWD/$3 out=$PWD/$4
work=$(mktemp -d)
cd "$work"
"$ar" x "$in"
"$ar" t "$in" | tr -d '\r' | sort | uniq -c | while read -r count member; do
    [ "$count" -gt 1 ] || continue
    rm -f "$member"
    for k in $(seq "$count"); do "$ar" xN "$k" "$in" "$member" && mv "$member" "${k}_$member"; done
done
for f in *; do
    case $(od -An -tx1 -N4 "$f" | tr -d ' \r\n') in
        4243c0de | dec0170b)
            "$clang" -c -O2 -ffunction-sections -fdata-sections -x ir "$f" -o "$f.native"
            rm "$f"
            ;;
    esac
done
"$ar" rcs "$out" *
cd / && rm -rf "$work"
"""

def _native_archives_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    outs = []
    for archive in ctx.files.srcs:
        out = ctx.actions.declare_file("native/" + archive.basename)
        ctx.actions.run_shell(
            command = _CONVERT,
            arguments = [cc_toolchain.compiler_executable, cc_toolchain.ar_executable, archive.path, out.path],
            inputs = [archive],
            outputs = [out],
            mnemonic = "LlvmNativeArchive",
            progress_message = "Compiling the bitcode of %s to native code" % archive.basename,
        )
        outs.append(out)
    return [DefaultInfo(files = depset(outs))]

native_archives = rule(
    implementation = _native_archives_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".a"]),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
)
