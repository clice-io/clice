module clice;

import :semantic.type_lines;

namespace clice::display {

namespace {

/// A type taken apart where a card may break it.
struct Shape {
    enum class Kind : std::uint8_t {
        /// What C++ prints in one piece: a builtin, a class, an alias.
        Named,
        Template,
        Pointer,
        Reference,
        Array,
        Function,
        /// A member pointer.
        Member,
        /// Arguments, `int, long`.
        List,
    };

    Kind kind = Kind::Named;

    /// Named: the type as C++ prints it. Template: the template's name.
    /// Pointer: its qualifiers, ` const`. Reference: `&` or `&&`. Array: its
    /// bound, `[4]`. Function: its qualifiers and exception specification,
    /// `const& noexcept`. Member: the class.
    std::string text;

    /// Template: its qualifiers, `const `.
    std::string before;

    /// Template: what follows its arguments, `::iterator`. Member: its
    /// qualifiers.
    std::string after;

    /// Template, List: its arguments. Pointer, Reference, Array, Member: what
    /// it applies to. Function: its return type, then its parameters.
    std::vector<Shape> parts;

    /// Function: whether it takes C's variable arguments.
    bool variadic = false;
};

using Kind = Shape::Kind;

/// The columns `text` takes: one per character, though the letters standing
/// for parts take more than one byte.
auto columns(llvm::StringRef text) -> std::size_t {
    return static_cast<std::size_t>(ranges::count_if(text, [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

class Builder {
public:
    Builder(clang::ASTContext& context, const clang::PrintingPolicy& policy) :
        context(context), policy(policy) {}

    auto build(clang::QualType type) -> Shape {
        auto qualifiers = type.getLocalQualifiers();
        const auto* node = type.getTypePtr();
        switch(node->getTypeClass()) {
            /// Sugar C++ prints as what it wraps.
            case clang::Type::Paren:
            case clang::Type::Adjusted:
            case clang::Type::Decayed:
            case clang::Type::SubstTemplateTypeParm: {
                return build(
                    context.getQualifiedType(node->getLocallyUnqualifiedSingleStepDesugaredType(),
                                             qualifiers));
            }
            case clang::Type::Auto: {
                auto deduced = llvm::cast<clang::AutoType>(node)->getDeducedType();
                if(deduced.isNull()) {
                    return named(type);
                }
                return build(context.getQualifiedType(deduced, qualifiers));
            }
            case clang::Type::Pointer: {
                Shape shape{.kind = Kind::Pointer};
                if(!qualifiers.empty()) {
                    shape.text = " " + qualifiers.getAsString(policy);
                }
                shape.parts.push_back(build(node->getPointeeType()));
                return shape;
            }
            case clang::Type::LValueReference:
            case clang::Type::RValueReference: {
                const auto* reference = llvm::cast<clang::ReferenceType>(node);
                Shape shape{.kind = Kind::Reference,
                            .text = reference->isLValueReferenceType() ? "&" : "&&"};
                shape.parts.push_back(build(reference->getPointeeTypeAsWritten()));
                return shape;
            }
            case clang::Type::MemberPointer: {
                const auto* member = llvm::cast<clang::MemberPointerType>(node);
                std::string scope;
                llvm::raw_string_ostream os(scope);
                member->getQualifier().print(os, policy);
                Shape shape{.kind = Kind::Member};
                shape.text = llvm::StringRef(scope).drop_back(2).str();
                if(!qualifiers.empty()) {
                    shape.after = " " + qualifiers.getAsString(policy);
                }
                shape.parts.push_back(build(member->getPointeeType()));
                return shape;
            }
            case clang::Type::ConstantArray:
            case clang::Type::IncompleteArray:
            case clang::Type::VariableArray:
            case clang::Type::DependentSizedArray: {
                /// What qualifies an array qualifies its elements.
                const auto* array = llvm::cast<clang::ArrayType>(node);
                Shape shape{.kind = Kind::Array, .text = "[" + bound(*array) + "]"};
                shape.parts.push_back(
                    build(context.getQualifiedType(array->getElementType(), qualifiers)));
                return shape;
            }
            case clang::Type::FunctionProto: {
                const auto* function = llvm::cast<clang::FunctionProtoType>(node);
                Shape shape{.kind = Kind::Function, .variadic = function->isVariadic()};
                shape.text = function->getMethodQuals().getAsString(policy);
                if(function->getRefQualifier() != clang::RQ_None) {
                    shape.text += function->getRefQualifier() == clang::RQ_LValue ? "&" : "&&";
                }
                std::string exceptions;
                llvm::raw_string_ostream os(exceptions);
                function->printExceptionSpecification(os, policy);
                if(auto spec = llvm::StringRef(exceptions).trim(); !spec.empty()) {
                    shape.text += (shape.text.empty() ? "" : " ") + spec.str();
                }
                shape.parts.push_back(build(function->getReturnType()));
                for(auto param: function->getParamTypes()) {
                    shape.parts.push_back(build(param));
                }
                return shape;
            }
            case clang::Type::FunctionNoProto: {
                Shape shape{.kind = Kind::Function};
                shape.parts.push_back(
                    build(llvm::cast<clang::FunctionType>(node)->getReturnType()));
                return shape;
            }
            case clang::Type::TemplateSpecialization: {
                const auto& id = *llvm::cast<clang::TemplateSpecializationType>(node);
                std::string name;
                llvm::raw_string_ostream os(name);
                id.getTemplateName().print(os, policy);
                llvm::SmallVector<Argument> arguments;
                flatten(id.template_arguments(), nullptr, arguments);
                return specialization(type, name, arguments);
            }
            case clang::Type::Record: {
                const auto* spec =
                    llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(node->getAsRecordDecl());
                if(!spec) {
                    break;
                }
                llvm::SmallVector<Argument> arguments;
                if(const auto* written = spec->getTemplateArgsAsWritten();
                   written && !policy.PrintAsCanonical) {
                    llvm::SmallVector<clang::TemplateArgument> args;
                    for(const auto& arg: written->arguments()) {
                        args.push_back(arg.getArgument());
                    }
                    flatten(args, nullptr, arguments);
                } else {
                    flatten(shown_arguments(*spec),
                            spec->getSpecializedTemplate()->getTemplateParameters(),
                            arguments);
                }
                /// A class prints its scope and name as a tag does: what
                /// precedes its arguments when printed whole.
                auto whole = print(type);
                auto tail = "<" + raw(arguments) + ">";
                if(!llvm::StringRef(whole).ends_with(tail)) {
                    break;
                }
                auto name = whole.substr(0, whole.size() - tail.size());
                auto before = prefix(qualifiers);
                if(!llvm::StringRef(name).starts_with(before)) {
                    break;
                }
                return specialization(type, name.substr(before.size()), arguments);
            }
            default: break;
        }
        if(auto shape = member_of_specialization(type)) {
            return std::move(*shape);
        }
        return named(type);
    }

    auto list(llvm::ArrayRef<clang::TemplateArgument> arguments) -> Shape {
        Shape shape{.kind = Kind::List};
        for(const auto& argument: arguments) {
            shape.parts.push_back(part({argument, false}));
        }
        return shape;
    }

private:
    /// A template argument, and whether C++ prints it with its type.
    struct Argument {
        clang::TemplateArgument argument;
        bool include_type;
    };

    auto print(clang::QualType type) const -> std::string {
        std::string text;
        llvm::raw_string_ostream os(text);
        type.print(os, policy);
        return text;
    }

    auto print(const Argument& argument) const -> std::string {
        std::string text;
        llvm::raw_string_ostream os(text);
        argument.argument.print(policy, os, argument.include_type);
        return text;
    }

    auto raw(llvm::ArrayRef<Argument> arguments) const -> std::string {
        std::string text;
        for(const auto& [index, argument]: views::enumerate(arguments)) {
            text += (index == 0 ? "" : ", ") + print(argument);
        }
        return text;
    }

    auto prefix(clang::Qualifiers qualifiers) const -> std::string {
        return qualifiers.empty() ? "" : qualifiers.getAsString(policy) + " ";
    }

    auto named(clang::QualType type) const -> Shape {
        return Shape{.kind = Kind::Named, .text = print(type)};
    }

    auto bound(const clang::ArrayType& array) const -> std::string {
        if(const auto* constant = llvm::dyn_cast<clang::ConstantArrayType>(&array)) {
            return std::to_string(constant->getZExtSize());
        }
        const clang::Expr* size = nullptr;
        if(const auto* variable = llvm::dyn_cast<clang::VariableArrayType>(&array)) {
            size = variable->getSizeExpr();
        } else if(const auto* dependent = llvm::dyn_cast<clang::DependentSizedArrayType>(&array)) {
            size = dependent->getSizeExpr();
        }
        if(!size) {
            return llvm::isa<clang::VariableArrayType>(array) ? "*" : "";
        }
        std::string text;
        llvm::raw_string_ostream os(text);
        size->printPretty(os, nullptr, policy);
        return text;
    }

    /// The arguments C++ prints for `spec`: the trailing ones a default
    /// gave left out, as for any class template specialization.
    auto shown_arguments(const clang::ClassTemplateSpecializationDecl& spec) const
        -> llvm::ArrayRef<clang::TemplateArgument> {
        auto arguments = spec.getTemplateArgs().asArray();
        if(!policy.SuppressDefaultTemplateArgs) {
            return arguments;
        }
        const auto* params = spec.getSpecializedTemplate()->getTemplateParameters();
        while(!arguments.empty() && arguments.size() <= params->size() &&
              (arguments.back().getIsDefaulted() ||
               clang::isSubstitutedDefaultArgument(context,
                                                   arguments.back(),
                                                   params->getParam(arguments.size() - 1),
                                                   arguments.drop_back(),
                                                   params->getDepth()))) {
            arguments = arguments.drop_back();
        }
        return arguments;
    }

    /// `arguments` one by one, a pack's elements in its place, each with
    /// whether C++ prints it with its type as its parameter in `params`
    /// asks.
    void flatten(llvm::ArrayRef<clang::TemplateArgument> arguments,
                 const clang::TemplateParameterList* params,
                 llvm::SmallVectorImpl<Argument>& out) const {
        for(const auto& [index, argument]: views::enumerate(arguments)) {
            bool include_type = clang::TemplateParameterList::shouldIncludeTypeForArgument(
                policy,
                params,
                static_cast<unsigned>(index));
            if(argument.getKind() == clang::TemplateArgument::Pack) {
                for(const auto& element: argument.getPackAsArray()) {
                    out.push_back({element, include_type});
                }
            } else {
                out.push_back({argument, include_type});
            }
        }
    }

    /// `type`, `name<arguments>` as C++ prints it, its arguments taken
    /// apart; one piece when the pieces would print otherwise.
    auto specialization(clang::QualType type, std::string name, llvm::ArrayRef<Argument> arguments)
        -> Shape {
        Shape shape{
            .kind = Kind::Template,
            .text = std::move(name),
            .before = prefix(type.getLocalQualifiers()),
        };
        if(shape.before + shape.text + "<" + raw(arguments) + ">" != print(type)) {
            return named(type);
        }
        for(const auto& argument: arguments) {
            shape.parts.push_back(part(argument));
        }
        return shape;
    }

    auto part(const Argument& argument) -> Shape {
        if(argument.argument.getKind() == clang::TemplateArgument::Type) {
            return build(argument.argument.getAsType());
        }
        return Shape{.kind = Kind::Named, .text = print(argument)};
    }

    /// A type a specialization's scope names, `std::map<K, V>::iterator`:
    /// the specialization, its arguments taken apart, then the rest.
    auto member_of_specialization(clang::QualType type) -> std::optional<Shape> {
        auto scope = type->getPrefix();
        if(scope.getKind() != clang::NestedNameSpecifier::Kind::Type) {
            return std::nullopt;
        }
        auto shape = build(clang::QualType(scope.getAsType(), 0));
        if(shape.kind != Kind::Template) {
            return std::nullopt;
        }
        auto whole = print(type);
        shape.before = prefix(type.getLocalQualifiers());
        auto head = shape.before + print(clang::QualType(scope.getAsType(), 0));
        if(!llvm::StringRef(whole).starts_with(head)) {
            return std::nullopt;
        }
        shape.after += whole.substr(head.size());
        return shape;
    }

private:
    clang::ASTContext& context;

    const clang::PrintingPolicy& policy;
};

bool is_group(const Shape& shape) {
    return shape.kind == Kind::Function || shape.kind == Kind::Member;
}

/// The parts of `shape` in the order its line writes them.
auto written_parts(const Shape& shape) -> llvm::SmallVector<const Shape*> {
    llvm::SmallVector<const Shape*> parts;
    for(const auto& part: shape.parts) {
        parts.push_back(&part);
    }
    if(shape.kind == Kind::Function) {
        /// A function writes its parameters, then what it returns.
        ranges::rotate(parts, parts.begin() + 1);
    }
    return parts;
}

/// The letters a line writes parts as.
using Names = llvm::DenseMap<const Shape*, llvm::StringRef>;

auto render(const Shape& shape, const Names& names) -> std::string;

/// `shape` as it follows another part or precedes `*`, `&` and `[N]`: a
/// function or a member pointer in parentheses.
auto operand(const Shape& shape, const Names& names) -> std::string {
    auto text = render(shape, names);
    return is_group(shape) && !names.contains(&shape) ? "(" + text + ")" : text;
}

auto parameters(const Shape& function, const Names& names) -> std::string {
    std::string text;
    for(const auto& [index, param]: views::enumerate(llvm::ArrayRef(function.parts).drop_front())) {
        text += (index == 0 ? "" : ", ") + render(param, names);
    }
    if(function.variadic) {
        text += function.parts.size() == 1 ? "..." : ", ...";
    }
    return text;
}

auto render(const Shape& shape, const Names& names) -> std::string {
    if(auto name = names.find(&shape); name != names.end()) {
        return name->second.str();
    }
    switch(shape.kind) {
        case Kind::Named: return shape.text;
        case Kind::Template: {
            std::string text = shape.before + shape.text + "<";
            for(const auto& [index, argument]: views::enumerate(shape.parts)) {
                text += (index == 0 ? "" : ", ") + render(argument, names);
            }
            return text + ">" + shape.after;
        }
        case Kind::Pointer: return operand(shape.parts[0], names) + "*" + shape.text;
        case Kind::Reference:
        case Kind::Array: return operand(shape.parts[0], names) + shape.text;
        case Kind::Member: {
            auto text = shape.text + "::-> " + operand(shape.parts[0], names);
            return shape.after.empty() ? text : "(" + text + ")" + shape.after;
        }
        case Kind::Function: {
            auto qualifiers = shape.text.empty() ? "" : shape.text + " ";
            return qualifiers + "(" + parameters(shape, names) + ") -> " +
                   render(shape.parts[0], names);
        }
        case Kind::List: {
            std::string text;
            for(const auto& [index, argument]: views::enumerate(shape.parts)) {
                text += (index == 0 ? "" : ", ") + render(argument, names);
            }
            return text;
        }
    }
    std::unreachable();
}

auto spelling(const Shape& shape) -> std::string {
    return render(shape, Names());
}

/// The letters a type writes parts as, as many as it defines on lines of
/// their own; past them a part is written `…`. No Latin letter looks like
/// them.
constexpr std::array<llvm::StringLiteral, 8> letters = {"α", "β", "γ", "δ", "ε", "ζ", "η", "θ"};

/// The columns a part takes before a line may write it as a letter: a
/// shorter one saves less than the line its definition takes.
constexpr std::size_t worth_a_line = 24;

/// What a line for `root` writes as letters, in the order it writes them:
/// first what nests a function in a function or a member pointer in a
/// member pointer, then what does not fit in `width`.
auto lifted(const Shape& root, std::size_t width) -> llvm::SmallVector<const Shape*> {
    Names names;
    llvm::DenseMap<const Shape*, const Shape*> parents;

    /// Every part the line shows, outermost first.
    auto shown = [&](const Shape& from) {
        llvm::SmallVector<const Shape*> nodes;
        llvm::SmallVector<const Shape*> stack = {&from};
        while(!stack.empty()) {
            const auto* node = stack.pop_back_val();
            if(node != &from && names.contains(node)) {
                continue;
            }
            nodes.push_back(node);
            auto parts = written_parts(*node);
            for(const auto* part: views::reverse(parts)) {
                parents[part] = node;
                stack.push_back(part);
            }
        }
        return nodes;
    };

    /// A part the line writes twice is lifted both times.
    auto lift = [&](const Shape* part) {
        auto text = spelling(*part);
        for(const auto* node: shown(root)) {
            if(node != &root && spelling(*node) == text) {
                names[node] = letters[0];
            }
        }
    };

    for(bool nested = true; nested;) {
        nested = false;
        for(const auto* outer: shown(root)) {
            if(!is_group(*outer)) {
                continue;
            }
            for(const auto* part: written_parts(*outer)) {
                if(names.contains(part)) {
                    continue;
                }
                auto inner = shown(*part);
                if(ranges::any_of(inner,
                                  [&](const Shape* node) { return node->kind == outer->kind; })) {
                    lift(part);
                    nested = true;
                    break;
                }
            }
            if(nested) {
                break;
            }
        }
    }

    /// The text `node` takes in the line as it stands.
    auto written = [&](const Shape* node) {
        const auto* parent = parents.lookup(node);
        bool operand_of =
            parent && (parent->kind == Kind::Pointer || parent->kind == Kind::Reference ||
                       parent->kind == Kind::Array || parent->kind == Kind::Member);
        return operand_of ? operand(*node, names) : render(*node, names);
    };

    /// Past as many parts as there are letters, a part lifted would read `…`.
    for(std::size_t lifts = 0; lifts < letters.size(); lifts += 1) {
        auto length = columns(render(root, names));
        if(length <= width) {
            break;
        }
        llvm::SmallVector<const Shape*> candidates;
        for(const auto* node: shown(root)) {
            if(node != &root && columns(spelling(*node)) > worth_a_line) {
                candidates.push_back(node);
            }
        }
        /// A template's only argument reads better inside it than below it.
        auto sole = [&](const Shape* node) {
            const auto* parent = parents.lookup(node);
            return parent && parent->kind == Kind::Template && parent->parts.size() == 1;
        };
        if(ranges::any_of(candidates, [&](const Shape* node) { return !sole(node); })) {
            candidates.erase(ranges::remove_if(candidates, sole).begin(), candidates.end());
        }
        if(candidates.empty()) {
            break;
        }
        const Shape* pick = nullptr;
        std::size_t best = 0;
        for(const auto* candidate: candidates) {
            auto remaining = length - columns(written(candidate)) + 1;
            if(remaining <= width && remaining > best) {
                pick = candidate;
                best = remaining;
            }
        }
        if(!pick) {
            pick = *ranges::max_element(candidates, {}, [](const Shape* node) {
                return columns(spelling(*node));
            });
        }
        lift(pick);
    }

    llvm::SmallVector<const Shape*> order;
    for(const auto* node: shown(root)) {
        for(const auto* part: written_parts(*node)) {
            if(names.contains(part)) {
                order.push_back(part);
            }
        }
    }
    return order;
}

/// The lines for one type: what each line lifts, decided first, and the
/// letters given in the order the lines are drawn.
class Lines {
public:
    explicit Lines(std::size_t width) : width(width) {}

    auto write(const Shape& root) -> std::string {
        auto entry = plan(root, 0, 0);
        assign(entry);
        draw(entry, 0, "");
        return llvm::join(lines, "\n");
    }

private:
    struct Entry {
        const Shape* shape;
        llvm::SmallVector<const Shape*> lifted;
        std::vector<Entry> children;
    };

    /// The line for `shape`, `indent` columns in after a lead of `lead`
    /// columns, and the lines defining what it lifts, two columns further
    /// in after `α = `.
    auto plan(const Shape& shape, std::size_t indent, std::size_t lead) -> Entry {
        auto margin = indent + lead;
        Entry entry{.shape = &shape, .lifted = lifted(shape, width > margin ? width - margin : 0)};
        for(const auto* part: entry.lifted) {
            if(defined.try_emplace(spelling(*part)).second) {
                /// One past the letters is written `…`, and so is all it lifts.
                entry.children.push_back(defined.size() <= letters.size()
                                             ? plan(*part, indent + 2, 4)
                                             : Entry{.shape = part});
            }
        }
        return entry;
    }

    /// Past the letters, a part and what its line would lift are all `…`.
    void assign(const Entry& entry, bool spent = false) {
        for(const auto& child: entry.children) {
            auto& letter = defined[spelling(*child.shape)];
            spent = spent || next == letters.size();
            if(spent) {
                letter = "…";
            } else {
                letter = letters[next];
                next += 1;
            }
            assign(child, spent);
        }
    }

    void draw(const Entry& entry, std::size_t indent, llvm::StringRef lead) {
        Names names;
        for(const auto* part: entry.lifted) {
            names[part] = defined.lookup(spelling(*part));
        }
        auto margin = std::string(indent, ' ') + lead.str();
        auto room = width > columns(margin) ? width - columns(margin) : 0;
        auto pieces = split(*entry.shape, names, room);
        for(const auto& [index, piece]: views::enumerate(pieces)) {
            lines.push_back((index == 0 ? margin : std::string(columns(margin), ' ')) + piece);
        }
        for(const auto& child: entry.children) {
            auto letter = defined.lookup(spelling(*child.shape));
            if(letter != "…") {
                draw(child, indent + 2, letter.str() + " = ");
            }
        }
    }

    /// `shape` on one line, or a template's arguments or a function's
    /// parameters too many to fit one on lines of their own, aligned after
    /// the bracket opening them.
    static auto split(const Shape& shape, const Names& names, std::size_t room)
        -> llvm::SmallVector<std::string> {
        auto text = render(shape, names);
        if(columns(text) <= room || (shape.kind != Kind::Template && shape.kind != Kind::Function &&
                                     shape.kind != Kind::List)) {
            return {text};
        }
        std::string head;
        std::string tail;
        llvm::SmallVector<std::string> items;
        if(shape.kind == Kind::List) {
            for(const auto& argument: shape.parts) {
                items.push_back(render(argument, names));
            }
        } else if(shape.kind == Kind::Template) {
            head = shape.before + shape.text + "<";
            tail = ">" + shape.after;
            for(const auto& argument: shape.parts) {
                items.push_back(render(argument, names));
            }
        } else {
            head = (shape.text.empty() ? "" : shape.text + " ") + "(";
            tail = ") -> " + render(shape.parts[0], names);
            for(const auto& param: llvm::ArrayRef(shape.parts).drop_front()) {
                items.push_back(render(param, names));
            }
            if(shape.variadic) {
                items.push_back("...");
            }
        }
        if(items.empty()) {
            return {text};
        }
        llvm::SmallVector<std::string> pieces = {head};
        for(const auto& [index, item]: views::enumerate(items)) {
            auto piece = item + (static_cast<std::size_t>(index) + 1 == items.size() ? tail : ",");
            auto& current = pieces.back();
            bool fresh = current.find_first_not_of(' ') == std::string::npos || current == head;
            if(!fresh && columns(current) + 1 + columns(piece) > room) {
                pieces.push_back(std::string(columns(head), ' ') + piece);
            } else {
                current += (fresh ? "" : " ") + piece;
            }
        }
        return pieces;
    }

private:
    std::size_t width;

    /// The letter each part defined on its own line goes by, by its
    /// spelling: a part spelled twice is defined once.
    llvm::StringMap<llvm::StringRef> defined;

    std::size_t next = 0;

    std::vector<std::string> lines;
};

}  // namespace

auto type_lines(clang::ASTContext& context,
                clang::QualType type,
                const clang::PrintingPolicy& policy,
                std::uint32_t width) -> std::string {
    auto shape = Builder(context, policy).build(type);
    return Lines(width).write(shape);
}

auto argument_lines(clang::ASTContext& context,
                    llvm::ArrayRef<clang::TemplateArgument> arguments,
                    const clang::PrintingPolicy& policy,
                    std::uint32_t width) -> std::string {
    auto shape = Builder(context, policy).list(arguments);
    return Lines(width).write(shape);
}

}  // namespace clice::display
