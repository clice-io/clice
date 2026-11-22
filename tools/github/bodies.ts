/// Review comments and review bodies reduced to their findings: the badges,
/// AI prompts, walkthroughs, tracking markup and reaction footers the review
/// bots wrap them in are dropped.

/// The review bots; their review bodies can carry findings with no thread.
export const BOTS = new Set(["chatgpt-codex-connector", "coderabbitai", "github-actions"]);

/// `<details>` blocks whose summary starts with one of these (after any
/// emoji) are dropped with their content; every other block — a per-file
/// group such as `support/foo.cpp (2)` included — is unwrapped so nested
/// findings stay visible.
const NOISE_DETAILS = new RegExp(
    String.raw`^\W*(?:Prompt for|About Codex|Walkthrough|Review details|Review info|Run configuration` +
        String.raw`|Autofix|Commits|Configuration used|Files selected|Files ignored|Files skipped` +
        String.raw`|Additional context|Additional comments|Learnings|Tips|Support|Docs|Pre-merge checks` +
        String.raw`|Finishing touches|Comment @coderabbitai)\b(?![./])`,
    "i",
);

/// The markup bots wrap findings in. Anything else between angle brackets
/// is code (`std::vector<int>`, `x < 0 && y > 1`) and stays.
const HTML_TAGS =
    /<\/?(?:a|b|blockquote|br|code|dd|details|div|dl|dt|em|h[1-6]|hr|i|img|li|ol|p|picture|pre|source|span|strong|sub|summary|sup|table|tbody|td|th|thead|tr|ul)\b[^>\n]*>/gi;

const CODE = /```[\s\S]*?```|`[^`\n]*`/g;

const NOISE_LINES =
    /Useful\? React with|Codex Review$|automated review suggestions|Reviewed commit:|^-{3,}$/;

const INNERMOST_DETAILS = /<details>(?:(?!<details>)[\s\S])*?<\/details>/g;

function stripDetails(text: string): string {
    // Innermost blocks first, so nested blocks unwrap correctly.
    for (let result = text; ; ) {
        const next = result.replace(INNERMOST_DETAILS, (block) => {
            const summary = /<summary>([\s\S]*?)<\/summary>/.exec(block)?.[1];
            if (summary !== undefined && NOISE_DETAILS.test(summary.replace(HTML_TAGS, ""))) {
                return "";
            }
            return block
                .replace(/<summary>([\s\S]*?)<\/summary>/g, "$1\n")
                .replace(/<\/?details>/g, "");
        });
        if (next === result) {
            return result;
        }
        result = next;
    }
}

/// The markup outside code spans and fenced blocks removed.
function stripMarkup(text: string): string {
    const spans: string[] = [];
    const stashed = text.replace(CODE, (span) => {
        spans.push(span);
        return `\u0000${spans.length - 1}\u0000`;
    });
    return stashed
        .replace(HTML_TAGS, "")
        .replace(/\u0000(\d+)\u0000/g, (_, index: string) => spans[Number(index)] ?? "");
}

/// A comment or review body reduced to its findings text, with the
/// severity of the codex badge (P1..P3) or of CodeRabbit (Critical, Major,
/// Minor, Trivial) when it has one.
export function clean(body: string): { severity: string; text: string } {
    const severity =
        /!\[(P\d) Badge\]/.exec(body)?.[1] ??
        /_[^_\n]*?(Critical|Major|Minor|Trivial)_/.exec(body)?.[1] ??
        "";
    let text = body.replace(/<!--[\s\S]*?-->/g, "");
    text = stripDetails(text);
    text = text.replace(/!\[[^\]]*\]\([^)]*\)/g, "");
    text = text.replace(
        /https:\/\/github\.com\/[^/\s]+\/[^/\s]+\/blob\/[0-9a-f]+\/([^\s#]+)#L(\d+)(?:-L(\d+))?/g,
        (_, file: string, first: string, last: string | undefined) =>
            `${file}:${first}${last === undefined ? "" : `-${last}`}`,
    );
    text = stripMarkup(text);
    text = text.replace(/^(> ?)+/gm, "");
    text = text.replace(/^\[!(?:CAUTION|NOTE|WARNING|TIP|IMPORTANT)\]\s*$/gm, "");
    text = text.replace(/^\*\*[ \t]+/gm, "**");
    const lines = text
        .split("\n")
        .filter((line) => !NOISE_LINES.test(line.trim()))
        .map((line) => line.trimEnd());
    return {
        severity,
        text: lines
            .join("\n")
            .trim()
            .replace(/\n{3,}/g, "\n\n"),
    };
}

/// A bot's review body counts when it carries a located finding rather than
/// a per-commit "reviewed" notice or an all-clear; people's review bodies
/// are not findings.
export function isFinding(author: string, cleaned: string): boolean {
    return BOTS.has(author) && /\S+:\d+|Outside diff range|Nitpick/.test(cleaned);
}

export function truncate(text: string, limit: number): string {
    return limit === 0 || text.length <= limit ? text : `${text.slice(0, limit - 1).trimEnd()}…`;
}

export function indent(text: string, prefix = "    "): string {
    return text
        .split("\n")
        .map((line) => (line === "" ? "" : prefix + line))
        .join("\n");
}
