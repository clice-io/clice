/// The command-line values the PR, CI and triage readers take, checked: a
/// value that does not parse is a usage error naming it, never a guess.

function usage(message: string): never {
    console.error(message);
    process.exit(2);
}

/// A whole number of zero or more.
export function count(value: string, name: string): number {
    const number = Number(value);
    if (value.trim() === "" || !Number.isInteger(number) || number < 0) {
        usage(`${name} takes a whole number, not ${JSON.stringify(value)}`);
    }
    return number;
}

/// An issue or PR number.
export function issueNumber(value: string, name: string): number {
    const number = count(value, name);
    if (number === 0) {
        usage(`${name} takes an issue or PR number, not 0`);
    }
    return number;
}

/// Comma-separated issue numbers; an absent option is none.
export function issueNumbers(value: string | undefined, name: string): Set<number> {
    return new Set(
        value === undefined ? [] : value.split(",").map((each) => issueNumber(each, name)),
    );
}

/// One of `allowed`.
export function oneOf<T extends string>(value: string, name: string, allowed: readonly T[]): T {
    const found = allowed.find((each) => each === value);
    if (found === undefined) {
        usage(`${name} takes ${allowed.join(" or ")}, not ${JSON.stringify(value)}`);
    }
    return found;
}

/// The positionals after the command, at most `most` of them.
export function atMost(positionals: readonly string[], most: number, help: string): string[] {
    if (positionals.length > most) {
        usage(`unexpected ${positionals.slice(most).join(" ")}\n${help}`);
    }
    return [...positionals];
}
