module clice;

import :feature.diagnostics;
import :server.document_failures;
import :server.session;
import :unittests.utils;

namespace clice::testing {

namespace {

using Clock = DocumentFailures::Clock;
using feature::RequestKind;
using std::chrono::seconds;

constexpr Fid doc{1};

/// A ticket on an open document whose buffer did not move since.
Ticket fresh_ticket(Fid document) {
    auto session = std::make_shared<Session>();
    session->path_id = document;
    return Ticket::take(std::move(session));
}

ZEST_SUITE(DocumentFailures) {

DocumentFailures failures;

ZEST_CASE(CrashStopsItsKind) {
    auto t0 = Clock::now();
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, t0));

    failures.crashed(doc, RequestKind::Compile, "d1", "killed by signal 11", t0);
    ZEXPECT(failures.stops(doc, RequestKind::Compile, t0));
    ZEXPECT(failures.find(doc, RequestKind::Compile) != nullptr);
    ZEXPECT(!failures.stops(doc, RequestKind::Hover, t0));
    ZEXPECT(!failures.stops(Fid{2}, RequestKind::Compile, t0));

    // A document that sits still is never retried.
    ZEXPECT(failures.stops(doc, RequestKind::Compile, t0 + seconds(3600)));
}

ZEST_CASE(ChangeRetriesAfterSpacing) {
    auto t0 = Clock::now();
    failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0);
    failures.changed(doc, t0 + seconds(1));
    ZEXPECT(failures.stops(doc, RequestKind::Compile, t0 + seconds(1)));
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, t0 + DocumentFailures::policy.spacing));
}

ZEST_CASE(CrashesLeaveSaveOnly) {
    auto t = Clock::now();
    for(unsigned crash = 1; crash <= DocumentFailures::policy.change_limit; crash += 1) {
        failures.crashed(doc, RequestKind::Compile, std::to_string(crash), "cause", t);
        failures.changed(doc, t);
        t += seconds(10);
    }
    ZEXPECT(failures.stops(doc, RequestKind::Compile, t));
    ZASSERT(failures.notes(doc).size() == 1u);
    ZEXPECT(failures.notes(doc)[0].save_only);

    failures.saved(doc);
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, t));

    // The save starts a fresh run: one crash later, a change retries again.
    failures.crashed(doc, RequestKind::Compile, "after-save", "cause", t);
    failures.changed(doc, t);
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, t + seconds(10)));
}

ZEST_CASE(FreshSuccessClears) {
    // Only an answer on the buffer the request was taken on clears the
    // record: an edit arriving meanwhile left the new buffer untried.
    auto t0 = Clock::now();
    failures.crashed(doc, RequestKind::Hover, "d1", "cause", t0);
    auto ticket = fresh_ticket(doc);
    ticket.session->generation += 1;
    ZEXPECT(!failures.succeeded(ticket, RequestKind::Hover));
    ZEXPECT(failures.any(doc));

    ZEXPECT(failures.succeeded(fresh_ticket(doc), RequestKind::Hover));
    ZEXPECT(!failures.any(doc));
    ZEXPECT(!failures.stops(doc, RequestKind::Hover, t0));
}

ZEST_CASE(DeathCountsOnce) {
    auto t0 = Clock::now();
    failures.crashed(doc, RequestKind::Hover, "d1", "cause", t0);
    failures.crashed(doc, RequestKind::Hover, "d1", "cause", t0);
    ZASSERT(failures.notes(doc).size() == 1u);
    ZEXPECT(failures.notes(doc)[0].count == 1u);
}

ZEST_CASE(AttemptHoldsRetry) {
    auto t0 = Clock::now();
    failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0);
    failures.saved(doc);
    {
        DocumentFailures::Attempt attempt(failures, doc, RequestKind::Compile);
        ZEXPECT(failures.stops(doc, RequestKind::Compile, t0));
    }
    // Ended without a crash: the retry comes back.
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, t0));

    {
        DocumentFailures::Attempt attempt(failures, doc, RequestKind::Compile);
        failures.crashed(doc, RequestKind::Compile, "d2", "cause", t0);
    }
    ZEXPECT(failures.stops(doc, RequestKind::Compile, t0 + seconds(10)));
}

