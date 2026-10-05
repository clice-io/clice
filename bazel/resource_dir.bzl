"""clang's resource directory where clice looks for it: lib/clang next to the
directory of its executable, which is bin/ in the package of the rule."""

def _resource_dir_impl(ctx):
    files = []
    for file in ctx.files.srcs:
        _, _, path = file.short_path.partition("/lib/clang/")
        link = ctx.actions.declare_file("lib/clang/" + path)
        ctx.actions.symlink(output = link, target_file = file)
        files.append(link)
    return [DefaultInfo(files = depset(files))]

resource_dir = rule(
    implementation = _resource_dir_impl,
    attrs = {"srcs": attr.label_list(allow_files = True)},
)
