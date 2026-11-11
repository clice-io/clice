module clice;

import :index.database;
import :index.indexing.tu_index;
import :index.project_index;
import :index.project_index.records;
import :index.shard;
import :index.shard.instantiations;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;

namespace clice::index {

namespace {

/// Put `path` in `files` or take it out; whether that changed the set.
bool set_file(Bitmap& files, std::uint32_t path, bool present) {
    return present ? files.addChecked(path) : files.removeChecked(path);
}

/// The search ranks a symbol by the logarithm of how many files reference
/// it: a count crossing a power of two moves it far enough to re-read.
auto searched_count(const Symbol& symbol) {
    return std::bit_width(symbol.reference_files.cardinality());
}

}  // namespace

const Symbol* ProjectIndex::symbol(SymbolHash hash) const {
    return read(symbol_records, number_key(hash), decode_symbol);
}

const Symbol* ProjectIndex::files_of(SymbolHash hash) const {
    auto key = number_key(hash);
    if(auto* external = read(symbol_records, key, decode_symbol)) {
        return external;
    }
    return read(local_records, key, decode_symbol);
}

std::optional<SymbolView> ProjectIndex::find_symbol(SymbolHash hash) const {
    auto* found = symbol(hash);
    if(!found) {
        return std::nullopt;
    }
    auto view = found->view();
    if(view.file != no_file) {
        view.file = fid_of(view.file).raw;
    }
    return view;
}

std::uint32_t ProjectIndex::reference_count(SymbolHash hash) const {
    auto* found = symbol(hash);
    return found ? static_cast<std::uint32_t>(found->reference_files.cardinality()) : 0;
}

bool ProjectIndex::for_each_file(SymbolHash hash,
                                 SymbolFiles which,
                                 llvm::function_ref<void(Fid)> visit) const {
    auto* found = files_of(hash);
    if(!found) {
        return false;
    }
    auto files = [&]() -> const Bitmap& {
        switch(which) {
            case SymbolFiles::Naming: return found->reference_files;
            case SymbolFiles::Declaring: return found->declaration_files;
            case SymbolFiles::Defining: return found->definition_files;
            case SymbolFiles::Instantiating: return found->instantiation_files;
        }
        std::unreachable();
    };
    for(auto path: files()) {
        if(auto file = fid_of(path); file.valid()) {
            visit(file);
        }
    }
    return true;
}

void ProjectIndex::for_each_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolView&, std::uint32_t)> visit) const {
    scan(symbol_records, decode_symbol, [&](llvm::StringRef key, const Symbol& found) {
        auto hash = key_number<SymbolHash>(key);
        if(!hash || reserved_key(*hash)) {
            damaged_record(BlobKind::Symbol, key);
            return true;
        }
        auto view = found.view();
        if(view.file != no_file) {
            view.file = fid_of(view.file).raw;
        }
        return visit(*hash, view, static_cast<std::uint32_t>(found.reference_files.cardinality()));
    });
}

std::uint32_t ProjectIndex::symbol_count() const {
    return db ? db->count(BlobKind::Symbol) : 0;
}

