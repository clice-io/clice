module clice;

import :feature.diagnostics;
import :server.master;
import :server.session;
import :syntax.position;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

using Clock = Session::Clock;
using feature::RequestKind;
using std::chrono::seconds;

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

ZEST_SUITE(Session) {

Session doc{Fid{1}, "int a;\n", 1};

ZEST_CASE(OpenInitializesBuffer) {
    Session session(Fid{1}, "int a;\nint b;\n", 3);

    ZASSERT(session.version == 3);
    ZASSERT(session.text() == "int a;\nint b;\n");
    ZASSERT(session.generation == 0u);
}

ZEST_CASE(RangeReplace) {
    Session session(Fid{1}, "int a;\nint b;\n", 1);

    session.edit(partial_change(1, 4, 1, 5, "value"), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "int a;\nint value;\n");
    ZASSERT(session.version == 2);
    ZASSERT(session.generation == 1u);
}

ZEST_CASE(SequentialChangesFold) {
    Session session(Fid{1}, "ab\ncd\n", 1);

    // The second range addresses the buffer as left by the first change.
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 1, 0, 2, "xyz"),  // "ab" -> "axyz"
        partial_change(1, 0, 1, 1, "Q"),    // "cd" -> "Qd"
    };
    session.edit(changes, 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "axyz\nQd\n");
    ZASSERT(session.generation == 1u);
}

ZEST_CASE(WholeDocumentReplace) {
    Session session(Fid{1}, "old\n", 1);

    protocol::TextDocumentContentChangeEvent change =
        protocol::TextDocumentContentChangeWholeDocument{.text = "brand\nnew\n"};
    session.edit(change, 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "brand\nnew\n");
}

ZEST_CASE(InvertedRangeSpans) {
    Session session(Fid{1}, "ab\ncd\n", 1);

    // A range whose start lies after its end covers the bytes between its
    // ends, as vscode-languageserver-textdocument reads it.
    session.edit(partial_change(1, 0, 0, 0, "X"), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "Xcd\n");
}

ZEST_CASE(InsideSurrogatePair) {
    Session session(Fid{1}, "\xf0\x9f\x98\x80x\n", 1);

    // UTF-16 unit 1 is inside the emoji's surrogate pair: the edit is applied
    // at the character's start.
    session.edit(partial_change(0, 1, 0, 1, "Y"), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "Y\xf0\x9f\x98\x80x\n");
}

ZEST_CASE(NonASCIILinesTracked) {
    Session session(Fid{1}, "ab\ncd\n", 1);

    // The second change addresses line 1 after the first made line 0
    // non-ASCII; the positions count UTF-16 units over the new text.
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 0, 0, 0, "\xc3\xa9"),
        partial_change(0, 2, 0, 3, "Z"),
    };
    session.edit(changes, 2, PositionEncoding::UTF16);

    ZASSERT(session.text() ==
            "\xc3\xa9"
            "aZ\ncd\n");
}

ZEST_CASE(UTF8ColumnsCountBytes) {
    Session session(Fid{1}, "ab\ncd\n", 1);

    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 0, 0, 0, "\xc3\xa9"),
        partial_change(0, 3, 0, 4, "Z"),
    };
    session.edit(changes, 2, PositionEncoding::UTF8);

    ZASSERT(session.text() ==
            "\xc3\xa9"
            "aZ\ncd\n");
}

ZEST_CASE(InsideUTF8Sequence) {
    Session session(Fid{1}, "\xc3\xa9x\n", 1);

    // Byte 1 is inside the 2-byte character: the edit is applied at the
    // character's start.
    session.edit(partial_change(0, 1, 0, 1, "Y"), 2, PositionEncoding::UTF8);

    ZASSERT(session.text() == "Y\xc3\xa9x\n");
}

ZEST_CASE(UTF32ColumnsCountCodePoints) {
    Session session(Fid{1},
                    "\xf0\x9f\x98\x80"
                    "ab\n",
                    1);

    // The emoji is one code point, two UTF-16 units, four bytes.
    session.edit(partial_change(0, 1, 0, 2, "Z"), 2, PositionEncoding::UTF32);

    ZASSERT(session.text() ==
            "\xf0\x9f\x98\x80"
            "Zb\n");
}

