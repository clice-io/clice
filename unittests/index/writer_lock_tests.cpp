module;

#include "support/prelude.h"

module clice;

import :index.database;
import :index.writer_lock;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(WriterLock) {

CacheStore open_store(TempDir& tmp) {
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store.has_value());
    return std::move(*store);
}

ZEST_CASE(WriterLockAtCacheRoot) {
    TempDir tmp;
    auto store = open_store(tmp);
    auto lock = index::WriterLock::acquire(store.root_dir());
    ZASSERT(lock);
    // The lock belongs to the open file: a second acquisition fails in this
    // process too.
    ZASSERT(!index::WriterLock::acquire(store.root_dir()));
    // The lock guards the cache directory, not a configuration's database,
    // and opening a database takes none.
    ZASSERT(fs::exists(path::join(store.root_dir(), "index.lock")).value_or(false));
    auto db = index::open_database(store, {"x"});
    ZASSERT(db != nullptr);
    ZASSERT(!fs::exists(path::join(index::database_directory(store, {"x"}), "index.lock"))
                 .value_or(false));
    auto held = read_file(path::join(store.root_dir(), "index.lock"));
    ZASSERT(held);
    ZASSERT(llvm::StringRef(*held).trim() == std::to_string(sys::pid()));
    lock.reset();
    auto released = read_file(path::join(store.root_dir(), "index.lock"));
    ZASSERT((released.has_value() && released->empty()));
}

ZEST_CASE(ProbeIgnoresStaleEndpoint) {
    TempDir tmp;
    auto store = open_store(tmp);
    auto cache_dir = store.root_dir();
    index::write_endpoint(cache_dir, {.pid = 1, .version = "x", .host = "127.0.0.1", .port = 1});
    ZASSERT(fs::exists(path::join(cache_dir, "server.json")).value_or(false));

    // Nobody holds the lock: the record is a crash's residue, removed by
    // the probe.
    auto probe = index::probe_writer(cache_dir);
    ZASSERT(probe.state == index::WriterProbe::State::Free);
    ZASSERT(!fs::exists(path::join(cache_dir, "server.json")).value_or(false));

    index::write_endpoint(cache_dir, {.pid = 1, .version = "x", .host = "127.0.0.1", .port = 1});
    index::remove_endpoint(cache_dir);
    ZASSERT(!fs::exists(path::join(cache_dir, "server.json")).value_or(false));
}

};  // ZEST_SUITE(WriterLock)

}  // namespace
}  // namespace clice::testing
