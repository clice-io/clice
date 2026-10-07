module;

#include "modules/prelude.h"

module clice;

import :server.session_store;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

namespace protocol = kota::ipc::protocol;
namespace lsp = kota::ipc::lsp;

protocol::TextDocumentContentChangeEvent partial_change(std::uint32_t start_line,
                                                        std::uint32_t start_char,
                                                        std::uint32_t end_line,
                                                        std::uint32_t end_char,
                                                        std::string text) {
    protocol::TextDocumentContentChangePartial change;
    change.range = protocol::Range{
        .start = protocol::Position{.line = start_line, .character = start_char},
        .end = protocol::Position{.line = end_line,   .character = end_char  },
    };
    change.text = std::move(text);
    return change;
}

ZEST_SUITE(SessionStore) {

ZEST_CASE(ApplyOpenInitializesBuffer) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\nint b;\n", 3);

    ZASSERT(session->version == 3);
    ZASSERT(session->text == "int a;\nint b;\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
    ZASSERT(session->generation == 1u);
}

ZEST_CASE(RangeReplace) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\nint b;\n", 1);

    auto change = partial_change(1, 4, 1, 5, "value");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "int a;\nint value;\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
    ZASSERT(session->version == 2);
    ZASSERT(session->generation == 2u);
}

ZEST_CASE(SequentialChangesFold) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "ab\ncd\n", 1);

    // The second range addresses the buffer as left by the first change.
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 1, 0, 2, "xyz"),  // "ab" -> "axyz"
        partial_change(1, 0, 1, 1, "Q"),    // "cd" -> "Qd"
    };
    store.apply_change(*session, changes, 2);

    ZASSERT(session->text == "axyz\nQd\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
    ZASSERT(session->generation == 2u);
}

ZEST_CASE(WholeDocumentReplace) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "old\n", 1);

    protocol::TextDocumentContentChangeEvent change =
        protocol::TextDocumentContentChangeWholeDocument{.text = "brand\nnew\n"};
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "brand\nnew\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(InvertedRangeSpans) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "ab\ncd\n", 1);

    // A range whose start lies after its end covers the bytes between its
    // ends, as vscode-languageserver-textdocument reads it.
    auto change = partial_change(1, 0, 0, 0, "X");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "Xcd\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(InsideSurrogatePair) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "\xf0\x9f\x98\x80x\n", 1);

    // UTF-16 unit 1 is inside the emoji's surrogate pair: the edit lands
    // at the character's start.
    auto change = partial_change(0, 1, 0, 1, "Y");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "Y\xf0\x9f\x98\x80x\n");
    ZASSERT(session->non_ascii_lines == lsp::non_ascii_lines(session->text));
}

ZEST_CASE(NonASCIILinesTracked) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "ab\ncd\n", 1);
    ZASSERT(session->non_ascii_lines.empty());

    // The second change addresses line 1 after the first made line 0
    // non-ASCII; the positions count UTF-16 units over the new text.
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 0, 0, 0, "\xc3\xa9"),
        partial_change(0, 2, 0, 3, "Z"),
    };
    store.apply_change(*session, changes, 2);

    ZASSERT(session->text ==
            "\xc3\xa9"
            "aZ\ncd\n");
    ZASSERT(session->non_ascii_lines == lsp::non_ascii_lines(session->text));
}

ZEST_CASE(SelectAllDeleteClamped) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int foo() { return 1; }\n", 1);

    // Several clients emit select-all-delete as an oversized range; per
    // LSP 3.17 positions past the document clamp to the document end.
    auto change = partial_change(0, 0, 99999, 0, "");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
    ZASSERT(session->version == 2);
    ZASSERT(session->generation == 2u);
}

ZEST_CASE(InsertPastLastLine) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\n", 1);

    // Line 1 (after the trailing newline) is the last line; line 2 clamps
    // to the true end of the document.
    auto change = partial_change(2, 0, 2, 0, "int b;\n");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "int a;\nint b;\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(InsertPastLineEnd) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "ab\ncd\n", 1);

    // A character beyond the line length clamps to the line end.
    auto change = partial_change(0, 9999, 0, 9999, "X");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "abX\ncd\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(ClampedChangeFolds) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "ab\ncd\n", 1);

    // The clamp for the second change must resolve against the buffer as
    // left by the first one (EOF has moved from 6 to 8).
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 1, 0, 2, "xyz"),  // "ab" -> "axyz"
        partial_change(99, 0, 99, 0, "!"),  // clamps to the new EOF
    };
    store.apply_change(*session, changes, 2);

    ZASSERT(session->text == "axyz\ncd\n!");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(EmptyDocumentClamped) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "", 1);

    auto change = partial_change(5, 3, 8, 0, "int x;");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "int x;");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(RangeEndPastEof) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\nint b;\n", 1);

    // The end position sits on the last (empty) line but one character
    // past its length: it clamps to the end of the document.
    auto change = partial_change(1, 0, 2, 1, "");
    store.apply_change(*session, change, 2);

    ZASSERT(session->text == "int a;\n");
    ZASSERT(session->line_starts == lsp::line_starts(session->text));
}

ZEST_CASE(ReopenBumpsGeneration) {
    SessionStore store;
    auto first = store.open(Fid{7});
    first->generation = 5;

    auto second = store.open(Fid{7});
    ZASSERT(first->generation == 6u);
    ZASSERT(first.get() != second.get());
    ZASSERT(store.find(Fid{7}).get() == second.get());
}

