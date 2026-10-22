/// A raw C/C++ lexer: where tokens begin and end, which the nightly sweep
/// and the benchmark need to pick the places they ask at.

export type TokenKind = "ident" | "number" | "string" | "char" | "comment" | "punct" | "header";

export interface Token {
    kind: TokenKind;
    begin: number;
    end: number;
    /// It is part of a directive line.
    directive: boolean;
}

const PUNCTUATORS = [
    "%:%:",
    "...",
    "<=>",
    "->*",
    "<<=",
    ">>=",
    "::",
    "->",
    ".*",
    "++",
    "--",
    "<<",
    ">>",
    "<=",
    ">=",
    "==",
    "!=",
    "&&",
    "||",
    "+=",
    "-=",
    "*=",
    "/=",
    "%=",
    "&=",
    "|=",
    "^=",
    "##",
    "<:",
    ":>",
    "<%",
    "%>",
    "%:",
];

const ENCODING_PREFIXES = new Set(["u8", "u", "U", "L", "R", "u8R", "uR", "UR", "LR"]);

export const KEYWORDS = new Set(
    (
        "alignas alignof asm auto bool break case catch char char8_t char16_t char32_t class " +
        "concept const consteval constexpr constinit const_cast continue co_await co_return " +
        "co_yield decltype default delete do double dynamic_cast else enum explicit export " +
        "extern false float for friend goto if inline int long mutable namespace new noexcept " +
        "nullptr operator private protected public register reinterpret_cast requires return " +
        "short signed sizeof static static_assert static_cast struct switch template this " +
        "thread_local throw true try typedef typeid typename union unsigned using virtual void " +
        "volatile wchar_t while"
    ).split(" "),
);

function isIdentChar(c: number): boolean {
    return (
        (c >= 0x30 && c <= 0x39) ||
        (c >= 0x41 && c <= 0x5a) ||
        (c >= 0x61 && c <= 0x7a) ||
        c === 0x5f ||
        c === 0x24 ||
        c >= 0x80
    );
}

function isSpace(c: number): boolean {
    return c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d || c === 0x0b || c === 0x0c;
}

/// A raw lex, good enough to tell token boundaries apart from the inside
/// of a token: comments, literals with prefixes, raw strings and suffixes,
/// header names, pp-numbers and the longest punctuators.
export function lex(text: string): Token[] {
    const tokens: Token[] = [];
    let i = 0;
    let lineStart = true;
    let directive: string | null = null;
    const n = text.length;
    const code = (k: number) => (k < n ? text.charCodeAt(k) : -1);

    const quoted = (start: number, quote: string): number => {
        let k = start;
        while (k < n && text[k] !== quote && text[k] !== "\n") {
            k += text[k] === "\\" ? 2 : 1;
        }
        return Math.min(n, k + 1);
    };
    const suffix = (k: number): number => {
        while (k < n && isIdentChar(code(k))) {
            k += 1;
        }
        return k;
    };

    while (i < n) {
        const c = code(i);
        if (isSpace(c)) {
            if (c === 0x0a) {
                if (
                    directive !== null &&
                    i > 0 &&
                    text[i - 1] !== "\\" &&
                    text.slice(i - 2, i) !== "\\\r"
                ) {
                    directive = null;
                }
                lineStart = true;
            }
            i += 1;
            continue;
        }
        const start = i;
        let kind: TokenKind;
        if (text.startsWith("//", i)) {
            while (i < n && text[i] !== "\n") {
                i += 1;
            }
            if (text[i - 1] === "\r") {
                i -= 1;
            }
            kind = "comment";
        } else if (text.startsWith("/*", i)) {
            const close = text.indexOf("*/", i + 2);
            i = close < 0 ? n : close + 2;
            kind = "comment";
        } else if (c === 0x22 /* " */) {
            i = suffix(quoted(i + 1, '"'));
            kind = "string";
        } else if (c === 0x27 /* ' */) {
            i = suffix(quoted(i + 1, "'"));
            kind = "char";
        } else if (
            c === 0x3c /* < */ &&
            directive !== null &&
            /^(include|include_next|import)$/.test(directive)
        ) {
            const close = text.indexOf(">", i);
            const eol = text.indexOf("\n", i);
            i = close < 0 || (eol >= 0 && eol < close) ? i + 1 : close + 1;
            kind = i - start > 1 ? "header" : "punct";
        } else if (isIdentChar(c) && !(c >= 0x30 && c <= 0x39)) {
            i = suffix(i);
            const word = text.slice(start, i);
            if (
                ENCODING_PREFIXES.has(word) &&
                (text[i] === '"' || (text[i] === "'" && !word.endsWith("R")))
            ) {
                if (word.endsWith("R")) {
                    const open = text.indexOf("(", i);
                    const delimiter = text.slice(i + 1, open);
                    const close = text.indexOf(`)${delimiter}"`, open);
                    i = close < 0 ? n : close + delimiter.length + 2;
                    i = suffix(i);
                } else {
                    i = suffix(quoted(i + 1, text[i] ?? '"'));
                }
                kind = text[start + word.length] === "'" ? "char" : "string";
            } else {
                kind = "ident";
                if (directive === "") {
                    directive = word;
                }
            }
        } else if (
            (c >= 0x30 && c <= 0x39) ||
            (c === 0x2e && code(i + 1) >= 0x30 && code(i + 1) <= 0x39)
        ) {
            i += 1;
            while (i < n) {
                const d = code(i);
                if ((d === 0x2b || d === 0x2d) && /[eEpP]/.test(text[i - 1] ?? "")) {
                    i += 1;
                } else if (d === 0x27 && isIdentChar(code(i + 1))) {
                    i += 2;
                } else if (isIdentChar(d) || d === 0x2e) {
                    i += 1;
                } else {
                    break;
                }
            }
            kind = "number";
        } else {
            // `<::` is `<` `::` unless a `:` or `>` follows ([lex.pptoken]).
            const digraph = text.startsWith("<::", i) && !/[:>]/.test(text[i + 3] ?? "");
            const punctuator = digraph ? "<" : PUNCTUATORS.find((p) => text.startsWith(p, i));
            i += punctuator?.length ?? 1;
            kind = "punct";
            if (c === 0x23 /* # */ && lineStart) {
                directive = "";
            }
        }
        lineStart = false;
        tokens.push({ kind, begin: start, end: i, directive: directive !== null });
    }
    return tokens;
}
