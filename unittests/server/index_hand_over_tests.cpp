module;

#include "support/prelude.h"

module clice;

import :support.bytes;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :worker.common;

namespace clice::testing {

namespace {

struct Reply {
    std::vector<std::byte> tu_index_data;
    bool index_in_file = false;
};

ZEST_SUITE(IndexHandOver) {

ZEST_CASE(SmallIndexInline) {
    Reply reply;
    ZEXPECT(!hand_over_index(copy_bytes("rows"), "", reply));
    ZEXPECT(as_chars(reply.tu_index_data) == "rows");
    ZEXPECT(!reply.index_in_file);
}

ZEST_CASE(LargeIndexToFile) {
    TempDir tmp;
    auto path = tmp.path("index.transfer");
    std::vector<std::byte> blob(max_index_bytes() + 1, std::byte{'x'});
    Reply reply;
    ZEXPECT(!hand_over_index(blob, path, reply));
    ZEXPECT(reply.index_in_file);
    ZEXPECT(reply.tu_index_data.empty());
    auto written = vfs::read(path, vfs::Read::Bytes);
    ZASSERT(written);
    ZEXPECT((*written)->getBuffer() == as_chars(blob));
}

ZEST_CASE(LargeIndexWithoutPath) {
    Reply reply;
    auto error =
        hand_over_index(std::vector<std::byte>(max_index_bytes() + 1, std::byte{'x'}), "", reply);
    ZASSERT(error);
    ZEXPECT(error->contains("too large to send between clice processes"));
    ZEXPECT(!reply.index_in_file);
    ZEXPECT(reply.tu_index_data.empty());
}

ZEST_CASE(FailedWriteReported) {
    TempDir tmp;
    tmp.touch("file");
    Reply reply;
    auto error = hand_over_index(std::vector<std::byte>(max_index_bytes() + 1, std::byte{'x'}),
                                 tmp.path("file/index"),
                                 reply);
    ZASSERT(error);
    ZEXPECT(error->starts_with("writing the index to"));
    ZEXPECT(!reply.index_in_file);
}

};  // ZEST_SUITE(IndexHandOver)

}  // namespace

}  // namespace clice::testing