ZEST_CASE(SelectAllDeleteClamped) {
    Session session(Fid{1}, "int foo() { return 1; }\n", 1);

    // Several clients emit select-all-delete as an oversized range; per
    // LSP 3.17 positions past the document clamp to the document end.
    session.edit(partial_change(0, 0, 99999, 0, ""), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "");
    ZASSERT(session.version == 2);
    ZASSERT(session.generation == 1u);
}

ZEST_CASE(InsertPastLastLine) {
    Session session(Fid{1}, "int a;\n", 1);

    // Line 1 (after the trailing newline) is the last line; line 2 clamps
    // to the true end of the document.
    session.edit(partial_change(2, 0, 2, 0, "int b;\n"), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "int a;\nint b;\n");
}

ZEST_CASE(InsertPastLineEnd) {
    Session session(Fid{1}, "ab\ncd\n", 1);

    // A character beyond the line length clamps to the line end.
    session.edit(partial_change(0, 9999, 0, 9999, "X"), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "abX\ncd\n");
}

ZEST_CASE(ClampedChangeFolds) {
    Session session(Fid{1}, "ab\ncd\n", 1);

    // The clamp for the second change must resolve against the buffer as
    // left by the first one (EOF has moved from 6 to 8).
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 1, 0, 2, "xyz"),  // "ab" -> "axyz"
        partial_change(99, 0, 99, 0, "!"),  // clamps to the new EOF
    };
    session.edit(changes, 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "axyz\ncd\n!");
}

ZEST_CASE(EmptyDocumentClamped) {
    Session session(Fid{1}, "", 1);

    session.edit(partial_change(5, 3, 8, 0, "int x;"), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "int x;");
}

ZEST_CASE(RangeEndPastEof) {
    Session session(Fid{1}, "int a;\nint b;\n", 1);

    // The end position sits on the last (empty) line but one character
    // past its length: it clamps to the end of the document.
    session.edit(partial_change(1, 0, 2, 1, ""), 2, PositionEncoding::UTF16);

    ZASSERT(session.text() == "int a;\n");
}

ZEST_CASE(NoopEditUnchanged) {
    // Only a real content change lets a crashed kind retry (see
    // Session::changed): an out-of-range deletion clamps to an empty range
    // at the end of the document, an empty change list and a no-op
    // replacement change nothing.
    Session session(Fid{1}, "int a;\n", 1);

    ZASSERT(!session.edit(partial_change(99, 0, 99, 1, ""), 2, PositionEncoding::UTF16));
    ZASSERT(!session.edit({}, 3, PositionEncoding::UTF16));
    ZASSERT(!session.edit(partial_change(0, 0, 0, 1, "i"), 4, PositionEncoding::UTF16));

    // A whole-document change carrying identical bytes is a no-op too.
    protocol::TextDocumentContentChangeWholeDocument whole;
    whole.text = session.text().str();
    ZASSERT(
        !session.edit(protocol::TextDocumentContentChangeEvent(whole), 5, PositionEncoding::UTF16));

    ZASSERT(session.edit(partial_change(0, 0, 0, 1, "u"), 6, PositionEncoding::UTF16));
}

ZEST_CASE(CrashStopsItsKind) {
    auto t0 = Clock::now();
    ZEXPECT(!doc.crash_stops(RequestKind::Compile, t0));

    doc.crashed(RequestKind::Compile, "d1", "killed by signal 11", t0);
    ZEXPECT(doc.crash_stops(RequestKind::Compile, t0));
    ZEXPECT(doc.crash_record(RequestKind::Compile) != nullptr);
    ZEXPECT(!doc.crash_stops(RequestKind::Hover, t0));
    Session other(Fid{2}, "int a;\n", 1);
    ZEXPECT(!other.crash_stops(RequestKind::Compile, t0));

    // A document that sits still is never retried.
    ZEXPECT(doc.crash_stops(RequestKind::Compile, t0 + seconds(3600)));
}