void ProjectIndex::merge_facts(const TUIndex& index, llvm::SmallVectorImpl<SymbolHash>& added) {
    // Units may spell one symbol differently (`X<int>` against
    // `X<signed int>`, a conversion to a typedef): the shortest spelling
    // wins, then the smaller one, so the record reads the same whatever the
    // merge order — among the units that send the symbol.
    //
    // LIMITATION: what a symbol is can depend on the unit's command — its
    // spelling, SystemHeader under -isystem against -I, Deprecated under a
    // deployment target — and the record folds whatever units send: a flag
    // only some units report stays, and so does one an edit removed (a
    // dropped `[[deprecated]]`) until every file naming the symbol was
    // indexed again or the index is rebuilt. A unit whose every file holding
    // the symbol's entries is a stored variant sends nothing of it
    // (TUIndexOptions::known_variants), so the first unit to store the
    // variant decides.
    auto prefer = [](std::string& current, llvm::StringRef incoming) {
        if(incoming.empty() ||
           (!current.empty() && (incoming.size() > current.size() ||
                                 (incoming.size() == current.size() && incoming >= current)))) {
            return false;
        }
        current = incoming.str();
        return true;
    };
    auto unreported = SymbolFlags::HasDefinition | SymbolFlags::SpelledInMacro;
    auto without = [](SymbolFlags flags, SymbolFlags bits) {
        return static_cast<SymbolFlags>(static_cast<std::uint16_t>(flags) &
                                        ~static_cast<std::uint16_t>(bits));
    };
    index.for_each_symbol(
        [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t>) {
            if(identity.scope != SymbolScope::External) {
                return true;
            }
            auto key = number_key(hash);
            auto* stored = read(symbol_records, key, decode_symbol);
            // The unit defines the symbol; some file another unit indexed
            // may too.
            bool defines = has_flag(identity.flags, SymbolFlags::HasDefinition);
            bool defined = stored && !stored->definition_files.isEmpty();

            auto name = stored ? stored->name : std::string();
            auto args = stored ? stored->args : std::string();
            auto signature = stored ? stored->signature : std::string();
            auto parent = stored ? stored->parent : SymbolHash(0);
            auto kind = stored ? stored->kind : SymbolKind();
            auto token_class = stored ? stored->token_class : TokenClass();
            bool changed = !stored;
            if(name.empty() && !identity.name.empty()) {
                parent = identity.parent;
                kind = identity.kind;
                token_class.kind = identity.token_class.kind;
                changed = true;
            } else if(defines && token_class.kind != identity.token_class.kind) {
                // A declaration may say `struct` where the definition says
                // `class`: the definition's word stands, whatever merges first.
                token_class.kind = identity.token_class.kind;
                changed = true;
            }
            changed = prefer(name, identity.name) || changed;
            changed = prefer(args, identity.args) || changed;
            // An edit to a function, a parameter renamed, keeps its hash: the
            // unit merging last describes it — a unit defining it, when one
            // does, which sees the names of every declaration.
            if(!identity.signature.empty() && signature != identity.signature &&
               (defines || !defined)) {
                signature = identity.signature.str();
                changed = true;
            }
            // Every declaration a unit sees is spelled by a macro; another
            // unit may see one written by hand (`FWD(Expr)` beside the
            // class): the symbol is macro-spelled only while every unit
            // reporting it says so.
            auto flags = without(identity.flags, unreported);
            bool macro_spelled = has_flag(identity.flags, SymbolFlags::SpelledInMacro);
            if(stored) {
                flags = flags | without(stored->flags, SymbolFlags::SpelledInMacro);
                macro_spelled =
                    macro_spelled && has_flag(stored->flags, SymbolFlags::SpelledInMacro);
            }
            if(macro_spelled) {
                flags |= SymbolFlags::SpelledInMacro;
            }
            changed = changed || (stored && flags != stored->flags);
            auto modifiers = token_class.modifiers | identity.token_class.modifiers;
            bool recolored = modifiers != token_class.modifiers;
            token_class.modifiers = modifiers;
            if(!changed && !recolored) {
                return true;
            }
            auto& record = change(symbol_records, key, decode_symbol);
            record.name = std::move(name);
            record.args = std::move(args);
            record.signature = std::move(signature);
            record.parent = parent;
            record.kind = kind;
            record.flags = flags;
            record.token_class = token_class;
            if(changed) {
                added.push_back(hash);
            }
            return true;
        });
}

void ProjectIndex::list_locals(const TUIndex& index, llvm::ArrayRef<Fid> file_ids_map) {
    index.for_each_symbol(
        [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t> listed) {
            if(identity.scope == SymbolScope::External || listed.size() < 2) {
                return true;
            }
            auto key = number_key(hash);
            const Symbol* stored = read(local_records, key, decode_symbol);
            for(auto local_id: listed) {
                // A file the database never named stores no entries: one
                // behind a precompiled header whose unit is not indexed yet.
                auto file = file_ids_map[local_id];
                auto path = known_path(file);
                if(!path || (stored && (stored->reference_files.contains(*path) ||
                                        stored->instantiation_files.contains(*path)))) {
                    continue;
                }
                auto* entries = shard(file);
                auto mention = entries ? entries->mention(hash) : std::nullopt;
                auto* resolved = instantiation_shard(file);
                bool instantiated = resolved && resolved->names(hash);
                if(!mention && !instantiated) {
                    continue;
                }
                auto& record = change(local_records, key, decode_symbol);
                if(mention) {
                    record.reference_files.add(*path);
                    if(mention->declares) {
                        record.declaration_files.add(*path);
                    }
                    if(mention->defines) {
                        record.definition_files.add(*path);
                    }
                }
                if(instantiated) {
                    record.instantiation_files.add(*path);
                }
                stored = &record;
            }
            return true;
        });
}

