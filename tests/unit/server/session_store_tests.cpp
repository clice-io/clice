#include <string>

#include "test/test.h"
#include "server/session/session_store.h"

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

};  // TEST_SUITE(SessionStore)

}  // namespace
}  // namespace clice::testing
