module;

#include "modules/prelude.h"

module clice;

import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.file_system;
import :worker.common;

namespace clice::testing {

namespace {

struct Reply {
    std::string tu_index_data;
    bool index_in_file = false;
};

ZEST_SUITE(IndexHandOver) {

ZEST_CASE(SmallIndexInline) {
    Reply reply;
    ZEXPECT(!hand_over_index("rows", "", reply));
    ZEXPECT(reply.tu_index_data == "rows");
    ZEXPECT(!reply.index_in_file);
}

ZEST_CASE(LargeIndexToFile) {
    TempDir tmp;
    auto path = tmp.path("index.transfer");
    std::string envelope(max_index_bytes() + 1, 'x');
    Reply reply;
    ZEXPECT(!hand_over_index(envelope, path, reply));
    ZEXPECT(reply.index_in_file);
    ZEXPECT(reply.tu_index_data.empty());
    auto written = vfs::read(path, vfs::Read::Bytes);
    ZASSERT(written);
    ZEXPECT((*written)->getBuffer() == envelope);
}

ZEST_CASE(LargeIndexWithoutPath) {
    Reply reply;
    auto error = hand_over_index(std::string(max_index_bytes() + 1, 'x'), "", reply);
    ZASSERT(error);
    ZEXPECT(error->contains("too large to send between clice processes"));
    ZEXPECT(!reply.index_in_file);
    ZEXPECT(reply.tu_index_data.empty());
}

ZEST_CASE(FailedWriteReported) {
    TempDir tmp;
    tmp.touch("file");
    Reply reply;
    auto error =
        hand_over_index(std::string(max_index_bytes() + 1, 'x'), tmp.path("file/index"), reply);
    ZASSERT(error);
    ZEXPECT(error->starts_with("writing the index to"));
    ZEXPECT(!reply.index_in_file);
}

};  // ZEST_SUITE(IndexHandOver)

}  // namespace

}  // namespace clice::testing
