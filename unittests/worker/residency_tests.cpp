module clice;

import :unittests.utils;
import :worker.residency;

namespace clice::testing {

namespace {

ZEST_SUITE(Residency) {

/// Assign `document` to `worker` and have its compile leave an AST there.
Residency::Assignment compile(Residency& residency, std::uint32_t document, std::size_t worker) {
    auto assignment = residency.assign(document, worker, std::format("/{}.cpp", document));
    residency.used(document);
    residency.compiled(document, assignment.id, true);
    return assignment;
}

llvm::SmallVector<std::string> evicted(Residency& residency) {
    llvm::SmallVector<std::string> paths;
    for(auto& eviction: residency.evictions()) {
        paths.push_back(eviction.path);
    }
    return paths;
}

ZEST_CASE(EvictsLeastRecentlyUsed) {
    Residency residency(2);
    compile(residency, 1, 0);
    compile(residency, 2, 0);
    residency.used(1);
    compile(residency, 3, 1);
    ZEXPECT(evicted(residency) == llvm::SmallVector<std::string>{"/2.cpp"});
    ZEXPECT(residency.resident(1));
    ZEXPECT(!residency.resident(2));
    ZEXPECT(!residency.assignment(2));
    ZEXPECT(residency.resident(3));
}

ZEST_CASE(HeldStays) {
    // A held document stays past the limit; its hold ending evicts it.
    Residency residency(1);
    compile(residency, 1, 0);
    residency.hold(1);
    compile(residency, 2, 0);
    residency.hold(2);
    ZEXPECT(evicted(residency).empty());
    residency.release(2);
    ZEXPECT(evicted(residency) == llvm::SmallVector<std::string>{"/2.cpp"});
    residency.release(1);
    ZEXPECT(evicted(residency).empty());
    ZEXPECT(residency.resident(1));
}

ZEST_CASE(OnlyASTsCount) {
    // A document assigned without an AST — its compile in flight, or one
    // that produced none — takes no place under the limit.
    Residency residency(1);
    compile(residency, 1, 0);
    auto assignment = residency.assign(2, 0, "/2.cpp");
    ZEXPECT(evicted(residency).empty());
    residency.compiled(2, assignment.id, false);
    ZEXPECT(evicted(residency).empty());
    ZEXPECT(residency.documents_on(0) == 2u);
}

ZEST_CASE(UndoneCompileCounts) {
    // A compile that stopped before it replaced the document's AST leaves
    // the one before on the worker: no query reaches it, the limit still
    // counts it.
    Residency residency(1);
    auto first = compile(residency, 1, 0);
    residency.used(1);
    residency.compiled(1, first.id, false);
    ZEXPECT(!residency.resident(1));
    compile(residency, 2, 0);
    ZEXPECT(evicted(residency) == llvm::SmallVector<std::string>{"/1.cpp"});
    ZEXPECT(residency.resident(2));
}

ZEST_CASE(StaleReplyIgnored) {
    // A compile reply for an assignment the document left — it closed and
    // opened again meanwhile — adds no AST.
    Residency residency(4);
    auto old = residency.assign(1, 0, "/1.cpp");
    auto eviction = residency.forget(1);
    ZASSERT(eviction);
    ZEXPECT(eviction->worker == 0u);
    residency.compiled(1, old.id, true);
    ZEXPECT(!residency.resident(1));
    auto fresh = residency.assign(1, 1, "/1.cpp");
    residency.compiled(1, old.id, true);
    ZEXPECT(!residency.resident(1));
    residency.compiled(1, fresh.id, true);
    ZEXPECT(residency.resident(1));
}

ZEST_CASE(WorkerDeathKeepsHolds) {
    // A worker's death takes its documents' ASTs, not the holds of the
    // requests about them, which hold the document through its recompile.
    Residency residency(1);
    compile(residency, 1, 0);
    compile(residency, 2, 1);
    residency.hold(2);
    ZEXPECT(evicted(residency) == llvm::SmallVector<std::string>{"/1.cpp"});
    ZEXPECT(residency.worker_died(1) == llvm::SmallVector<std::uint32_t>{2});
    ZEXPECT(!residency.resident(2));
    ZEXPECT(!residency.assignment(2));
    compile(residency, 2, 0);
    compile(residency, 3, 0);
    ZEXPECT(evicted(residency) == llvm::SmallVector<std::string>{"/3.cpp"});
    ZEXPECT(residency.resident(2));
}

};  // ZEST_SUITE(Residency)

}  // namespace

}  // namespace clice::testing
