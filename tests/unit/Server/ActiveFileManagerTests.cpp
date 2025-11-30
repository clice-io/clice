#include <format>
#include <limits>
#include <optional>
#include <string>

#include "Test/Test.h"
#include "Server/Server.h"

namespace clice::testing {

namespace {

TEST_SUITE(ActiveFileManager) {

using Manager = ActiveFileManager;

TEST_CASE(MaxSize) {
    Manager actives;

    ASSERT_EQ(actives.max_size(), Manager::DefaultMaxActiveFileNum);

    actives.set_capability(0);
    ASSERT_EQ(actives.max_size(), 1U);

    actives.set_capability(std::numeric_limits<size_t>::max());
    ASSERT_TRUE(actives.max_size() <= Manager::UnlimitedActiveFileNum);
}

TEST_CASE(LruAlgorithm) {
    Manager actives;
    actives.set_capability(1);

    ASSERT_EQ(actives.size(), 0U);

    auto& first = actives.add("first", OpenFile{.version = 1});
    ASSERT_EQ(actives.size(), 1U);
    ASSERT_TRUE(actives.contains("first"));
    ASSERT_EQ(first->version, 1U);

    auto& second = actives.add("second", OpenFile{.version = 2});
    ASSERT_EQ(actives.size(), 1U);
}

TEST_CASE(IteratorBasic) {
    Manager actives;
    actives.set_capability(3);

    actives.add("first", OpenFile{.version = 1});
    actives.add("second", OpenFile{.version = 2});
    actives.add("third", OpenFile{.version = 3});
    ASSERT_EQ(actives.size(), 3U);

    auto iter = actives.begin();
    ASSERT_TRUE(iter != actives.end());
    ASSERT_EQ(iter->first, "third");
    ASSERT_EQ(iter->second->version, 3U);

    iter++;
    ASSERT_TRUE(iter != actives.end());
    ASSERT_EQ(iter->first, "second");
    ASSERT_EQ(iter->second->version, 2U);

    iter++;
    ASSERT_TRUE(iter != actives.end());
    ASSERT_EQ(iter->first, "first");
    ASSERT_EQ(iter->second->version, 1U);

    iter++;
    ASSERT_TRUE(iter == actives.end());
}

TEST_CASE(IteratorCheck) {
    ActiveFileManager manager;

    constexpr static size_t TotalInsertedNum = 10;
    constexpr static size_t MaxActiveFileNum = 3;
    manager.set_capability(MaxActiveFileNum);

    // insert file from (1 .. TotalInsertedNum).
    // so there should be (TotalInsertedNum - MaxActiveFileNum) after inserted
    for(uint32_t i = 1; i <= TotalInsertedNum; i++) {
        std::string fpath = std::format("{}", i);
        OpenFile object{.version = i};

        auto& inseted = manager.add(fpath, std::move(object));
        std::optional new_added_entry = manager.get_or_add(fpath);
        ASSERT_TRUE(new_added_entry.has_value());
        auto new_added = std::move(new_added_entry).value();
        ASSERT_TRUE(inseted == new_added);
        ASSERT_TRUE(new_added != nullptr);
        ASSERT_EQ(new_added->version, i);

        auto& [path, openfile] = *manager.begin();
        ASSERT_EQ(path, fpath);
        ASSERT_EQ(openfile->version, new_added->version);
    }

    ASSERT_EQ(manager.size(), manager.max_size());

    // the remain file should be in reversed order.
    auto iter = manager.begin();
    int i = TotalInsertedNum;
    while(iter != manager.end()) {
        auto& [path, openfile] = *iter;
        ASSERT_EQ(path, std::to_string(i));
        ASSERT_EQ(openfile->version, static_cast<uint32_t>(i));
        iter++;
        i--;
    }
}

};  // TEST_SUITE(ActiveFileManager)

}  // namespace
}  // namespace clice::testing