ZEST_CASE(CloseBumpsGeneration) {
    SessionStore store;
    auto session = store.open(Fid{7});
    session->generation = 5;

    store.close(Fid{7});
    ZASSERT(session->generation == 6u);
    ZASSERT(store.find(Fid{7}) == nullptr);
}

ZEST_CASE(ForEachVisitsAll) {
    SessionStore store;
    store.open(Fid{1});
    store.open(Fid{2});

    int visited = 0;
    store.for_each([&](Fid, const Session&) -> bool {
        ++visited;
        return true;
    });
    ZASSERT(visited == 2);

    // A false return stops the iteration early.
    visited = 0;
    store.for_each([&](Fid, const Session&) -> bool {
        ++visited;
        return false;
    });
    ZASSERT(visited == 1);
}

ZEST_CASE(EditLetsCrashRetry) {
    // The state machine itself is pinned by quarantine_tests; this pins
    // that apply_change feeds real edits into it.
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\n", 1);

    auto later = Quarantine::Clock::now() + std::chrono::minutes(1);
    session->quarantine->on_crash(0, "d1", "cause", Quarantine::Clock::now());
    ZASSERT(session->quarantine->barred(0, later));
    store.apply_change(*session, partial_change(0, 0, 0, 0, "x"), 2);
    ZASSERT(!session->quarantine->barred(0, later));
}

ZEST_CASE(NoopEditNoRetry) {
    SessionStore store;
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\n", 1);
    auto later = Quarantine::Clock::now() + std::chrono::minutes(1);
    session->quarantine->on_crash(0, "d1", "cause", Quarantine::Clock::now());

    // A retry needs a real content change: an out-of-range deletion clamps
    // to an empty range at the end of the document, an empty change list
    // and a no-op replacement change nothing — none may send the unchanged
    // crashing bytes to another worker.
    store.apply_change(*session, partial_change(99, 0, 99, 1, ""), 2);
    ZASSERT(session->quarantine->barred(0, later));

    store.apply_change(*session, {}, 3);
    ZASSERT(session->quarantine->barred(0, later));

    store.apply_change(*session, partial_change(0, 0, 0, 1, "i"), 4);
    ZASSERT(session->quarantine->barred(0, later));

    // A whole-document change carrying identical bytes is a no-op too.
    protocol::TextDocumentContentChangeWholeDocument whole;
    whole.text = session->text;
    store.apply_change(*session, protocol::TextDocumentContentChangeEvent(whole), 5);
    ZASSERT(session->quarantine->barred(0, later));

    store.apply_change(*session, partial_change(0, 0, 0, 1, "u"), 6);
    ZASSERT(!session->quarantine->barred(0, later));
}

ZEST_CASE(ReopenKeepsCrashes) {
    SessionStore store;
    auto later = Quarantine::Clock::now() + std::chrono::minutes(1);
    auto session = store.open(Fid{1});
    store.apply_open(*session, "int a;\n", 1);
    session->quarantine->on_crash(0, "d1", "cause", Quarantine::Clock::now());
    store.close(Fid{1});
    ZASSERT(session->closed);

    // Closing and reopening the same bytes is no retry.
    session = store.open(Fid{1});
    store.apply_open(*session, "int a;\n", 1);
    ZASSERT(session->quarantine->barred(0, later));

    // Reopened on other bytes, the difference counts as a change.
    store.close(Fid{1});
    session = store.open(Fid{1});
    store.apply_open(*session, "int b;\n", 1);
    ZASSERT(session->quarantine->crashed(0));
    ZASSERT(!session->quarantine->barred(0, later));
}

ZEST_CASE(InFlightAcrossClose) {
    SessionStore store;
    auto later = Quarantine::Clock::now() + std::chrono::minutes(1);
    auto closed = store.open(Fid{1});
    store.apply_open(*closed, "int a;\n", 1);
    closed->quarantine->on_crash(0, "d1", "cause", Quarantine::Clock::now());
    closed->quarantine->on_change(Quarantine::Clock::now());

    // The retry's license, taken before the close, comes back to the
    // reopened document when the retry ends without an outcome.
    std::shared_ptr<Session> reopened;
    {
        Quarantine::Attempt attempt(*closed->quarantine, 0);
        store.close(Fid{1});
        reopened = store.open(Fid{1});
        store.apply_open(*reopened, "int a;\n", 1);
        ZASSERT(reopened->quarantine->barred(0, later));
    }
    ZASSERT(!reopened->quarantine->barred(0, later));

    // A crash of work still in flight on the closed session bars the
    // reopened one.
    closed->quarantine->on_crash(1, "d2", "cause", Quarantine::Clock::now());
    ZASSERT(reopened->quarantine->barred(1, later));

    // Even when the document had no record at the close.
    auto clean = store.open(Fid{2});
    store.apply_open(*clean, "int b;\n", 1);
    store.close(Fid{2});
    clean->quarantine->on_crash(0, "d3", "cause", Quarantine::Clock::now());
    auto again = store.open(Fid{2});
    store.apply_open(*again, "int b;\n", 1);
    ZASSERT(again->quarantine->barred(0, later));
}

};  // ZEST_SUITE(SessionStore)

}  // namespace
}  // namespace clice::testing
