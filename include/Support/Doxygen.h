#pragma once

#include <memory>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class DoxygenInfo {
public:
    struct BlockCommandCommentContent {
        std::string content;
    };

    struct ParamCommandCommentContent {
        std::string content;
        enum class ParamDirection : uint8_t { Unspecified, In, Out, InOut };
        ParamDirection direction = ParamDirection::Unspecified;
    };

    void add_block_command_comment(llvm::StringRef tag, llvm::StringRef content);

    void add_param_command_comment(llvm::StringRef name,
                                   llvm::StringRef content,
                                   ParamCommandCommentContent::ParamDirection direction =
                                       ParamCommandCommentContent::ParamDirection::Unspecified);

    /// \param ret_info docs for return value
    /// \param cover if already has docs for return, new value cover the old one
    void add_return_info(llvm::StringRef ret_info, bool cover = true) {
        if(!doc_for_return.has_value() || cover) {
            doc_for_return = ret_info.str();
        }
    }

    std::optional<ParamCommandCommentContent*> find_param_info(llvm::StringRef name);

    std::vector<std::pair<llvm::StringRef, llvm::ArrayRef<BlockCommandCommentContent>>>
        get_block_command_comments();

    std::optional<llvm::StringRef> get_return_info() {
        return doc_for_return;
    }

private:
    llvm::SmallDenseMap<llvm::StringRef, std::vector<BlockCommandCommentContent>>
        block_command_comments;
    llvm::SmallDenseMap<llvm::StringRef, ParamCommandCommentContent> param_command_comments;
    std::optional<std::string> doc_for_return;
};

/// Strip doxygen info from raw comment
/// \return `DoxygenInfo` and the rest comment
std::pair<DoxygenInfo, std::string> strip_doxygen_info(llvm::StringRef raw_comment);

}  // namespace clice
