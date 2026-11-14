module clice;

import :server.control;
import :unittests.utils;
import :unittests.utils.temp_dir;
import :vfs.path;

namespace clice::testing {
namespace {

#ifndef _WIN32
ZEST_SUITE(ControlChannel, zest::LoopFixture) {

ZEST_CASE(SocketMovesToTemp) {
    // A socket's path holds about a hundred bytes: a cache directory
    // deeper than that keeps none, and a client reaches the server at the
    // name it is given instead.
    TempDir tmp;
    auto cache_dir = tmp.path(std::string(120, 'c'));
    auto listener = control::listen(cache_dir, loop);
    ZASSERT(listener.has_value());
    ZASSERT(listener->directory.has_value());
    ZEXPECT(path::parent(listener->name) == listener->directory->path());
    ZEXPECT(!listener->name.starts_with(cache_dir));

    auto accepted = [&]() -> async::task<> {
        auto connection = co_await listener->acceptor.accept();
        ZEXPECT(connection.has_value());
    };
    auto connected = [&]() -> async::task<> {
        auto connection = co_await async::pipe::connect(listener->name, loop);
        ZEXPECT(connection.has_value());
    };
    run(accepted(), connected());
}

ZEST_CASE(SocketOwnerOnly) {
    TempDir tmp;
    auto listener = control::listen(tmp.root, loop);
    ZASSERT(listener.has_value());
    ZEXPECT(listener->name == tmp.path("server.sock"));
    auto status = fs::status(listener->name);
    ZASSERT(status);
    ZEXPECT(status->type == fs::FileType::Socket);
    ZEXPECT(status->permissions == 0600);
}

};  // ZEST_SUITE(ControlChannel)
#endif

}  // namespace
}  // namespace clice::testing
