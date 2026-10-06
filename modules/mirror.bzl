"""Empty headers, created at build time from a list."""

def _mirror_impl(ctx):
    files = []
    for header in ctx.attr.headers:
        file = ctx.actions.declare_file("mirror/%s/%s" % (ctx.attr.library, header))
        ctx.actions.write(file, "")
        files.append(file)
    return [DefaultInfo(files = depset(files))]

mirror = rule(
    implementation = _mirror_impl,
    doc = "mirror/<library>/<header> for each header, empty, in the package's output directory.",
    attrs = {
        "headers": attr.string_list(mandatory = True),
        "library": attr.string(mandatory = True),
    },
)
