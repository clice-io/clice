/// The GENERATED regions of a handwritten markdown page and the table
/// layout inside them, shared by the feature and configuration generators.

/// A region's markers. `begin` captures the region key in its first group
/// and, like `end`, must match an entire line, so marker text inside
/// generated content can neither open nor close a region.
export interface RegionMarkers {
    begin: RegExp;
    end: string;
}

/// Rewrites every region of `text`, its body replaced by `render(key)`
/// with a blank line on each side of a non-empty body; the page's own text
/// around the regions is kept. A duplicate key and a region without an end
/// marker are reported into `problems`; the unclosed region keeps its
/// original text, and so does everything after it. Returns the new text
/// and the keys seen, for the callers' coverage checks.
export function rewriteRegions(
    text: string,
    docPath: string,
    markers: RegionMarkers,
    render: (key: string) => string,
    problems: string[],
): { text: string; seen: Set<string> } {
    const lines = text.split("\n");
    const out: string[] = [];
    const seen = new Set<string>();

    let idx = 0;
    while (idx < lines.length) {
        const line = lines[idx] ?? "";
        const match = markers.begin.exec(line);
        if (!match) {
            out.push(line);
            idx += 1;
            continue;
        }
        const key = (match[1] ?? "").trim();
        if (seen.has(key)) {
            problems.push(`${docPath}: duplicate region '${key}'`);
        }
        seen.add(key);
        let end = idx + 1;
        while (end < lines.length && lines[end] !== markers.end) {
            end += 1;
        }
        if (end >= lines.length) {
            problems.push(`${docPath}: region '${key}' has no closing marker`);
            out.push(...lines.slice(idx));
            break;
        }
        out.push(line, "");
        const body = render(key);
        if (body) {
            out.push(body, "");
        }
        out.push(lines[end] ?? "");
        idx = end + 1;
    }
    return { text: out.join("\n"), seen };
}

/// A pipe table padded the way prettier formats markdown tables, so
/// `pixi run format` leaves the generated region untouched.
export function renderMarkdownTable(rows: readonly (readonly string[])[]): string[] {
    const widths: number[] = [];
    for (const row of rows) {
        row.forEach((cell, i) => {
            widths[i] = Math.max(widths[i] ?? 0, cell.length);
        });
    }
    const line = (cells: readonly string[]): string =>
        `| ${cells.map((cell, i) => cell.padEnd(widths[i] ?? 0)).join(" | ")} |`;
    const separator = widths.map((width) => "-".repeat(width));
    return [line(rows[0] ?? []), line(separator), ...rows.slice(1).map(line)];
}
