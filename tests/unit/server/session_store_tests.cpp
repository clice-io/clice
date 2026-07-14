#include <string>

#include "test/test.h"
#include "server/state/session_store.h"

#include "kota/ipc/lsp/text.h"

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

TEST_SUITE(SessionStore) {

TEST_CASE(ApplyOpenInitializesBuffer) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "int a;\nint b;\n", 3);

    ASSERT_EQ(session->version, 3);
    ASSERT_EQ(session->text, "int a;\nint b;\n");
    ASSERT_EQ(session->line_starts, lsp::build_line_starts(session->text));
    ASSERT_EQ(session->generation, 1u);
}

TEST_CASE(RangeReplace) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "int a;\nint b;\n", 1);

    auto change = partial_change(1, 4, 1, 5, "value");
    store.apply_change(*session, change, 2);

    ASSERT_EQ(session->text, "int a;\nint value;\n");
    ASSERT_EQ(session->line_starts, lsp::build_line_starts(session->text));
    ASSERT_EQ(session->version, 2);
    ASSERT_TRUE(session->ast_dirty);
    ASSERT_EQ(session->generation, 2u);
}

TEST_CASE(SequentialChangesFold) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "ab\ncd\n", 1);

    // The second range addresses the buffer as left by the first change.
    protocol::TextDocumentContentChangeEvent changes[] = {
        partial_change(0, 1, 0, 2, "xyz"),  // "ab" -> "axyz"
        partial_change(1, 0, 1, 1, "Q"),    // "cd" -> "Qd"
    };
    store.apply_change(*session, changes, 2);

    ASSERT_EQ(session->text, "axyz\nQd\n");
    ASSERT_EQ(session->line_starts, lsp::build_line_starts(session->text));
    ASSERT_EQ(session->generation, 2u);
}

TEST_CASE(WholeDocumentReplace) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "old\n", 1);

    protocol::TextDocumentContentChangeEvent change =
        protocol::TextDocumentContentChangeWholeDocument{.text = "brand\nnew\n"};
    store.apply_change(*session, change, 2);

    ASSERT_EQ(session->text, "brand\nnew\n");
    ASSERT_EQ(session->line_starts, lsp::build_line_starts(session->text));
}

TEST_CASE(InvalidRangeDropped) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "short\n", 1);

    // A range past the end of the document cannot be mapped to offsets;
    // the change is silently dropped but version bookkeeping still runs.
    auto change = partial_change(99, 0, 99, 1, "junk");
    store.apply_change(*session, change, 2);

    ASSERT_EQ(session->text, "short\n");
    ASSERT_EQ(session->version, 2);
    ASSERT_TRUE(session->ast_dirty);
    ASSERT_EQ(session->generation, 2u);
}

TEST_CASE(ReopenBumpsGeneration) {
    SessionStore store;
    auto first = store.open(7);
    first->generation = 5;

    auto second = store.open(7);
    ASSERT_EQ(first->generation, 6u);
    ASSERT_NE(first.get(), second.get());
    ASSERT_EQ(store.find(7).get(), second.get());
}

TEST_CASE(CloseBumpsGeneration) {
    SessionStore store;
    auto session = store.open(7);
    session->generation = 5;

    store.close(7);
    ASSERT_EQ(session->generation, 6u);
    ASSERT_EQ(store.find(7), nullptr);
}

TEST_CASE(ResetSupersededBumpsGeneration) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "int x;", 1);
    session->ast_dirty = false;
    session->trial_done = true;
    session->pch_key = "key";
    session->ast_deps.emplace();
    auto gen = session->generation;
    auto epoch = session->dirty_epoch;

    SessionStore::reset_compile_state(*session, ResetDepth::Superseded);

    ASSERT_TRUE(session->ast_dirty);
    ASSERT_FALSE(session->trial_done);
    ASSERT_FALSE(session->pch_key.has_value());
    ASSERT_FALSE(session->ast_deps.has_value());
    ASSERT_EQ(session->generation, gen + 1);
    ASSERT_EQ(session->dirty_epoch, epoch);
}

