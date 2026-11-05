module clice:compile.shared_header;

import :support.prelude;

namespace clice {

/// A macro, by the hash of its name, and its state where it is read or
/// written: the hash of its definition (CompilationUnit::macro_state), 0
/// for none.
struct MacroState {
    std::uint64_t name = 0;

    std::uint64_t state = 0;

    friend bool operator==(const MacroState&, const MacroState&) = default;
};

/// What the build of a shared header — the includes clice writes for the
/// units of one command, whose PCH they load instead of each parsing what
/// those include — learned of the headers the PCH holds: those whose guard
/// it leaves defined, which a unit loading it skips where its own text
/// includes them. Enough for the unit to tell whether parsing a held header
/// itself would have read the macros the build read
/// (CompilationParams::shared), and to put what the header included in its
/// own include tree.
struct SharedHeaderRecord {
    constexpr static std::uint32_t none = ~0u;

    struct File {
        std::string path;

        /// The hash of the bytes the build read.
        std::uint64_t content_hash = 0;

        friend bool operator==(const File&, const File&) = default;
    };

    /// An include directive a held header ran, or a file it entered
    /// without a guard ran: the latter's are the header's too, and so are
    /// the macros it read and wrote.
    struct Include {
        /// The include of the file that ran this one, none for the
        /// header's own text.
        std::uint32_t parent = none;

        std::uint32_t line = 0;

        /// Into `files`.
        std::uint32_t file = 0;

        /// The held header it names, none for a file it entered without a
        /// guard, whose includes follow it.
        std::uint32_t held = none;

        /// How many of the header's writes came before it.
        std::uint32_t writes = 0;

        friend bool operator==(const Include&, const Include&) = default;
    };

    struct Header {
        /// Into `files`.
        std::uint32_t file = 0;

        bool system = false;

        /// The macros its run read before it wrote them, with the state it
        /// read, identifiers no macro names among them (state 0).
        std::vector<MacroState> reads;

        /// Each macro its run wrote, in order, with the state it left.
        std::vector<MacroState> writes;

        std::vector<Include> includes;

        friend bool operator==(const Header&, const Header&) = default;
    };

    std::vector<File> files;

    /// In the order the build entered them.
    std::vector<Header> headers;

    /// The headers the command forces in (`-include`), in order: clang
    /// leaves them out of a unit loading the PCH, whose command forces in
    /// the same.
    std::vector<std::uint32_t> forced;

    /// A macro the build wrote, with the state the command gave it.
    struct Written {
        std::uint64_t name = 0;

        std::string spelling;

        std::uint64_t initial = 0;

        /// A file outside the system headers wrote it.
        bool project = false;

        friend bool operator==(const Written&, const Written&) = default;
    };

    /// Sorted by name.
    std::vector<Written> written;

    /// The state the command gave the macros a header read and none wrote,
    /// those it defined, sorted by name.
    std::vector<MacroState> initial;

    friend bool operator==(const SharedHeaderRecord&, const SharedHeaderRecord&) = default;
};

/// An include of a held header the unit loading a shared header's PCH
/// would have run, and what that header included in turn: the unit's
/// include tree as if it had parsed the header itself.
struct GraftedInclude {
    /// The unit's file holding the directive, for one the unit's own text
    /// skipped.
    clang::FileID includer;

    /// The grafted include whose file ran this one, none for one of the
    /// unit's own.
    std::uint32_t parent = SharedHeaderRecord::none;

    std::uint32_t line = 0;

    /// Into SharedHeaderRecord::files.
    std::uint32_t file = 0;

    /// It found the header entered already.
    bool skipped = false;
};

}  // namespace clice
