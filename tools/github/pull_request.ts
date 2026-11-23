/// A PR read in one place, and its review threads resolved (the pull-request skill).
///
/// Usage:
///   node tools/github/pull_request.ts status [PR] [--commits <n>]
///   node tools/github/pull_request.ts threads [PR] [--all] [--max-chars <n>]
///   node tools/github/pull_request.ts reply <thread id> <text>
///   node tools/github/pull_request.ts resolve <thread id>...
///
/// The PR defaults to the current branch's. `status` prints its state,
/// whether it merges, the CI result (tools/github/ci.ts), the review load
/// and the last commits. `threads` prints every unresolved review thread
/// (`--all`: the resolved too) and the findings bots post in review bodies
/// instead of threads, each reduced to its author, place, severity and text
/// (`--max-chars`, default 4000; 0 keeps everything).

import { parseArgs } from "node:util";
import { clean, indent, isFinding, truncate } from "./bodies.ts";
import { statusOf } from "./ci.ts";
import { currentPr, ghJson, graphql, login, repo } from "./gh.ts";
import { atMost, count, issueNumber } from "./options.ts";

interface Comment {
    author: { login: string } | null;
    body: string;
    url: string;
}

interface Connection<T> {
    pageInfo: { hasNextPage: boolean; endCursor: string | null };
    nodes: T[];
}

interface Thread {
    id: string;
    isResolved: boolean;
    isOutdated: boolean;
    path: string;
    line: number | null;
    originalLine: number | null;
    comments: Connection<Comment>;
}

interface Review {
    author: { login: string } | null;
    state: string;
    body: string;
    url: string;
    commit: { oid: string } | null;
}

const THREADS_QUERY = `
query($owner: String!, $name: String!, $pr: Int!, $after: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $pr) {
      title
      reviewThreads(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id isResolved isOutdated path line originalLine
          comments(first: 100) {
            pageInfo { hasNextPage endCursor }
            nodes { author { login } body url }
          }
        }
      }
    }
  }
}`;

const THREAD_COMMENTS_QUERY = `
query($id: ID!, $after: String) {
  node(id: $id) {
    ... on PullRequestReviewThread {
      comments(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes { author { login } body url }
      }
    }
  }
}`;

const REVIEWS_QUERY = `
query($owner: String!, $name: String!, $pr: Int!, $before: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $pr) {
      reviews(last: 50, before: $before) {
        pageInfo { hasPreviousPage startCursor }
        nodes { author { login } state body url commit { oid } }
      }
    }
  }
}`;

const REPLY_MUTATION = `
mutation($id: ID!, $body: String!) {
  addPullRequestReviewThreadReply(input: { pullRequestReviewThreadId: $id, body: $body }) {
    comment { url }
  }
}`;

const RESOLVE_MUTATION = `
mutation($id: ID!) {
  resolveReviewThread(input: { threadId: $id }) { thread { id isResolved } }
}`;

async function fetchThreads(pr: number): Promise<{ title: string; threads: Thread[] }> {
    const { owner, name } = await repo();
    const threads: Thread[] = [];
    let title = "";
    for (let after: string | undefined; ; ) {
        const page = (
            await graphql<{
                repository: { pullRequest: { title: string; reviewThreads: Connection<Thread> } };
            }>(THREADS_QUERY, { owner, name, pr, after })
        ).repository.pullRequest;
        title = page.title;
        threads.push(...page.reviewThreads.nodes);
        if (!page.reviewThreads.pageInfo.hasNextPage) {
            break;
        }
        after = page.reviewThreads.pageInfo.endCursor ?? undefined;
    }
    for (const thread of threads) {
        while (thread.comments.pageInfo.hasNextPage) {
            const more = (
                await graphql<{ node: { comments: Connection<Comment> } }>(THREAD_COMMENTS_QUERY, {
                    id: thread.id,
                    after: thread.comments.pageInfo.endCursor ?? undefined,
                })
            ).node.comments;
            thread.comments.nodes.push(...more.nodes);
            thread.comments.pageInfo = more.pageInfo;
        }
    }
    return { title, threads };
}

async function fetchReviews(pr: number): Promise<Review[]> {
    const { owner, name } = await repo();
    let reviews: Review[] = [];
    for (let before: string | undefined; ; ) {
        const page = (
            await graphql<{
                repository: {
                    pullRequest: {
                        reviews: {
                            pageInfo: { hasPreviousPage: boolean; startCursor: string | null };
                            nodes: Review[];
                        };
                    };
                };
            }>(REVIEWS_QUERY, { owner, name, pr, before })
        ).repository.pullRequest.reviews;
        reviews = [...page.nodes, ...reviews];
        if (!page.pageInfo.hasPreviousPage) {
            return reviews;
        }
        before = page.pageInfo.startCursor ?? undefined;
    }
}

/// The reviews whose bodies carry a bot's finding, cleaned.
function findings(
    reviews: readonly Review[],
): { review: Review; severity: string; text: string }[] {
    return reviews.flatMap((review) => {
        const { severity, text } = clean(review.body);
        return isFinding(login(review), text) ? [{ review, severity, text }] : [];
    });
}

