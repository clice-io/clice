"""A repository over a directory named by an environment variable."""

def _env_dir_impl(rctx):
    root = rctx.os.environ.get(rctx.attr.env)
    if not root:
        fail("set --repo_env=%s=<path>" % rctx.attr.env)
    for entry in rctx.path(root).readdir():
        rctx.symlink(entry, entry.basename)
    rctx.file("BUILD.bazel", rctx.read(rctx.attr.build_file))

env_dir_repository = repository_rule(
    implementation = _env_dir_impl,
    attrs = {
        "env": attr.string(mandatory = True),
        "build_file": attr.label(mandatory = True),
    },
    environ = ["CLICE_LLVM_ROOT"],
)