ZEST_CASE(ChangeRetriesAfterSpacing) {
    auto t0 = Clock::now();
    doc.crashed(RequestKind::Compile, "d1", "cause", t0);
    doc.changed(t0 + seconds(1));
    ZEXPECT(doc.crash_stops(RequestKind::Compile, t0 + seconds(1)));
    ZEXPECT(!doc.crash_stops(RequestKind::Compile, t0 + Session::policy.spacing));
}

ZEST_CASE(CrashesLeaveSaveOnly) {
    auto t = Clock::now();
    for(unsigned crash = 1; crash <= Session::policy.change_limit; crash += 1) {
        doc.crashed(RequestKind::Compile, std::to_string(crash), "cause", t);
        doc.changed(t);
        t += seconds(10);
    }
    ZEXPECT(doc.crash_stops(RequestKind::Compile, t));
    ZASSERT(doc.crash_notes().size() == 1u);
    ZEXPECT(doc.crash_notes()[0].save_only);

    doc.saved();
    ZEXPECT(!doc.crash_stops(RequestKind::Compile, t));

    // The save starts a fresh run: one crash later, a change retries again.
    doc.crashed(RequestKind::Compile, "after-save", "cause", t);
    doc.changed(t);
    ZEXPECT(!doc.crash_stops(RequestKind::Compile, t + seconds(10)));
}

ZEST_CASE(FreshSuccessClears) {
    // Only an answer on the buffer the request was taken on clears the
    // record: an edit arriving meanwhile left the new buffer untried.
    auto t0 = Clock::now();
    doc.crashed(RequestKind::Hover, "d1", "cause", t0);
    auto taken = doc.generation;
    doc.generation += 1;
    ZEXPECT(!doc.succeeded(RequestKind::Hover, taken));
    ZEXPECT(doc.crash_record(RequestKind::Hover) != nullptr);

    ZEXPECT(doc.succeeded(RequestKind::Hover, doc.generation));
    ZEXPECT(doc.crash_record(RequestKind::Hover) == nullptr);
    ZEXPECT(!doc.crash_stops(RequestKind::Hover, t0));
}

ZEST_CASE(DeathCountsOnce) {
    auto t0 = Clock::now();
    doc.crashed(RequestKind::Hover, "d1", "cause", t0);
    doc.crashed(RequestKind::Hover, "d1", "cause", t0);
    ZASSERT(doc.crash_notes().size() == 1u);
    ZEXPECT(doc.crash_notes()[0].count == 1u);
}

ZEST_CASE(AttemptHoldsRetry) {
    auto t0 = Clock::now();
    doc.crashed(RequestKind::Compile, "d1", "cause", t0);
    doc.saved();
    {
        Session::Attempt attempt(doc, RequestKind::Compile);
        ZEXPECT(doc.crash_stops(RequestKind::Compile, t0));
    }
    // Ended without a crash: the retry comes back.
    ZEXPECT(!doc.crash_stops(RequestKind::Compile, t0));

    {
        Session::Attempt attempt(doc, RequestKind::Compile);
        doc.crashed(RequestKind::Compile, "d2", "cause", t0);
    }
    ZEXPECT(doc.crash_stops(RequestKind::Compile, t0 + seconds(10)));
}

ZEST_CASE(SaveThenCrashStops) {
    // The crash still spends the retry the attempt held, a save during its
    // flight included.
    auto t0 = Clock::now();
    doc.crashed(RequestKind::Compile, "d1", "cause", t0);
    doc.changed(t0);
    {
        Session::Attempt attempt(doc, RequestKind::Compile);
        doc.saved();
        doc.crashed(RequestKind::Compile, "d2", "cause", t0 + seconds(1));
    }
    ZEXPECT(doc.crash_stops(RequestKind::Compile, t0 + seconds(10)));
}

ZEST_CASE(SiblingCrashMeanwhile) {
    auto t0 = Clock::now();
    Session::Attempt attempt(doc, RequestKind::Hover);
    ZEXPECT(!attempt.crashed_meanwhile());
    doc.crashed(RequestKind::Compile, "d1", "cause", t0);
    ZEXPECT(!attempt.crashed_meanwhile());
    doc.crashed(RequestKind::Hover, "d2", "cause", t0);
    ZEXPECT(attempt.crashed_meanwhile());
}