TEST_CASE(ResetLostBumpsEpoch) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "int x;", 1);
    session->ast_dirty = false;
    session->trial_done = true;
    session->pch_key = "key";
    auto gen = session->generation;
    auto epoch = session->dirty_epoch;

    SessionStore::reset_compile_state(*session, ResetDepth::Lost);

    // The buffer is still the same buffer and its inputs did not change:
    // only the freshness claim is revoked.
    ASSERT_TRUE(session->ast_dirty);
    ASSERT_TRUE(session->trial_done);
    ASSERT_TRUE(session->pch_key.has_value());
    ASSERT_EQ(session->generation, gen);
    ASSERT_EQ(session->dirty_epoch, epoch + 1);
}

TEST_CASE(SettleCompileConditional) {
    Session session;
    session.ast_dirty = true;
    auto launch_epoch = session.dirty_epoch;

    // Invalidation landed mid-flight: the product must not claim freshness.
    session.dirty_epoch += 1;
    session.settle_compile(launch_epoch);
    ASSERT_TRUE(session.ast_dirty);

    // Quiet flight: the clear goes through.
    session.settle_compile(session.dirty_epoch);
    ASSERT_FALSE(session.ast_dirty);
}

TEST_CASE(ForEachVisitsAll) {
    SessionStore store;
    store.open(1);
    store.open(2);

    int visited = 0;
    store.for_each([&](std::uint32_t, const Session&) -> bool {
        ++visited;
        return true;
    });
    ASSERT_EQ(visited, 2);

    // A false return stops the iteration early.
    visited = 0;
    store.for_each([&](std::uint32_t, const Session&) -> bool {
        ++visited;
        return false;
    });
    ASSERT_EQ(visited, 1);
}

TEST_CASE(QuarantineProbeOnEdit) {
    // The state machine itself is pinned by quarantine_tests; this pins
    // that apply_change feeds real edits into it without resetting the
    // record — only a successful compile proves the document healthy.
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "int a;\n", 1);

    session->quarantine.on_crash();
    store.apply_change(*session, partial_change(0, 0, 0, 0, "x"), 2);
    ASSERT_EQ(session->quarantine.crashes(), 1u);

    // At the threshold an edit re-arms one probe and keeps the streak: a
    // quarantined document earns a single attempt per change, not a fresh
    // budget.
    session->quarantine.on_crash();
    ASSERT_TRUE(session->quarantine.blocked());
    store.apply_change(*session, partial_change(0, 0, 0, 0, "y"), 3);
    ASSERT_EQ(session->quarantine.crashes(), Quarantine::threshold);
    ASSERT_FALSE(session->quarantine.blocked());
}

TEST_CASE(DroppedEditNoProbe) {
    SessionStore store;
    auto session = store.open(1);
    store.apply_open(*session, "int a;\n", 1);
    session->quarantine.on_crash();
    session->quarantine.on_crash();
    ASSERT_TRUE(session->quarantine.blocked());

    // The probe license is one attempt per real content change: an edit
    // whose range does not fit the buffer is dropped, an empty change list
    // and a no-op replacement change nothing — none may re-arm a compile
    // of the unchanged poison bytes.
    store.apply_change(*session, partial_change(99, 0, 99, 1, "junk"), 2);
    ASSERT_TRUE(session->quarantine.blocked());

    store.apply_change(*session, {}, 3);
    ASSERT_TRUE(session->quarantine.blocked());

    store.apply_change(*session, partial_change(0, 0, 0, 1, "i"), 4);
    ASSERT_TRUE(session->quarantine.blocked());

    // A whole-document change carrying identical bytes is a no-op too.
    protocol::TextDocumentContentChangeWholeDocument whole;
    whole.text = session->text;
    store.apply_change(*session, protocol::TextDocumentContentChangeEvent(whole), 5);
    ASSERT_TRUE(session->quarantine.blocked());

    store.apply_change(*session, partial_change(0, 0, 0, 1, "u"), 6);
    ASSERT_FALSE(session->quarantine.blocked());
}

TEST_CASE(ReopenClearsQuarantine) {
    SessionStore store;
    auto session = store.open(1);
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    store.apply_open(*session, "int a;\n", 1);

    ASSERT_EQ(session->quarantine.crashes(), 0u);
    ASSERT_FALSE(session->quarantine.blocked());
}

};  // TEST_SUITE(SessionStore)

}  // namespace
}  // namespace clice::testing