async function status(pr: number, commits: number): Promise<void> {
    const view = await ghJson<{
        number: number;
        title: string;
        state: string;
        isDraft: boolean;
        url: string;
        headRefName: string;
        baseRefName: string;
        headRefOid: string;
        author: { login: string } | null;
        mergeable: string;
        mergeStateStatus: string;
        reviewDecision: string;
        additions: number;
        deletions: number;
        changedFiles: number;
        updatedAt: string;
        labels: { name: string }[];
        commits: { oid: string; messageHeadline: string }[];
    }>([
        "pr",
        "view",
        String(pr),
        "--json",
        "number,title,state,isDraft,url,headRefName,baseRefName,headRefOid,author,mergeable," +
            "mergeStateStatus,reviewDecision,additions,deletions,changedFiles,updatedAt,labels,commits",
    ]);
    const { threads } = await fetchThreads(pr);
    const found = findings(await fetchReviews(pr));
    const onHead = found.filter(({ review }) => review.commit?.oid === view.headRefOid).length;
    const labels = view.labels.map((label) => label.name).join(", ");
    console.log(`#${view.number} ${view.title}`);
    console.log(
        `${view.state}${view.isDraft ? " (draft)" : ""} · ${view.headRefName} → ${view.baseRefName}` +
            ` · ${view.headRefOid.slice(0, 9)} · by ${login(view)}` +
            ` · +${view.additions} -${view.deletions} in ${view.changedFiles} files`,
    );
    console.log(
        `merge: ${view.mergeable} / ${view.mergeStateStatus} · review: ${view.reviewDecision || "none"}` +
            ` · updated ${view.updatedAt}${labels ? ` · labels: ${labels}` : ""}`,
    );
    console.log(
        (await statusOf(`CI @ ${view.headRefOid.slice(0, 9)}`, view.headRefOid)).join("\n"),
    );
    const unresolved = threads.filter((thread) => !thread.isResolved).length;
    console.log(
        `review threads: ${unresolved} unresolved of ${threads.length}` +
            ` · findings in review bodies: ${found.length}, ${onHead} on the head commit`,
    );
    console.log("last commits:");
    for (const commit of view.commits.slice(-commits)) {
        console.log(`  ${commit.oid.slice(0, 9)} ${commit.messageHeadline}`);
    }
    console.log(view.url);
}

async function printThreads(pr: number, all: boolean, maxChars: number): Promise<void> {
    const { title, threads } = await fetchThreads(pr);
    const found = findings(await fetchReviews(pr));
    const unresolved = threads.filter((thread) => !thread.isResolved).length;
    console.log(`PR #${pr} ${title}`);
    console.log(`review threads: ${unresolved} unresolved of ${threads.length}`);
    threads
        .filter((thread) => all || !thread.isResolved)
        .forEach((thread, index) => {
            const [first, ...replies] = thread.comments.nodes;
            if (first === undefined) {
                return;
            }
            const { severity, text } = clean(first.body);
            const line = thread.line ?? thread.originalLine;
            const flags = [
                thread.isResolved ? "resolved" : "",
                thread.isOutdated ? "outdated" : "",
                severity,
            ].filter((flag) => flag !== "");
            console.log(
                `\n[${index + 1}] ${thread.id}  ${thread.path}:${line ?? "?"}  ${login(first)}  ${flags.join(" ")}`.trimEnd(),
            );
            console.log(indent(truncate(text, maxChars)));
            for (const reply of replies) {
                const replyText = clean(reply.body).text;
                if (replyText !== "") {
                    console.log(
                        indent(
                            `↳ ${login(reply)}: ${truncate(replyText, Math.floor(maxChars / 2))}`,
                        ),
                    );
                }
            }
        });
    if (found.length > 0) {
        console.log("\nfindings in review bodies (no inline thread):");
        for (const { review, severity, text } of found) {
            const commit = review.commit?.oid.slice(0, 9) ?? "?";
            console.log(
                `\n- ${`${login(review)} @ ${commit} ${severity}`.trimEnd()}  ${review.url}`,
            );
            console.log(indent(truncate(text, maxChars)));
        }
    }
}

async function resolve(ids: readonly string[]): Promise<void> {
    for (const id of ids) {
        const answer = await graphql<{
            resolveReviewThread: { thread: { id: string; isResolved: boolean } };
        }>(RESOLVE_MUTATION, { id });
        const thread = answer.resolveReviewThread.thread;
        console.log(`${thread.id}: ${thread.isResolved ? "resolved" : "still open"}`);
    }
}

async function reply(id: string, body: string): Promise<void> {
    const answer = await graphql<{
        addPullRequestReviewThreadReply: { comment: { url: string } };
    }>(REPLY_MUTATION, { id, body }, false);
    console.log(answer.addPullRequestReviewThreadReply.comment.url);
}

async function main(): Promise<void> {
    const help =
        "usage: node tools/github/pull_request.ts status [PR] | threads [PR] [--all] [--max-chars <n>] | reply <thread id> <text> | resolve <thread id>...";
    // A reply's text is the user's own: one that starts with "-" (a list, a
    // flag's name) is not an option.
    const [first, id, text, ...extra] = process.argv.slice(2);
    if (first === "reply") {
        if (id === undefined || text === undefined || extra.length > 0) {
            console.error(help);
            process.exit(2);
        }
        await reply(id, text);
        return;
    }
    const { values, positionals } = parseArgs({
        options: {
            all: { type: "boolean", default: false },
            "max-chars": { type: "string", default: "4000" },
            commits: { type: "string", default: "5" },
        },
        allowPositionals: true,
    });
    const [command, ...rest] = positionals;
    const pr = async (): Promise<number> => {
        const [given] = atMost(rest, 1, help);
        return given === undefined ? await currentPr() : issueNumber(given, "the PR");
    };
    if (command === "status") {
        await status(await pr(), count(values.commits, "--commits"));
    } else if (command === "threads") {
        await printThreads(await pr(), values.all, count(values["max-chars"], "--max-chars"));
    } else if (command === "resolve" && rest.length > 0) {
        await resolve(rest);
    } else {
        console.error(help);
        process.exit(2);
    }
}

if (import.meta.main) {
    await main();
}