ZEST_CASE(ChangeDuringFlightStands) {
    // The crash describes the inputs the attempt carried; an edit during
    // its flight is still untried.
    auto t0 = Clock::now();
    {
        Session::Attempt attempt(doc, RequestKind::Compile);
        doc.changed(t0);
        doc.crashed(RequestKind::Compile, "d1", "cause", t0 + seconds(20));
    }
    ZEXPECT(!doc.crash_stops(RequestKind::Compile, t0 + seconds(30)));
}

ZEST_CASE(EditingCrashStaysSilent) {
    auto t0 = Clock::now();
    doc.changed(t0);
    ZEXPECT(!doc.crashed(RequestKind::Compile, "d1", "cause", t0 + seconds(1)));
    ZEXPECT(doc.crash_notes().empty());

    // A repeat shows.
    doc.changed(t0 + seconds(2));
    ZEXPECT(doc.crashed(RequestKind::Compile, "d2", "cause", t0 + seconds(5)));
    ZEXPECT(doc.crash_notes().size() == 1u);
}

ZEST_CASE(SavedCrashShows) {
    // Saved code is no half-typed code, however recent the edit.
    auto t0 = Clock::now();
    doc.changed(t0);
    doc.saved();
    ZEXPECT(doc.crashed(RequestKind::Compile, "d1", "cause", t0 + seconds(1)));
}

ZEST_CASE(ColdCrashShows) {
    auto t0 = Clock::now();
    doc.changed(t0);
    doc.crashed(RequestKind::Hover,
                "d1",
                "killed by signal 6 (SIGABRT)",
                t0 + Session::policy.editing_window);
    auto notes = doc.crash_notes();
    ZASSERT(notes.size() == 1u);
    ZEXPECT(notes[0].kind == RequestKind::Hover);
    ZEXPECT(notes[0].cause == "killed by signal 6 (SIGABRT)");
    ZEXPECT(!notes[0].save_only);
}

};  // ZEST_SUITE(Session)

/// The open documents of a server whose projects have not started: a
/// didOpen before initialize waits in the table with no owner.
ZEST_SUITE(DocumentTable) {

kota::event_loop loop;
MasterServer server{loop, ""};
Spelling spelled = Spelling::absolute("/proj/a.cpp");
Fid path_id = server.files.intern(spelled);

void open(std::string text = "int a;\n") {
    server.open_document(path_id, spelled.str(), std::move(text), 1);
}

ZEST_CASE(ReopenBumpsGeneration) {
    open();
    auto first = server.find_session(path_id);
    ZASSERT(first != nullptr);
    first->generation = 5;

    open();
    ZASSERT(first->generation == 6u);
    auto second = server.find_session(path_id);
    ZASSERT(second != nullptr);
    ZASSERT(first.get() != second.get());
}

ZEST_CASE(CloseBumpsGeneration) {
    open();
    auto session = server.find_session(path_id);
    ZASSERT(session != nullptr);
    session->generation = 5;

    server.close_document(path_id, spelled.str());
    ZASSERT(session->generation == 6u);
    ZASSERT(server.find_session(path_id) == nullptr);
}

ZEST_CASE(ReopenStartsClean) {
    // A document's crash records die with it: reopened on the very same
    // bytes, its kinds run again.
    auto t0 = Clock::now();
    open();
    auto first = server.find_session(path_id);
    ZASSERT(first != nullptr);
    first->crashed(RequestKind::Compile, "d1", "cause", t0);
    ZEXPECT(first->crash_stops(RequestKind::Compile, t0));

    server.close_document(path_id, spelled.str());
    open();
    auto reopened = server.find_session(path_id);
    ZASSERT(reopened != nullptr);
    ZEXPECT(reopened->crash_record(RequestKind::Compile) == nullptr);
    ZEXPECT(!reopened->crash_stops(RequestKind::Compile, t0));
    ZEXPECT(reopened->crash_notes().empty());
}

};  // ZEST_SUITE(DocumentTable)

}  // namespace

}  // namespace clice::testing
