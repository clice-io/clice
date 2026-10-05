/// # Declaration-line folding
///
/// - status: supported
/// - issues: clangd#2666
///
/// A declaration's body folds from the line holding its name
///
/// When the opening brace of a function, class or namespace sits below the
/// name — on a line of its own, or after a signature spanning several lines —
/// a client that folds whole lines starts the fold on the name's line, so the
/// folded declaration keeps showing what it is. A client folding by
/// characters starts at the brace and keeps everything before it visible
/// anyway. A conditional directive between the name and the brace keeps the
/// fold at the brace.
///
/// > **Client support**: VS Code still leaves the closing `}` on a separate
/// > line rather than collapsing it onto the signature line
/// > ([vscode#3352](https://github.com/microsoft/vscode/issues/3352) — still
/// > open).

// snap: The snapshot pins character ranges, which still start at the brace;
// snap: the start on the name's line for line-folding clients is pinned by
// snap: tests/integration/features/folding_range.test.ts.

struct Config {
    int width;
    int height;
};

int process_data(const Config& cfg)
{
    int area = cfg.width * cfg.height;
    return area;
}

static int
scale(const Config& cfg,
      int factor)
{
    return cfg.width * factor;
}

class Renderer
    : public Config
{
    int frames;
};
