/// Annotated-source parser mirroring tests/unit/test/annotation.cpp.
///
/// Fixture files may carry inline annotations: `§` / `§(name)` points and
/// `§⟦...⟧` / `§(name)⟦...⟧` ranges (nesting allowed). The unit Tester strips
/// them before compiling; the integration harness strips them before didOpen,
/// so the server compiles the same clean text while the test keeps the marked
/// offsets. Files without annotations pass through unchanged.
///
/// Offsets are byte offsets in the stripped UTF-8 text, matching the C++ side.

const SIGIL = Buffer.from("§");
const OPEN_MARK = Buffer.from("⟦");
const CLOSE_MARK = Buffer.from("⟧");

export interface AnnotatedSource {
    content: string;
    offsets: Map<string, number>;
    ranges: Map<string, [number, number]>;
    namelessOffsets: number[];
}

function startsWith(data: Buffer, prefix: Buffer, at: number): boolean {
    return data.subarray(at, at + prefix.length).equals(prefix);
}

export function parseAnnotations(text: string): AnnotatedSource {
    const data = Buffer.from(text);
    const source: number[] = [];
    const offsets = new Map<string, number>();
    const ranges = new Map<string, [number, number]>();
    const namelessOffsets: number[] = [];
    const stack: [string, number][] = [];

    let i = 0;
    while (i < data.length) {
        if (startsWith(data, SIGIL, i)) {
            i += SIGIL.length;

            let key = "";
            if (i < data.length && data[i] === 0x28 /* ( */) {
                const keyEnd = data.indexOf(0x29 /* ) */, i + 1);
                if (keyEnd < 0) {
                    throw new Error("Unterminated §(name) in annotated source.");
                }
                key = data.subarray(i + 1, keyEnd).toString();
                // ASCII-only like the C++ twin's std::isalnum.
                if (!/^[A-Za-z0-9_]*$/.test(key)) {
                    throw new Error(
                        `§(${key}) is not an identifier name; use §() for a ` +
                            "nameless point before real parentheses.",
                    );
                }
                i = keyEnd + 1;
            }

            if (startsWith(data, OPEN_MARK, i)) {
                stack.push([key, source.length]);
                i += OPEN_MARK.length;
            } else if (!key) {
                namelessOffsets.push(source.length);
            } else if (offsets.has(key)) {
                throw new Error(`Duplicate point annotation §(${key}).`);
            } else {
                offsets.set(key, source.length);
            }
            continue;
        }

        if (startsWith(data, CLOSE_MARK, i)) {
            const open = stack.pop();
            if (open === undefined) {
                throw new Error("`⟧` without a matching `§⟦` in annotated source.");
            }
            const [key, begin] = open;
            if (ranges.has(key)) {
                throw new Error(
                    key
                        ? `Duplicate range annotation §(${key})⟦...⟧.`
                        : "Multiple nameless ranges in one source; name them instead.",
                );
            }
            ranges.set(key, [begin, source.length]);
            i += CLOSE_MARK.length;
            continue;
        }

        if (startsWith(data, OPEN_MARK, i)) {
            throw new Error("`⟦` must follow `§` or `§(name)`.");
        }

        source.push(data.readUInt8(i));
        i += 1;
    }

    if (stack.length > 0) {
        throw new Error("Unclosed `§⟦` annotation at end of input.");
    }

    return {
        content: Buffer.from(source).toString(),
        offsets,
        ranges,
        namelessOffsets,
    };
}
