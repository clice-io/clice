"""The run layout CMake builds give: bin/<executables> next to lib/clang, the
resource directory clice derives from its own path."""

def _dist_impl(ctx):
    outs = []
    for exe in ctx.files.binaries:
        out = ctx.actions.declare_file("%s/bin/%s" % (ctx.label.name, exe.basename))

        # A real file, not a symlink: getMainExecutable resolves symlinks, and
        # the resource directory is looked up next to the resolved path.
        ctx.actions.run_shell(
            command = 'ln -L "$1" "$2" 2>/dev/null || cp "$1" "$2"',
            arguments = [exe.path, out.path],
            inputs = [exe],
            outputs = [out],
            mnemonic = "CliceDist",
            progress_message = "Placing %s" % exe.basename,
        )
        outs.append(out)
    for f in ctx.files.resource_dir:
        rel = f.path[f.path.find("lib/clang/"):]
        out = ctx.actions.declare_file("%s/%s" % (ctx.label.name, rel))
        ctx.actions.symlink(output = out, target_file = f)
        outs.append(out)
    return [DefaultInfo(files = depset(outs), runfiles = ctx.runfiles(files = outs))]

clice_dist = rule(
    implementation = _dist_impl,
    attrs = {
        "binaries": attr.label_list(allow_files = True),
        "resource_dir": attr.label(allow_files = True),
    },
)
