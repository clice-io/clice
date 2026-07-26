"""Annotated-source parser mirroring tests/unit/test/annotation.cpp.

Fixture files may carry inline annotations: `§` / `§(name)` points and
`§⟦...⟧` / `§(name)⟦...⟧` ranges (nesting allowed). The unit Tester strips
them before compiling; the integration harness strips them before didOpen,
so the server compiles the same clean text while the test keeps the marked
offsets. Files without annotations pass through unchanged.

Offsets are byte offsets in the stripped UTF-8 text, matching the C++ side.
"""

from dataclasses import dataclass, field

SIGIL = "§".encode()
OPEN_MARK = "⟦".encode()
CLOSE_MARK = "⟧".encode()


@dataclass
class AnnotatedSource:
    content: str
    offsets: dict[str, int] = field(default_factory=dict)
    ranges: dict[str, tuple[int, int]] = field(default_factory=dict)
    nameless_offsets: list[int] = field(default_factory=list)


def parse_annotations(text: str) -> AnnotatedSource:
    data = text.encode()
    source = bytearray()
    offsets: dict[str, int] = {}
    ranges: dict[str, tuple[int, int]] = {}
    nameless: list[int] = []
    stack: list[tuple[str, int]] = []

    i = 0
    while i < len(data):
        if data.startswith(SIGIL, i):
            i += len(SIGIL)

            key = ""
            if i < len(data) and data[i : i + 1] == b"(":
                key_end = data.find(b")", i + 1)
                if key_end < 0:
                    raise ValueError("Unterminated §(name) in annotated source.")
                key = data[i + 1 : key_end].decode()
                # ASCII-only like the C++ twin's std::isalnum; str.isalnum
                # would accept non-ASCII letters the unit Tester rejects.
                if not all(c.isascii() and (c.isalnum() or c == "_") for c in key):
                    raise ValueError(
                        f"§({key}) is not an identifier name; use §() for a "
                        "nameless point before real parentheses."
                    )
                i = key_end + 1

            if data.startswith(OPEN_MARK, i):
                stack.append((key, len(source)))
                i += len(OPEN_MARK)
            elif not key:
                nameless.append(len(source))
            elif key in offsets:
                raise ValueError(f"Duplicate point annotation §({key}).")
            else:
                offsets[key] = len(source)
            continue

        if data.startswith(CLOSE_MARK, i):
            if not stack:
                raise ValueError("`⟧` without a matching `§⟦` in annotated source.")
            key, begin = stack.pop()
            if key in ranges:
                raise ValueError(
                    f"Duplicate range annotation §({key})⟦...⟧."
                    if key
                    else "Multiple nameless ranges in one source; name them instead."
                )
            ranges[key] = (begin, len(source))
            i += len(CLOSE_MARK)
            continue

        if data.startswith(OPEN_MARK, i):
            raise ValueError("`⟦` must follow `§` or `§(name)`.")

        source.append(data[i])
        i += 1

    if stack:
        raise ValueError("Unclosed `§⟦` annotation at end of input.")

    return AnnotatedSource(source.decode(), offsets, ranges, nameless)