Symbol* ProjectIndex::change_symbol(SymbolHash hash, bool local) {
    auto& records = local ? local_records : symbol_records;
    auto key = number_key(hash);
    if(!read(records, key, decode_symbol)) {
        return nullptr;
    }
    return &change(records, key, decode_symbol);
}

void ProjectIndex::symbol_changed(SymbolHash hash, bool local, const Symbol& record) {
    if(record.in_no_file()) {
        fileless.emplace_back(hash, local);
    }
    if(!local) {
        search.symbol_changed(hash);
    }
}

void ProjectIndex::drop_fileless_symbols() {
    for(auto [hash, local]: fileless) {
        auto& records = local ? local_records : symbol_records;
        auto key = number_key(hash);
        if(auto* record = read(records, key, decode_symbol); record && record->in_no_file()) {
            records.changed[key] = std::nullopt;
        }
    }
    fileless.clear();
}

void ProjectIndex::update_symbols(Fid file, const Shard* before, const Shard* after) {
    std::vector<Mention> was;
    std::vector<Mention> now;
    if(before) {
        before->for_each_mention([&](const Mention& mention) { was.push_back(mention); });
    }
    if(after) {
        after->for_each_mention([&](const Mention& mention) { now.push_back(mention); });
    }
    auto apply = [&](const Mention* old, const Mention* fresh) {
        auto& either = fresh ? *fresh : *old;
        if(old && fresh && *old == *fresh) {
            return;
        }
        auto* record = change_symbol(either.symbol, either.local);
        if(!record) {
            return;
        }
        auto path = path_id(file);
        auto count = searched_count(*record);
        bool moved = set_file(record->reference_files, path, fresh != nullptr);
        bool declared = set_file(record->declaration_files, path, fresh && fresh->declares);
        declared = set_file(record->definition_files, path, fresh && fresh->defines) || declared;
        // The search files a symbol under the files declaring it, ranks it
        // lower without a definition and by how many files reference it.
        if(record->in_no_file() || declared || (moved && searched_count(*record) != count)) {
            symbol_changed(either.symbol, either.local, *record);
        }
    };
    auto old = was.begin();
    auto fresh = now.begin();
    while(old != was.end() || fresh != now.end()) {
        if(fresh == now.end() || (old != was.end() && old->symbol < fresh->symbol)) {
            apply(&*old, nullptr);
            ++old;
        } else if(old == was.end() || fresh->symbol < old->symbol) {
            apply(nullptr, &*fresh);
            ++fresh;
        } else {
            apply(&*old, &*fresh);
            ++old;
            ++fresh;
        }
    }
}

void ProjectIndex::update_symbols(Fid file,
                                  const InstantiationShard* before,
                                  const InstantiationShard* after) {
    auto was = before ? before->named_symbols() : std::vector<SymbolHash>();
    auto now = after ? after->named_symbols() : std::vector<SymbolHash>();
    auto apply = [&](SymbolHash hash, bool named) {
        bool local = symbol(hash) == nullptr;
        auto* record = change_symbol(hash, local);
        if(!record) {
            return;
        }
        if(set_file(record->instantiation_files, path_id(file), named) && record->in_no_file()) {
            symbol_changed(hash, local, *record);
        }
    };
    std::vector<SymbolHash> gone;
    std::vector<SymbolHash> come;
    ranges::set_difference(was, now, std::back_inserter(gone));
    ranges::set_difference(now, was, std::back_inserter(come));
    for(auto hash: gone) {
        apply(hash, false);
    }
    for(auto hash: come) {
        apply(hash, true);
    }
}

}  // namespace clice::index