ZEST_CASE(SaveThenCrashStops) {
    // The crash still spends the retry the attempt held, a save during its
    // flight included.
    auto t0 = Clock::now();
    failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0);
    failures.changed(doc, t0);
    {
        DocumentFailures::Attempt attempt(failures, doc, RequestKind::Compile);
        failures.saved(doc);
        failures.crashed(doc, RequestKind::Compile, "d2", "cause", t0 + seconds(1));
    }
    ZEXPECT(failures.stops(doc, RequestKind::Compile, t0 + seconds(10)));
}

ZEST_CASE(SiblingCrashMeanwhile) {
    auto t0 = Clock::now();
    DocumentFailures::Attempt attempt(failures, doc, RequestKind::Hover);
    ZEXPECT(!attempt.crashed_meanwhile());
    failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0);
    ZEXPECT(!attempt.crashed_meanwhile());
    failures.crashed(doc, RequestKind::Hover, "d2", "cause", t0);
    ZEXPECT(attempt.crashed_meanwhile());
}

ZEST_CASE(ChangeDuringFlightStands) {
    // The crash describes the inputs the attempt carried; an edit during
    // its flight is still untried.
    auto t0 = Clock::now();
    {
        DocumentFailures::Attempt attempt(failures, doc, RequestKind::Compile);
        failures.changed(doc, t0);
        failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0 + seconds(20));
    }
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, t0 + seconds(30)));
}

ZEST_CASE(EditingCrashStaysSilent) {
    auto t0 = Clock::now();
    failures.changed(doc, t0);
    ZEXPECT(!failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0 + seconds(1)));
    ZEXPECT(failures.notes(doc).empty());

    // A repeat shows.
    failures.changed(doc, t0 + seconds(2));
    ZEXPECT(failures.crashed(doc, RequestKind::Compile, "d2", "cause", t0 + seconds(5)));
    ZEXPECT(failures.notes(doc).size() == 1u);
}

ZEST_CASE(SavedCrashShows) {
    // Saved code is no half-typed code, however recent the edit.
    auto t0 = Clock::now();
    failures.changed(doc, t0);
    failures.saved(doc);
    ZEXPECT(failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0 + seconds(1)));
}

ZEST_CASE(ColdCrashShows) {
    auto t0 = Clock::now();
    failures.changed(doc, t0);
    failures.crashed(doc,
                     RequestKind::Hover,
                     "d1",
                     "killed by signal 6 (SIGABRT)",
                     t0 + DocumentFailures::policy.editing_window);
    auto notes = failures.notes(doc);
    ZASSERT(notes.size() == 1u);
    ZEXPECT(notes[0].kind == RequestKind::Hover);
    ZEXPECT(notes[0].cause == "killed by signal 6 (SIGABRT)");
    ZEXPECT(!notes[0].save_only);
}

ZEST_CASE(ReopenKeepsCrashes) {
    auto t0 = Clock::now();
    auto later = t0 + std::chrono::minutes(1);
    failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0);
    failures.closed(doc, 1);

    // Closing and reopening the same bytes is no retry.
    failures.opened(doc, 1, t0);
    ZEXPECT(failures.stops(doc, RequestKind::Compile, later));

    // Reopened on other bytes, the difference counts as a change.
    failures.closed(doc, 1);
    failures.opened(doc, 2, t0);
    ZEXPECT(failures.find(doc, RequestKind::Compile) != nullptr);
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, later));
}

ZEST_CASE(InFlightAcrossClose) {
    auto t0 = Clock::now();
    auto later = t0 + std::chrono::minutes(1);
    failures.crashed(doc, RequestKind::Compile, "d1", "cause", t0);
    failures.changed(doc, t0);

    // The retry, taken before the close, comes back to the reopened
    // document when it ends without an outcome.
    {
        DocumentFailures::Attempt attempt(failures, doc, RequestKind::Compile);
        failures.closed(doc, 1);
        failures.opened(doc, 1, t0);
        ZEXPECT(failures.stops(doc, RequestKind::Compile, later));
    }
    ZEXPECT(!failures.stops(doc, RequestKind::Compile, later));

    // A crash of work still in flight after the close stops the reopened
    // document, even one with no record at the close.
    Fid clean{2};
    failures.closed(clean, 1);
    failures.crashed(clean, RequestKind::Compile, "d2", "cause", t0);
    failures.opened(clean, 1, t0);
    ZEXPECT(failures.stops(clean, RequestKind::Compile, later));
}

};  // ZEST_SUITE(DocumentFailures)

}  // namespace

}  // namespace clice::testing
