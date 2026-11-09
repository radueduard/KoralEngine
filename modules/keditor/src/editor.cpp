// keditor: the editor's interface — its panels, its graph, its scene view — and the scene it runs in.

#include "editor_internal.h"

namespace keditor
{
    namespace {
        kgraph::NumberLimits LimitsOfPort(const kgraph::Port& port) { return { port.min, port.max, port.logarithmic }; }

        /** The kind of number a drag gives, for a value of @p scalar's: a float's, unless it is said to be another. */
        kui::NumberType NumberTypeOf(const kgraph::Scalar scalar)
        {
            switch (scalar) {
                case kgraph::Scalar::eDouble: return kui::NumberType::eDouble;
                case kgraph::Scalar::eInt: return kui::NumberType::eInt;
                case kgraph::Scalar::eUInt: case kgraph::Scalar::eBool: return kui::NumberType::eUInt;
                default: return kui::NumberType::eFloat;
            }
        }
        /** For a number of a builder's type: of an array's, its element's. */
        kui::NumberType NumberTypeOf(const kb::Type& type)
        {
            const kb::Type& number = type.kind == kb::Kind::eArray && type.element ? *type.element : type;
            switch (number.kind) {
                case kb::Kind::eInt: return kui::NumberType::eInt;
                case kb::Kind::eUInt: return kui::NumberType::eUInt;
                case kb::Kind::eFloat: return number.bits == 64 ? kui::NumberType::eDouble : kui::NumberType::eFloat;
                default: return kui::NumberType::eFloat;
            }
        }
        /**
         * For an input: of exactly what it takes (Port::shape), or its builder argument's kind, or — a count, an index — a
         * whole number, of none or more when it is never less.
         */
        kui::NumberType NumberTypeOf(const kgraph::Port& port)
        {
            if (port.shape.Known()) return NumberTypeOf(port.shape.scalar);
            if (port.builder) return NumberTypeOf(*port.builder);
            if (port.integer) return port.min >= 0.f ? kui::NumberType::eUInt : kui::NumberType::eInt;
            return kui::NumberType::eFloat;
        }
        /** For a field of a component, by its type's name: "int", "uvec3", "double", "vec4". */
        kui::NumberType NumberTypeOfField(const std::string_view type)
        {
            if (type.starts_with("i")) return kui::NumberType::eInt;
            if (type.starts_with("u") || type.starts_with("b")) return kui::NumberType::eUInt;
            if (type.starts_with("d")) return kui::NumberType::eDouble;
            return kui::NumberType::eFloat;
        }

        /** The numbers of a value as @p shape has them: as many as it has, what the value holds or zeros. */
        std::vector<float> NumbersOfShape(const Value& value, const kgraph::Shape shape)
        {
            const Value made = kgraph::Conform(value.IsNull() ? Value(0.0) : value, shape);
            std::vector<float> numbers;
            if (const auto* items = made.Items())
                for (const Value& each : *items) numbers.push_back(each.Number() ? static_cast<float>(*each.Number()) : each.Bool() && *each.Bool() ? 1.f : 0.f);
            else numbers.push_back(made.Number() ? static_cast<float>(*made.Number()) : made.Bool() && *made.Bool() ? 1.f : 0.f);
            numbers.resize(shape.components, 0.f);
            return numbers;
        }
        /** What numbers dragged are, as @p shape takes them: one, or a vector of them, each of its kind. */
        Value ValueOfShape(const std::vector<float>& numbers, const kgraph::Shape shape)
        {
            if (numbers.size() == 1) return kgraph::Conform(Value(numbers.front()), shape);
            return kgraph::Conform(Value(Value::List(numbers.begin(), numbers.end())), shape);
        }

        /**
         * An editor for a value of a builder's type, whatever it is: what a node's inspector is made of. A number of it —
         * each of an array's — is kept within @p limits, an unsigned one at none or more; a field of a struct, within
         * what kgraph::LimitsOf says of it.
         */
        kui::Widget TypeEditor(const kb::Type& type, const Value& value, const std::function<void(Value)>& set, const std::string& key,
                               kgraph::NumberLimits limits = {})
        {
            using kb::Kind;
            if (type.kind == Kind::eUInt || (type.kind == Kind::eArray && type.element && type.element->kind == Kind::eUInt)) limits.min = std::max(limits.min, 0.f);
            switch (type.kind) {
            case Kind::eBool:
                return kui::Checkbox(value.Bool() && *value.Bool(), [set](const bool on) { set(Value(on)); });
            case Kind::eInt:
            case Kind::eUInt:
            case Kind::eFloat:
                return kui::DragValue(NumberOf(value), [set](const double v) { set(Value(v)); }, Dragging(limits, NumberTypeOf(type)));
            case Kind::eString: {
                const std::string text = value.String() ? *value.String() : std::string();
                return kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
            }
            case Kind::eEnum:
            case Kind::eChoice: {
                std::vector<std::string> names;
                int selected = -1;
                if (type.enumeration)
                    for (const auto& entry : type.enumeration->values) {
                        if ((value.String() && *value.String() == entry.name) || (value.Number() && static_cast<std::uint64_t>(*value.Number()) == entry.value))
                            selected = static_cast<int>(names.size());
                        names.emplace_back(entry.name);
                    }
                return kui::Dropdown(names, selected, [set, names](const int index) { set(Value(names[static_cast<std::size_t>(index)])); },
                                     kui::DropdownOptions {}.SetPlaceholder("(as it is by default)"));
            }
            case Kind::eFlags: {
                std::set<std::string> on;
                if (const auto* items = value.Items()) { for (const auto& item : *items) if (item.String()) on.insert(*item.String()); }
                else if (value.String()) on.insert(*value.String());
                std::vector<kui::Widget> boxes;
                if (type.enumeration)
                    for (const auto& entry : type.enumeration->values) {
                        const std::string name(entry.name);
                        boxes.push_back(kui::Checkbox(on.contains(name), [set, on, name](const bool checked) {
                            auto changed = on;
                            if (checked) changed.insert(name); else changed.erase(name);
                            set(Value(Value::List(changed.begin(), changed.end())));
                        }, name));
                    }
                return kui::Column(std::move(boxes), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f));
            }
            case Kind::eArray: {
                // A few of one enumeration side by side: a view's swizzle, a choice for each of red, green, blue and alpha.
                if (type.element && type.element->kind == Kind::eEnum && type.element->enumeration) {
                    const auto& entries = type.element->enumeration->values;
                    std::vector<std::string> names;
                    for (const auto& entry : entries) names.emplace_back(entry.name);
                    Value::List chosen(type.length, Value(names.empty() ? std::string() : names.front()));
                    if (const auto* items = value.Items())
                        for (std::size_t i = 0; i < chosen.size() && i < items->size(); ++i) {
                            const Value& item = (*items)[i];
                            if (item.String()) chosen[i] = item;
                            else if (item.Number())
                                for (const auto& entry : entries)
                                    if (entry.value == static_cast<std::uint64_t>(*item.Number())) chosen[i] = Value(std::string(entry.name));
                        }
                    static constexpr const char* channels[] { "R", "G", "B", "A" };
                    const bool swizzle = std::string_view(type.element->enumeration->name).find("Swizzle") != std::string_view::npos && type.length <= 4;
                    std::vector<kui::Widget> parts;
                    for (std::size_t i = 0; i < chosen.size(); ++i) {
                        const std::string* now = chosen[i].String();
                        const auto at = now ? std::ranges::find(names, *now) : names.end();
                        // A swizzle's choices said as what they give: identity, zero, one, or a channel of the image.
                        std::vector<std::string> shown;
                        for (const auto& name : names) shown.push_back(swizzle && name.size() > 1 && name[0] == 'e' ? name.substr(1) : name);
                        auto pick = kui::Dropdown(shown, at == names.end() ? -1 : static_cast<int>(at - names.begin()), [set, chosen, names, i](const int index) {
                            auto list = chosen;
                            list[i] = Value(names[static_cast<std::size_t>(index)]);
                            set(Value(std::move(list)));
                        });
                        parts.push_back(kui::Expanded(swizzle ? kgui::Labeled(channels[i], std::move(pick), 16.f) : std::move(pick)));
                    }
                    // A swizzle two channels a line: each choice wide enough to read.
                    if (swizzle && parts.size() == 4) {
                        std::vector<kui::Widget> second(std::make_move_iterator(parts.begin() + 2), std::make_move_iterator(parts.end()));
                        parts.resize(2);
                        return Rows({ kui::Row(std::move(parts), kui::FlexOptions {}.SetGap(8.f)), kui::Row(std::move(second), kui::FlexOptions {}.SetGap(8.f)) }, 4.f);
                    }
                    return kui::Row(std::move(parts), kui::FlexOptions {}.SetGap(4.f));
                }
                if (!type.element || (type.element->kind != Kind::eFloat && type.element->kind != Kind::eInt && type.element->kind != Kind::eUInt)) break;
                std::vector<float> numbers(type.length, 0.f);
                if (const auto* items = value.Items())
                    for (std::size_t i = 0; i < numbers.size() && i < items->size(); ++i) numbers[i] = NumberOf((*items)[i]);
                return DragNumbers(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); },
                                   limits, NumberTypeOf(type));
            }
            case Kind::eStruct: {
                // A colour an attachment is cleared to: picked, and given as the four floats it is.
                if (type.name == "KoralClearColor") {
                    kui::Color color(0.f, 0.f, 0.f, 1.f);
                    if (const Value* held = value.Find("value"))
                        if (const Value* floats = held->Find("f"); floats && floats->Items() && floats->Items()->size() == 4) {
                            const auto& c = *floats->Items();
                            color = kui::Color(NumberOf(c[0]), NumberOf(c[1]), NumberOf(c[2]), NumberOf(c[3]));
                        }
                    return kui::ColorEdit(color, [set](const kui::Color picked) {
                        set(Value(Value::Members { { "scalarType", "Float" }, { "components", 4 },
                                                   { "value", Value::Members { { "f", Value::List { picked.r, picked.g, picked.b, picked.a } } } } }));
                    });
                }
                std::vector<kui::Widget> rows;
                for (const auto& field : type.fields) {
                    const std::string name(field.name);
                    const Value* member = value.Find(name);
                    const auto setMember = [set, value, name](Value changed) {
                        Value::Members members = value.Named() ? *value.Named() : Value::Members {};
                        const auto known = std::ranges::find_if(members, [&](const auto& m) { return m.first == name; });
                        if (known != members.end()) known->second = std::move(changed);
                        else members.emplace_back(name, std::move(changed));
                        set(Value(std::move(members)));
                    };
                    rows.push_back(kgui::Labeled(name, TypeEditor(*field.type, member ? *member : Value {}, setMember, key + "." + name, kgraph::LimitsOf(type.name, field.name)), 110.f));
                }
                return Rows(std::move(rows), 4.f);
            }
            case Kind::eList: {
                if (!type.element) break;
                const Value::List items = value.Items() ? *value.Items() : Value::List {};
                std::vector<kui::Widget> rows;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const auto setItem = [set, items, i](Value changed) { auto list = items; list[i] = std::move(changed); set(Value(std::move(list))); };
                    rows.push_back(kui::Row({ kui::Expanded(TypeEditor(*type.element, items[i], setItem, std::format("{}[{}]", key, i))),
                                              kui::Tooltip("Take it out", SmallIcon("close", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); })) },
                                            kui::FlexOptions {}.SetGap(4.f)));
                }
                rows.push_back(Small("+ one more", [set, items, element = type.element] {
                    auto list = items;
                    list.push_back(element->kind == Kind::eStruct ? Value(Value::Members {}) : element->kind == Kind::eString ? Value(std::string()) : Value(0));
                    set(Value(std::move(list)));
                }));
                return Rows(std::move(rows), 4.f);
            }
            default: break;
            }
            return JsonField(value, set, key);
        }

        /**
         * An editor for an input of one of the engine's own nodes, which says only what its type is called; a number kept
         * within @p limits, of the kind @p number says.
         */
        kui::Widget NamedEditor(const std::string& type, const Value& value, const std::function<void(Value)>& set, const std::string& key,
                                const kgraph::NumberLimits& limits = {}, const kui::NumberType number = kui::NumberType::eFloat)
        {
            if (type == "number")
                return kui::DragValue(NumberOf(value), [set](const double v) { set(Value(v)); }, Dragging(limits, number));
            if (type == "bool") return kui::Checkbox(value.Bool() && *value.Bool(), [set](const bool on) { set(Value(on)); });
            if (type == "string") {
                const std::string text = value.String() ? *value.String() : std::string();
                return kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
            }
            return JsonField(value, set, key);
        }

        /** A dropdown of names, showing the one that is the value: and the value itself when it is none of them. */
        kui::Widget Choice(std::vector<std::string> names, const Value& value, const std::function<void(Value)>& set, std::string placeholder = {})
        {
            const std::string now = value.String() ? *value.String() : std::string();
            if (!now.empty() && std::ranges::find(names, now) == names.end()) names.push_back(now);
            const auto at = std::ranges::find(names, now);
            return kui::Dropdown(names, at == names.end() ? -1 : static_cast<int>(at - names.begin()),
                                 [set, names](const int index) { set(Value(names[static_cast<std::size_t>(index)])); },
                                 kui::DropdownOptions {}.SetPlaceholder(std::move(placeholder)));
        }

        kui::Widget TextEdit(const Value& value, const std::function<void(Value)>& set, const std::string& key, std::string placeholder = {})
        {
            const std::string text = value.String() ? *value.String() : std::string();
            return kui::TextField(kui::TextFieldOptions { .text = text, .placeholder = std::move(placeholder),
                                                          .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } }).Key(key + "=" + text);
        }

        /** A list of names to tick: the ones there are to choose from, and any the value has besides. */
        kui::Widget Ticks(std::vector<std::string> names, const Value& value, const std::function<void(Value)>& set)
        {
            std::vector<std::string> on;
            if (const auto* items = value.Items())
                for (const auto& item : *items) if (item.String()) on.push_back(*item.String());
            for (const auto& name : on) if (std::ranges::find(names, name) == names.end()) names.push_back(name);
            if (names.empty()) return kgui::Muted("there are none yet");
            std::vector<kui::Widget> boxes;
            for (const auto& name : names)
                boxes.push_back(kui::Checkbox(std::ranges::find(on, name) != on.end(), [set, on, name](const bool checked) {
                    auto changed = on;
                    if (checked) changed.push_back(name); else std::erase(changed, name);
                    set(Value(Value::List(changed.begin(), changed.end())));
                }, name));
            return kui::Column(std::move(boxes), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f));
        }

        // ---- a vertex layout, as a picture and as what it is made of ------------------------------------------

        /** One attribute of a vertex layout, as VertexLayout's `attributes` hold it. */
        struct LayoutAttribute {
            std::string semantic, type = "eFloat";
            int count = 3, buffer = 0;
            bool instanced = false;
        };
        std::vector<LayoutAttribute> AttributesOf(const Value& value)
        {
            std::vector<LayoutAttribute> out;
            if (const auto* items = value.Items())
                for (const auto& item : *items) {
                    LayoutAttribute attribute;
                    if (const Value* v = item.Find("semantic"); v && v->String()) attribute.semantic = *v->String();
                    if (const Value* v = item.Find("type"); v && v->String()) attribute.type = *v->String();
                    if (const Value* v = item.Find("count"); v && v->Number()) attribute.count = static_cast<int>(*v->Number());
                    if (const Value* v = item.Find("binding"); v && v->Number()) attribute.buffer = static_cast<int>(*v->Number());
                    if (const Value* v = item.Find("instanced"); v && v->Bool()) attribute.instanced = *v->Bool();
                    out.push_back(std::move(attribute));
                }
            return out;
        }
        Value ValueOf(const std::vector<LayoutAttribute>& attributes)
        {
            Value::List list;
            for (const auto& a : attributes) {
                Value::Members members { { "semantic", a.semantic }, { "type", a.type }, { "count", a.count } };
                if (a.buffer != 0) members.emplace_back("binding", a.buffer);
                if (a.instanced) members.emplace_back("instanced", true);
                list.emplace_back(std::move(members));
            }
            return Value(std::move(list));
        }
        std::size_t BytesOf(const LayoutAttribute& a)
        {
            static const std::map<std::string, std::size_t, std::less<>> sizes {
                { "eFloat", 4 }, { "eInt", 4 }, { "eUInt", 4 }, { "eShort", 2 }, { "eUShort", 2 }, { "eByte", 1 }, { "eUByte", 1 }, { "eDouble", 8 } };
            const auto it = sizes.find(a.type);
            return (it == sizes.end() ? 4 : it->second) * static_cast<std::size_t>(std::clamp(a.count, 1, 4));
        }
        /** An attribute as a shader would write its type: float3, ubyte4. */
        std::string ShortType(const LayoutAttribute& a)
        {
            std::string type = a.type.size() > 1 ? a.type.substr(1) : a.type;
            std::ranges::transform(type, type.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return a.count > 1 ? std::format("{}{}", type, a.count) : type;
        }
        /** The colour an attribute is drawn in: the same for the same semantic, wherever it is. */
        kui::Color SemanticColor(const std::string& semantic)
        {
            static const std::map<std::string, std::uint32_t, std::less<>> known {
                { "POSITION", 0x4C8DD6 }, { "NORMAL", 0x52B788 }, { "UV", 0xE0A23B }, { "TEXCOORD", 0xE0A23B }, { "COLOR", 0xC0637E },
                { "TANGENT", 0x8E6CD9 }, { "BITANGENT", 0x3FB8AF }, { "BONE_IDS", 0xD9703F }, { "BONE_WEIGHTS", 0xB08C5A } };
            std::string base = semantic;
            while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();     // UV1 is a UV
            if (const auto it = known.find(base); it != known.end()) return kui::Color::Hex(it->second);
            static const std::uint32_t palette[] { 0x6FA8DC, 0x93C47D, 0xF6B26B, 0xE06666, 0xB4A7D6, 0x76A5AF, 0xD5A6BD, 0x7A8B99 };
            std::size_t hash = 0;
            for (const char c : semantic) if (!std::isdigit(static_cast<unsigned char>(c))) hash = hash * 31 + static_cast<unsigned char>(c);
            return kui::Color::Hex(palette[hash % std::size(palette)]);
        }

        /**
         * What a vertex is made of, as a picture: a strip for each buffer the mesh's vertices are in, its attributes one
         * after another along it, each as wide as its bytes; and what each buffer is — how many bytes a vertex (or an
         * instance) takes of it.
         */
        kui::Widget LayoutPicture(const std::vector<LayoutAttribute>& attributes, const float perByte = 4.f)
        {
            std::map<int, std::vector<const LayoutAttribute*>> buffers;
            for (const auto& a : attributes) buffers[a.buffer].push_back(&a);
            if (buffers.empty()) return kgui::Muted("No attributes yet.", 11.f);
            std::vector<kui::Widget> rows;
            kui::TextStyle inside;
            inside.size = 10.f;
            inside.color = kui::Color::Hex(0x101010);
            for (const auto& [index, members] : buffers) {
                std::size_t stride = 0;
                bool instanced = false;
                for (const auto* a : members) { stride += BytesOf(*a); instanced = instanced || a->instanced; }
                kui::TextStyle header;
                header.size = 10.f;
                header.color = kui::Theme::Current().textMuted;
                rows.push_back(kui::Text(std::format("buffer {}  ·  {}  ·  {} bytes", index, instanced ? "per instance" : "per vertex", stride), header, kui::TextAlign::eStart, false));
                std::vector<kui::Widget> blocks;
                for (const auto* a : members) {
                    // As wide as its bytes, and never too narrow for its name.
                    const std::string label = a->semantic.empty() ? std::string("?") : a->semantic;
                    const float width = std::max(static_cast<float>(BytesOf(*a)) * perByte, static_cast<float>(label.size()) * 6.4f + 10.f);
                    blocks.push_back(kui::Tooltip(std::format("{}: {}, {} bytes", a->semantic.empty() ? "(no name)" : a->semantic, ShortType(*a), BytesOf(*a)),
                        kui::Container(kui::ContainerOptions { .width = width, .height = 20.f, .padding = kui::EdgeInsets::Symmetric(4.f, 0.f),
                                                               .decoration = { .color = SemanticColor(a->semantic), .radius = 3.f } },
                                       kui::Align(kui::Alignment::CenterLeft(), kui::Text(label, inside, kui::TextAlign::eStart, false)))));
                }
                rows.push_back(kui::Row(std::move(blocks), kui::FlexOptions {}.SetGap(2.f)));
            }
            return kui::Column(std::move(rows), kui::FlexOptions {}.SetGap(3.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart));
        }

        /**
         * What a vertex has, a buffer at a time: each buffer's attributes under it — what each is, of what, how many,
         * in which buffer, moved up and down — whether the buffer goes a vertex or an instance at a time, and the
         * picture of it all on top.
         */
        kui::Widget Attributes(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            static const std::vector<std::string> types { "eFloat", "eInt", "eUInt", "eShort", "eUShort", "eByte", "eUByte", "eDouble" };
            const auto attributes = AttributesOf(value);
            const auto change = [set, attributes](std::function<void(std::vector<LayoutAttribute>&)> how) {
                return [set, attributes, how = std::move(how)] { auto now = attributes; how(now); set(ValueOf(now)); };
            };
            std::set<int> buffers;
            for (const auto& a : attributes) buffers.insert(a.buffer);
            if (buffers.empty()) buffers.insert(0);
            std::vector<std::string> bufferNames;
            const int most = *buffers.rbegin() + 1;
            for (int b = 0; b <= most; ++b) bufferNames.push_back(b == most ? std::format("{} new", b) : std::to_string(b));

            std::vector<kui::Widget> rows { LayoutPicture(attributes, 5.f), kui::Separator() };
            for (const int buffer : buffers) {
                bool instanced = false;
                for (const auto& a : attributes) if (a.buffer == buffer && a.instanced) instanced = true;
                rows.push_back(kui::Row({
                    kui::Expanded(kgui::Muted(std::format("Buffer {}", buffer))),
                    kui::Tooltip("Its attributes go an instance at a time, not a vertex at a time: what each copy of a mesh drawn many times has of its own",
                                 kui::Checkbox(instanced, [set, attributes, buffer](const bool on) {
                                     auto now = attributes;
                                     for (auto& a : now) if (a.buffer == buffer) a.instanced = on;
                                     set(ValueOf(now));
                                 }, "per instance")),
                    Small("+ attribute", change([buffer](auto& now) {
                        bool instanced = false;
                        for (const auto& a : now) if (a.buffer == buffer && a.instanced) instanced = true;
                        now.push_back({ now.empty() ? "POSITION" : "", "eFloat", now.empty() ? 3 : 4, buffer, instanced });
                    })),
                }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                for (std::size_t i = 0; i < attributes.size(); ++i) {
                    if (attributes[i].buffer != buffer) continue;
                    const auto& a = attributes[i];
                    rows.push_back(kui::Row({
                        kui::SizedBox(4.f, 22.f, kui::Container(kui::ContainerOptions { .decoration = { .color = SemanticColor(a.semantic), .radius = 2.f } }, kui::SizedBox(4.f, 22.f))),
                        kui::Expanded(TextEdit(Value(a.semantic), [set, attributes, i](Value v) { auto now = attributes; now[i].semantic = v.String() ? *v.String() : std::string(); set(ValueOf(now)); },
                                               std::format("{}[{}]", key, i), "SEMANTIC")),
                        Choice(types, Value(a.type), [set, attributes, i](Value v) { auto now = attributes; if (v.String()) now[i].type = *v.String(); set(ValueOf(now)); }).Width(100.f),
                        kui::DragValue(a.count, [set, attributes, i](const double v) { auto now = attributes; now[i].count = static_cast<int>(v); set(ValueOf(now)); },
                                       kui::DragValueOptions {}.SetSpeed(0.1f).SetRange(1.f, 4.f).SetType(kui::NumberType::eUInt).SetWidth(44.f)),
                        kui::Tooltip("Which buffer it is in", kui::Dropdown(bufferNames, a.buffer, [set, attributes, i](const int b) {
                            auto now = attributes;
                            now[i].buffer = b;
                            // A buffer goes a vertex or an instance at a time: it takes the way of the one it joins.
                            bool joined = false, instanced = false;
                            for (std::size_t j = 0; j < now.size(); ++j) if (j != i && now[j].buffer == b) { joined = true; instanced = instanced || now[j].instanced; }
                            if (joined) now[i].instanced = instanced;
                            set(ValueOf(now));
                        }, kui::DropdownOptions {}.SetWidth(64.f))),
                        kui::Tooltip("Earlier", SmallIcon("arrow_upward", change([i](auto& now) { if (i > 0) std::swap(now[i], now[i - 1]); }))),
                        kui::Tooltip("Later", SmallIcon("arrow_downward", change([i](auto& now) { if (i + 1 < now.size()) std::swap(now[i], now[i + 1]); }))),
                        kui::Tooltip("Remove it", SmallIcon("close", change([i](auto& now) { now.erase(now.begin() + static_cast<std::ptrdiff_t>(i)); }))),
                    }, kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                }
            }
            rows.push_back(Small("+ buffer", change([most](auto& now) { now.push_back({ "", "eFloat", 4, most, false }); })));
            return Rows(std::move(rows), 4.f);
        }

        /** An element's fields, a line a field: its name, what it is, and how many of it — 1 for one, more for a fixed array. */
        kui::Widget Fields(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            static const std::vector<std::string> types { "eFloat", "eInt", "eUInt", "eBool", "eVec2", "eVec3", "eVec4", "eIVec2", "eIVec3", "eIVec4",
                                                          "eUVec2", "eUVec3", "eUVec4", "eMat3", "eMat4" };
            const Value::List items = value.Items() ? *value.Items() : Value::List {};
            std::vector<kui::Widget> rows;
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto with = [set, items, i](const std::string& name, Value changed) {
                    auto list = items;
                    Value::Members members = list[i].Named() ? *list[i].Named() : Value::Members {};
                    const auto known = std::ranges::find_if(members, [&](const auto& m) { return m.first == name; });
                    if (known != members.end()) known->second = std::move(changed); else members.emplace_back(name, std::move(changed));
                    list[i] = Value(std::move(members));
                    set(Value(std::move(list)));
                };
                const Value* name = items[i].Find("name");
                const Value* type = items[i].Find("type");
                const Value* count = items[i].Find("count");
                rows.push_back(kui::Row({
                    kui::Expanded(TextEdit(name ? *name : Value {}, [with](Value v) { with("name", std::move(v)); }, std::format("{}[{}]", key, i), "position")),
                    Choice(types, type ? *type : Value("eFloat"), [with](Value v) { with("type", std::move(v)); }).Width(96.f),
                    kui::DragValue(count ? NumberOf(*count) : 1.f, [with](const double v) { with("count", Value(v)); },
                                   kui::DragValueOptions {}.SetSpeed(1.f).SetRange(1.f, 256.f).SetType(kui::NumberType::eUInt).SetWidth(48.f)),
                    kui::Tooltip("Take it out", SmallIcon("close", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); })),
                }, kui::FlexOptions {}.SetGap(4.f)));
            }
            rows.push_back(Small("+ field", [set, items] {
                auto list = items;
                list.emplace_back(Value::Members { { "name", std::format("field{}", list.size()) }, { "type", "eFloat" } });
                set(Value(std::move(list)));
            }));
            return Rows(std::move(rows), 4.f);
        }

        /** A Bundle node's members' names, a line each: each an input of the node, to wire or to give. */
        kui::Widget Names(const Value& value, const std::function<void(Value)>& set, const std::string& key)
        {
            const Value::List items = value.Items() ? *value.Items() : Value::List {};
            std::vector<kui::Widget> rows;
            for (std::size_t i = 0; i < items.size(); ++i)
                rows.push_back(kui::Row({
                    kui::Expanded(TextEdit(items[i], [set, items, i](Value v) { auto list = items; list[i] = std::move(v); set(Value(std::move(list))); },
                                           std::format("{}[{}]", key, i), "viewProjection")),
                    kui::Tooltip("Take it out", SmallIcon("close", [set, items, i] { auto list = items; list.erase(list.begin() + static_cast<std::ptrdiff_t>(i)); set(Value(std::move(list))); })),
                }, kui::FlexOptions {}.SetGap(4.f)));
            rows.push_back(Small("+ member", [set, items] {
                auto list = items;
                list.emplace_back(std::format("member{}", list.size()));
                set(Value(std::move(list)));
            }));
            return Rows(std::move(rows), 4.f);
        }

        /**
         * What a field that holds another entity is shown as: which of the scene's it is — each under what it is under —
         * or none; and an entity dragged here from the outliner is the one it holds from then on.
         */
        kui::Widget EntityPicker(const kecs::World& world, const Value& value, std::function<void(Value)> set)
        {
            std::vector<kecs::Entity> choices { kecs::NullEntity };
            std::vector<std::string> labels { "(nothing)" };
            const std::function<void(kecs::Entity, int)> add = [&](const kecs::Entity entity, const int depth) {
                choices.push_back(entity);
                labels.push_back(std::string(static_cast<std::size_t>(depth) * 3, ' ') + world.NameOf(entity));
                for (const kecs::Entity child : world.ChildrenOf(entity)) add(child, depth + 1);
            };
            for (const kecs::Entity root : world.Roots()) add(root, 0);
            const kecs::Entity held = EntityOfValue(value).value_or(kecs::NullEntity);
            int selected = 0;
            if (const auto at = std::ranges::find(choices, held); at != choices.end()) selected = static_cast<int>(at - choices.begin());
            else if (!held.IsNull()) { selected = static_cast<int>(choices.size()); choices.push_back(held); labels.push_back("(an entity that is gone)"); }
            kui::Widget picker = kui::Dropdown(std::move(labels), selected, [set, choices](const int index) {
                set(EntityValue(choices[static_cast<std::size_t>(std::max(index, 0))]));
            });
            return kui::Tooltip("An entity of the scene: chosen here, or dragged here from the scene tree.",
                kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([set](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* entity = dragged.As<kecs::Entity>()) set(EntityValue(*entity));
                }), std::move(picker)));
        }

        /** What an output of a kind is called on a node: a vertex layout for `struct KoralVertexLayout`, and other types as they are. */
        std::string TypeLabel(const std::string& type)
        {
            if (!type.starts_with("struct Koral")) return type;
            std::string words;
            for (const char c : type.substr(12)) {
                if (std::isupper(static_cast<unsigned char>(c)) && !words.empty()) words += ' ';
                words += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return words;
        }

        /** What a port is called on a node: its name, without what kind of input it is — `position` for `field.position`. */
        std::string PortLabel(const std::string& name)
        {
            for (const std::string_view kind : { "field.", "param.", "push.", "constant." })
                if (name.starts_with(kind) && name.size() > kind.size()) return name.substr(kind.size());
            return name;
        }

        /** An input of a node, as it stands. */
        struct Shown {
            kgraph::Port port;      // its name is the input's: `color[1].view`, not `color[].view`
            bool given = false;     // the node has it: a value, a wire, or a port left open for one
        };

        /**
         * What an input of a view of an image is kept within, by the image it is of: its first level and how many from
         * there, its first layer and how many from there — within what that image has. A view's own node, or one of a
         * Script node's views (`HeightView.baseMipLevel`); the image as the view was built of it, or as it is wired or named.
         */
        void LimitByImage(const kgraph::Runtime& runtime, const kgraph::Node& node, kgraph::Port& port)
        {
            std::string view, input = port.name;
            if (node.type == "Script") {
                const auto dot = port.name.find('.');
                if (dot == std::string::npos) return;
                view = port.name.substr(0, dot);
                input = port.name.substr(dot + 1);
            } else if (node.type != "ImageView") {
                return;
            }
            if (input != "baseMipLevel" && input != "mipLevelCount" && input != "baseArrayLayer" && input != "arrayLayerCount") return;
            const std::string prefix = view.empty() ? std::string() : view + ".";
            kor::ResourceRef<const kor::Image> image;
            if (const auto held = runtime.ResourceOf(node.id, view.empty() ? "out" : view); held && held->kind == "ImageView" && held->source && held->source->handle)
                image = kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(held->source->handle));
            if (!image.Get())
                if (const auto wired = node.inputs.find(prefix + "image"); wired != node.inputs.end() && wired->second.link)
                    image = runtime.ImageOf(wired->second.link->node, wired->second.link->output);
            if (!image.Get() && !view.empty())
                for (const auto& script : runtime.ResourceScripts())
                    for (const auto& resource : script.resources)
                        if (resource.name == view && resource.kind == "ImageView")
                            for (const auto& [setting, value] : resource.settings)
                                if (setting == "image" && value.String()) image = runtime.ImageOf(node.id, *value.String());
            if (!image.Get()) return;
            const auto given = [&](const std::string& name) {
                const auto it = node.inputs.find(prefix + name);
                return it != node.inputs.end() && it->second.value.Number() ? static_cast<float>(*it->second.value.Number()) : 0.f;
            };
            const float levels = static_cast<float>(std::max<kor::u32>(image->MipLevels(), 1));
            const float layers = static_cast<float>(std::max<kor::u32>(image->ArrayLayers(), 1));
            if (input == "baseMipLevel") { port.min = 0.f; port.max = levels - 1.f; }
            else if (input == "mipLevelCount") { port.min = 1.f; port.max = std::max(levels - std::clamp(given("baseMipLevel"), 0.f, levels - 1.f), 1.f); }
            else if (input == "baseArrayLayer") { port.min = 0.f; port.max = layers - 1.f; }
            else { port.min = 1.f; port.max = std::max(layers - std::clamp(given("baseArrayLayer"), 0.f, layers - 1.f), 1.f); }
        }

        /**
         * A node's inputs as they stand. Those it cannot do without, those it has been given, and those asked for
         * (@p asked): in the order its type lists them. One given more than once is there once for each it has, and
         * once more when it is one a node needs. The rest are in @p more, for whoever wants one.
         */
        std::vector<Shown> InputsOf(const kgraph::Runtime& runtime, const kgraph::Node& node, const std::function<bool(const std::string&)>& asked,
                                    std::vector<kgraph::Port>* more = nullptr)
        {
            std::vector<Shown> shown;
            std::set<std::string> listed;
            const auto consider = [&](kgraph::Port port, bool always) {
                LimitByImage(runtime, node, port);
                const bool given = node.inputs.contains(port.name);
                // One that does nothing as the node is set — an indirect offset of a draw of vertices — is not there until it does, or is given.
                if (!given && !kgraph::Applies(port, node)) { listed.insert(port.name); if (more) more->push_back(std::move(port)); return; }
                // One set on the node is there, given or not: its control is what it is.
                if (kgraph::PlacementOf(port, node) == kgraph::Placement::eNode) always = true;
                listed.insert(port.name);
                if (given || always || !port.optional || asked(port.name)) shown.push_back({ std::move(port), given });
                else if (more) more->push_back(std::move(port));
            };
            const kgraph::NodeType* type = kgraph::FindType(node.type);
            // An input given whole — `groups`, wired from a vector — is not also there in its parts.
            const auto givenWhole = [&](const std::string& name) {
                if (!type || name.size() < 3 || name[name.size() - 2] != '.') return false;
                const std::string whole = name.substr(0, name.size() - 2);
                return node.inputs.contains(whole) && std::ranges::contains(kgraph::PartsOf(*type, whole), name);
            };
            for (const auto& port : runtime.PortsOf(node.id)) {
                if (givenWhole(port.name)) { listed.insert(port.name); continue; }
                // An Image or a Buffer is made for what uses it: its usage is not the inspector's to tick.
                if ((node.type == "Image" || node.type == "Buffer") && port.name == "usage") { listed.insert(port.name); continue; }
                const auto at = port.name.find("[]");
                // What a shader declares is the node's to be given, whatever else is: a pipeline's parameter, or a
                // field of a ShaderBuffer's struct, is never hidden.
                if (at == std::string::npos) { consider(port, port.name.starts_with("param.") || port.name.starts_with("field.")); continue; }
                const std::string prefix = port.name.substr(0, at + 1);
                int count = 0;
                for (const auto& name : node.inputs | std::views::keys)
                    if (name.starts_with(prefix)) count = std::max(count, std::atoi(name.c_str() + prefix.size()) + 1);
                for (int i = 0; i <= count; ++i) {
                    kgraph::Port each = port;
                    each.name = std::format("{}{}]{}", prefix, i, port.name.substr(at + 2));
                    each.repeatable = false;
                    // The next one free is offered like any input that can be done without.
                    if (i == count) each.optional = port.optional || count > 0;
                    consider(std::move(each), false);
                }
            }
            // And what the node has that its type does not list: a shader wired in, an input of another way of drawing.
            for (const auto& [name, input] : node.inputs)
                if (!listed.contains(name))
                    shown.push_back({ kgraph::Port { name, input.link ? kgraph::PortKind::eResource : kgraph::PortKind::eValue, "value", false, {}, nullptr }, true });
            return shown;
        }

        /** Whether an input shows as a port on its node: what takes a resource, and a value that is wired or left open for a wire. */
        bool ShowsAsPort(const Shown& shown, const kgraph::Node& node)
        {
            if (shown.port.kind == kgraph::PortKind::eResource) return true;
            // A field of a shader's struct is there to be wired to, from an entity's: a port from the start.
            if ((node.type == "ShaderBuffer" || node.type == "BufferBuilder") && shown.port.name.starts_with("field.")) return true;
            // A component's buffer fills its fields from the entities: one is a port only once something else is wired to it.
            if (node.type == "ComponentBuffer" && shown.port.name.starts_with("field.")) {
                const auto input = node.inputs.find(shown.port.name);
                return input != node.inputs.end() && input->second.link;
            }
            // One set on the node is a port with its control; one wired only, a port to wire.
            if (kgraph::PlacementOf(shown.port, node) != kgraph::Placement::eInspector) return true;
            const auto input = node.inputs.find(shown.port.name);
            return input != node.inputs.end() && (input->second.link || input->second.value.IsNull());
        }

        /** What a port carries, as the graph's view tells kinds apart: `r:Image`, `v:number`. */
        std::string WireType(const kgraph::Port& port) { return (port.kind == kgraph::PortKind::eResource ? "r:" : "v:") + port.type; }

        /** Whether a wire may go from an output of kind @p from to an input of kind @p to: a resource to what takes one of its kind (a view of an image where an image goes), a value to a value. */
        bool WireFits(const std::string_view from, const std::string_view to)
        {
            // A loop's instances fill a buffer of each instance's: a Draw's (a TraceRays', a Dispatch's) buffer, and nothing else.
            if (from == "r:Instances" || to == "r:Instances") return from == "r:Instances" && (to == "r:Buffer" || to == "r:Resource");
            if (from.substr(0, 2) != to.substr(0, 2)) return false;
            // A Bundle goes where a Bundle is taken — or a function's edge, which takes any value — and nothing else takes one.
            if ((from == "v:bundle") != (to == "v:bundle")) return from == "v:value" || to == "v:value";
            if (from.starts_with("v:")) return true;
            return from == to || from == "r:Resource" || to == "r:Resource" || (from == "r:ImageView" && to == "r:Image");
        }

        using Timings = std::map<std::string, kgraph::Runtime::Timing, std::less<>>;

        /** The GPU's time of every pass of a frame. */
        double FrameGpuMs(const Timings& timings)
        {
            double total = 0.0;
            for (const auto& timing : timings | std::views::values) total += timing.gpuMs;
            return total;
        }

        /** How hot a share of the frame is, by how bright it is said: faint for what hardly counts, the text's own for the most. */
        kui::Color HeatColor(const double share)
        {
            const kui::Theme& t = kui::Theme::Current();
            if (share >= 0.15) return t.text;
            if (share >= 0.05) return t.text.WithAlpha(0.78f);
            return t.textMuted;
        }

        /** A cost said in a few words: its GPU time and its share of the frame's; the CPU's recording, where the GPU can't be timed. */
        std::string CostText(const kgraph::Runtime::Timing& cost, const double frameMs)
        {
            if (!cost.gpuMeasured) return std::format("record {:.2f} ms", cost.recordMs);
            const double share = frameMs > 0.0 ? cost.gpuMs / frameMs : 0.0;
            return std::format("GPU {:.2f} ms  ·  {:.0f}%", cost.gpuMs, share * 100.0);
        }

        /** A port's colour, and its wires': by what it carries, so that an image's wire is told from a buffer's at a glance. */
        kui::Color PortColor(const std::string_view kind)
        {
            const std::string_view type = kind.size() > 2 ? kind.substr(2) : kind;
            if (kind.starts_with("r:")) {
                if (type == "Image" || type == "ImageView") return kui::Color::Hex(0x3FB8AF);
                if (type == "Buffer" || type == "BufferView") return kui::Color::Hex(0xD9A441);
                // A loop's instances: a buffer filled as best it can be from what its entities have, told from any other at a glance.
                if (type == "Instances") return kui::Color::Hex(0xFF4F7B);
                if (type == "Sampler") return kui::Color::Hex(0xA77BD8);
                if (type == "Mesh") return kui::Color::Hex(0x7DBB4B);
                if (type == "Shader") return kui::Color::Hex(0xC76B98);
                if (type.ends_with("Pipeline")) return kui::Color::Hex(0xD9703F);
                return kui::Color::Hex(0x9AA4B0);
            }
            if (type == "bool" || type == "Bool") return kui::Color::Hex(0xD86A6A);
            if (type == "bundle") return kui::Color::Hex(0xD46FA8);
            if (type == "number" || type == "Float" || type == "Double" || type == "Int" || type == "UInt") return kui::Color::Hex(0x5B9BD5);
            if (type == "string" || type == "String") return kui::Color::Hex(0xC9A0DC);
            if (type == "vector" || type == "Color") return kui::Color::Hex(0x62C3E8);
            return kui::Color::Hex(0x8FA3B8);
        }

        /** A port's mark, by what it carries: an image's a diamond, a buffer's a square, anything else's a circle. */
        kui::PortShape PortShapeOf(const std::string_view kind)
        {
            if (!kind.starts_with("r:")) return kui::PortShape::eCircle;
            const std::string_view type = kind.substr(2);
            if (type == "Image" || type == "ImageView") return kui::PortShape::eDiamond;
            if (type == "Buffer" || type == "BufferView") return kui::PortShape::eSquare;
            return kui::PortShape::eCircle;
        }

        /** A value in a few characters, to be said on a node. */
        std::string Brief(const kgraph::Value& value, const std::size_t most = 28)
        {
            std::string text;
            if (const bool* flag = value.Bool()) text = *flag ? "yes" : "no";
            else if (const double* number = value.Number()) text = *number == std::floor(*number) && std::abs(*number) < 1e9 ? std::format("{}", static_cast<long long>(*number)) : std::format("{:.3g}", *number);
            else if (const std::string* string = value.String()) text = *string;
            else if (const auto* items = value.Items()) {
                for (const auto& item : *items) text += (text.empty() ? "" : ", ") + Brief(item, most);
            } else if (!value.IsNull()) text = "...";
            // A file is its name, not where it is.
            if (text.size() > most && text.find('/') != std::string::npos) text = text.substr(text.rfind('/') + 1);
            if (text.size() > most) text = text.substr(0, most - 3) + "...";
            return text;
        }

        kui::Color CategoryColor(const std::string& category)
        {
            // A colour a kind: what is made on the device warm, what is worked out cool, what the scene holds green.
            static const std::map<std::string, std::uint32_t, std::less<>> colors {
                { "Pipelines", 0x7A4BC2 }, { "Passes", 0xC2643B }, { "Resources", 0xB0892F }, { "Values", 0x3B7FC2 }, { "Math", 0x2F8FA8 },
                { "Vectors", 0x4C6FCF }, { "Random", 0x3F9C8F }, { "Logic", 0x8A5BA8 }, { "Cameras", 0x5A8F3A }, { "Scene", 0x3B9C7A }, { "Components", 0x4E9E4E }, { "Flow", 0x6B7685 },
                { "Functions", 0x3E9FA8 } };
            const auto it = colors.find(category);
            return kui::Color::Hex(it == colors.end() ? 0x3E9FA8 : it->second);   // none of these: what a function is for
        }

        // ---- a node's picture ----------------------------------------------------------------------------------------

        /**
         * A node's picture on its card — its thumbnail, or as it is looked at closely — built again on its own when a new
         * one is made: a picture every few frames costs that much, not the whole interface built again.
         */
        class NodePicture final : public kui::StatefulWidget {
        public:
            /// A node's picture, found again each time it is made anew: one held on to would be gone a few frames after the next is made.
            /// As big as @p size; framed as a node's card has it, or bare, as a tile's.
            NodePicture(std::shared_ptr<Session> session, std::string id, const bool looking, const kor::Vec2 size = { 236.f, 146.f }, const bool framed = true)
                : _session(std::move(session)), _id(std::move(id)), _looking(looking), _size(size), _framed(framed) {}

            void InitState() override
            {
                _seen = _session->PictureVersionOf(_id, _looking);
                Animate([this](float) {
                    if (const auto version = _session->PictureVersionOf(_id, _looking); version != _seen) SetState([&] { _seen = version; });
                    return true;
                });
            }
            void DidUpdateWidget(const kui::StatefulWidget& newer) override
            {
                const auto& given = static_cast<const NodePicture&>(newer);
                _id = given._id;
                _looking = given._looking;
                _size = given._size;
                _framed = given._framed;
                _seen = _session->PictureVersionOf(_id, _looking);
            }
            kui::Widget Build() override
            {
                const auto picture = _looking ? _session->LookPictureOf(_id) : _session->ThumbnailOf(_id);
                kui::Widget image = kui::Image(picture, kui::ImageFit::eContain, { _size.x, _size.y }, _looking ? _session->lookFilter : kui::ImageFilter::eLinear);
                if (!_framed) return image;
                return kui::Container(kui::ContainerOptions { .padding = kui::EdgeInsets::All(2.f), .decoration = { .color = kui::Theme::Current().background, .radius = 4.f } },
                                      std::move(image));
            }

        private:
            std::shared_ptr<Session> _session;
            std::string _id;
            bool _looking = false;
            kor::Vec2 _size;
            bool _framed = true;
            std::uint64_t _seen = 0;
        };


        /**
         * One of the window's own buttons in the title bar — smaller, bigger or back, close — shaped as the buttons down the
         * dock's sides are: a square of their size with their corners, lit under the pointer; close, red.
         */
        class WindowButton final : public kui::StatefulWidget {
        public:
            enum class Does { eMinimize, eMaximize, eClose };
            WindowButton(const kor::Window* window, const Does does) : _window(window), _does(does) {}

            void DidUpdateWidget(const kui::StatefulWidget& newer) override
            {
                const auto& given = static_cast<const WindowButton&>(newer);
                _window = given._window;
                _does = given._does;
            }
            kui::Widget Build() override
            {
                const kui::Theme& t = kui::Theme::Current();
                const bool close = _does == Does::eClose;
                const kui::Color fill = !_over ? t.surface : close ? kui::Color::Hex(0xE81123) : _down ? t.surfacePressed : t.surfaceHover;
                const char* icon = _does == Does::eMinimize ? "remove" : close ? "close" : _window && _window->IsMaximized() ? "filter_none" : "crop_square";
                kui::Widget face = kui::Container(kui::ContainerOptions {}.SetSize(30.f, 30.f).SetAlignment(kui::Alignment::Center())
                                                      .SetDecoration(kui::Decoration {}.SetColor(fill).SetRadius(8.f)),
                                                  kui::SizedBox(16.f, 16.f, kui::Icon(icon, kui::IconStyle::eFilled, close && _over ? kui::colors::White : t.text)));
                return kui::GestureDetector(kui::GestureOptions {}
                    .OnEnter([this] { SetState([&] { _over = true; }); })
                    .OnExit([this] { SetState([&] { _over = false; _down = false; }); })
                    .OnTapDown([this](kor::Vec2) { SetState([&] { _down = true; }); })
                    .OnTapUp([this] { SetState([&] { _down = false; }); })
                    .OnTap([this] {
                        if (!_window) return;
                        if (_does == Does::eMinimize) _window->Minimize();
                        else if (_does == Does::eMaximize) _window->ToggleMaximize();
                        else _window->RequestClose();
                    }), std::move(face));
            }

        private:
            const kor::Window* _window;
            Does _does;
            bool _over = false, _down = false;
        };

        /** How many frames a second, built again on its own as it changes: four times a second, it was the whole interface. */
        class FrameRate final : public kui::StatefulWidget {
        public:
            explicit FrameRate(std::shared_ptr<Session> session) : _session(std::move(session)) {}

            void InitState() override
            {
                _shown = _session->Fps();
                Animate([this](float) {
                    if (const int now = _session->Fps(); now != _shown) SetState([&] { _shown = now; });
                    return true;
                });
            }
            kui::Widget Build() override
            {
                return kui::Padding(kui::EdgeInsets::Symmetric(4.f, 0.f), kui::Text(std::format("{} fps", _shown), kui::TextStyle {}.SetColor(kui::Color::Hex(0xE0E0E0)).SetSize(12.f)));
            }

        private:
            std::shared_ptr<Session> _session;
            int _shown = 0;
        };

        /** A line or a few of text that follows what @p text says, asked a few times a second: built again only itself, when it changes. */
        class LiveText final : public kui::StatefulWidget {
        public:
            LiveText(std::function<std::string()> text, std::string tip = {}, const int perSecond = 10)
                : _text(std::move(text)), _tip(std::move(tip)), _every(1.f / static_cast<float>(std::max(perSecond, 1))) {}

            void InitState() override
            {
                _shown = _text();
                Animate([this](const float dt) {
                    _since += dt;
                    if (_since < _every) return true;
                    _since = 0.f;
                    if (std::string now = _text(); now != _shown) SetState([&] { _shown = std::move(now); });
                    return true;
                });
            }
            void DidUpdateWidget(const kui::StatefulWidget& newer) override
            {
                const auto& given = static_cast<const LiveText&>(newer);
                _text = given._text;
                _tip = given._tip;
                _every = given._every;
                _shown = _text();
            }
            kui::Widget Build() override
            {
                std::vector<kui::Widget> lines;
                const kui::TextStyle style = kui::TextStyle {}.SetColor(kui::Color::Hex(0xE0E0E0)).SetSize(12.f);
                for (std::size_t at = 0; at <= _shown.size();) {
                    const auto end = std::min(_shown.find('\n', at), _shown.size());
                    lines.push_back(kui::Text(_shown.substr(at, end - at), style));
                    at = end + 1;
                }
                kui::Widget body = kui::Padding(kui::EdgeInsets::Symmetric(4.f, 0.f), kui::Column(std::move(lines), kui::FlexOptions {}.SetGap(2.f).SetMainAxisSize(kui::MainAxisSize::eMin)
                                                                                                                    .SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)));
                return _tip.empty() ? body : kui::Tooltip(_tip, std::move(body));
            }

        private:
            std::function<std::string()> _text;
            std::string _tip, _shown;
            float _every = 0.1f, _since = 0.f;
        };

        /**
         * Which way the view looks, as the world's axes are seen from it: each end of each a disc — X red, Y green, Z
         * blue, the far ones faint — that, pressed, looks along it straight on; the middle, perspective or straight on.
         * Built again by itself as the view turns.
         */
        class ViewCube final : public kui::StatefulWidget {
        public:
            explicit ViewCube(std::shared_ptr<Session> session, const bool second = false) : _session(std::move(session)), _second(second) {}

            void InitState() override
            {
                _seen = Seen();
                Animate([this](float) {
                    if (const kor::Vec3 now = Seen(); kor::Length(now - _seen) > 0.25f) SetState([&] { _seen = now; });
                    return true;
                });
            }
            kui::Widget Build() override
            {
                constexpr float Size = 84.f, Reach = 30.f, Disc = 18.f;
                constexpr float Radians = 3.14159265358979323846f / 180.f;
                const float yaw = _seen.x * Radians, pitch = _seen.y * Radians;
                // The view's own right, up and forward, as EditorCamera::Position has it looking at its target.
                const kor::Vec3 back(std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw));
                const kor::Vec3 right(std::cos(yaw), 0.f, -std::sin(yaw));
                const kor::Vec3 up = kor::Cross(back, right);
                using Axis = Session::Axis;
                struct End { kor::Vec3 way; const char* label; std::uint32_t colour; Axis axis; };
                std::vector<End> ends {
                    { { 1.f, 0.f, 0.f }, "X", 0xE0605A, Axis::eRight }, { { -1.f, 0.f, 0.f }, "", 0xE0605A, Axis::eLeft },
                    { { 0.f, 1.f, 0.f }, "Y", 0x7DBB4B, Axis::eTop }, { { 0.f, -1.f, 0.f }, "", 0x7DBB4B, Axis::eBottom },
                    { { 0.f, 0.f, 1.f }, "Z", 0x5B9BD5, Axis::eFront }, { { 0.f, 0.f, -1.f }, "", 0x5B9BD5, Axis::eBack },
                };
                // The far ones first: the near ones over them.
                std::ranges::sort(ends, [&](const End& a, const End& b) { return kor::Dot(a.way, back) < kor::Dot(b.way, back); });
                std::vector<kui::Widget> layers;
                layers.push_back(kui::Container(kui::ContainerOptions {}.SetSize(Size, Size)
                                                    .SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x101012).WithAlpha(0.55f)).SetRadius(Size * 0.5f))));
                for (const End& end : ends) {
                    const float x = Size * 0.5f + kor::Dot(end.way, right) * Reach - Disc * 0.5f, y = Size * 0.5f - kor::Dot(end.way, up) * Reach - Disc * 0.5f;
                    const bool near = kor::Dot(end.way, back) >= -0.05f;
                    const kui::Color colour = kui::Color::Hex(end.colour).WithAlpha(near ? 1.f : 0.45f);
                    kui::Widget disc = kui::Container(kui::ContainerOptions {}.SetSize(Disc, Disc).SetAlignment(kui::Alignment::Center())
                                                          .SetDecoration(kui::Decoration {}.SetColor(*end.label ? colour : colour.WithAlpha(near ? 0.55f : 0.3f)).SetRadius(Disc * 0.5f)),
                                                      kui::Text(end.label, kui::TextStyle {}.SetSize(11.f).SetColor(kui::Color::Hex(0x101012))));
                    const char* names[] { "above", "below", "the front (+Z)", "the back (-Z)", "the right (+X)", "the left (-X)" };
                    layers.push_back(kui::Positioned(kui::PositionedOptions { .left = x, .top = y },
                        kui::Tooltip(std::format("Look from {}, straight on", names[static_cast<int>(end.axis)]),
                                     kui::Button(std::move(disc), [this, axis = end.axis] { _session->LookAlong(axis, false, _second ? &_session->secondCamera : nullptr); },
                                                 kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)))));
                }
                // The middle: perspective, or straight on.
                const bool straight = _seen.z > 0.5f;
                layers.push_back(kui::Positioned(kui::PositionedOptions { .left = Size * 0.5f - 12.f, .top = Size * 0.5f - 12.f },
                    kui::Tooltip(straight ? "Seen straight on: press for perspective" : "In perspective: press to see it straight on",
                                 kui::Button(kui::Container(kui::ContainerOptions {}.SetSize(24.f, 24.f).SetAlignment(kui::Alignment::Center())
                                                                .SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x2A2A2E).WithAlpha(0.9f)).SetRadius(12.f)),
                                                            kui::Text(straight ? "ORT" : "PER", kui::TextStyle {}.SetSize(8.f).SetColor(kui::Color::Hex(0xE0E0E0)))),
                                             [this, straight] { View().orthographic = !straight; _session->SetOrthographic(_session->camera.orthographic); },
                                             kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)))));
                return kui::SizedBox(Size, Size, kui::Stack(std::move(layers)));
            }

        private:
            EditorCamera& View() { return _second ? _session->secondCamera : _session->camera; }
            /** Its yaw, its pitch, and whether it is straight on: what it is built again for. */
            kor::Vec3 Seen() { const EditorCamera& view = View(); return { view.yaw, view.pitch, view.orthographic ? 1.f : 0.f }; }

            std::shared_ptr<Session> _session;
            bool _second = false;
            kor::Vec3 _seen { 0.f };
        };

        /**
         * The picked entity's inspector while the scene plays, built again on its own when what it is made of changes —
         * a component given or taken, switched, renamed; ten times a second at most. What its fields hold as scripts and
         * physics change them is each field's own to follow (LiveValue). It was the whole interface that was built again:
         * with thousands of bodies moving, every frame, and a frame took 30 times as long as with nothing picked.
         */
        class Following final : public kui::StatefulWidget {
        public:
            Following(std::shared_ptr<Session> session, const kecs::Entity entity, std::function<kui::Widget()> build)
                : _session(std::move(session)), _entity(entity), _build(std::move(build)) {}

            void InitState() override
            {
                _seen = Changes();
                Animate([this](float) {
                    if (!_session->IsPlaying()) return true;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - _at < std::chrono::milliseconds(100)) return true;
                    if (const std::uint64_t changes = Changes(); changes != _seen) SetState([&] { _seen = changes; _at = now; });
                    return true;
                });
            }
            void DidUpdateWidget(const kui::StatefulWidget& newer) override
            {
                const auto& given = static_cast<const Following&>(newer);
                _entity = given._entity;
                _build = given._build;
                _seen = Changes();
            }
            kui::Widget Build() override { return _build(); }

        private:
            /** What the inspector is made of has changed when this has: which entities there are, what each has and is called. */
            [[nodiscard]] std::uint64_t Changes() const
            {
                const kecs::World& world = _session->Doc().World();
                return world.Alive(_entity) ? world.StructureVersion() : 0;
            }

            std::shared_ptr<Session> _session;
            kecs::Entity _entity;
            std::function<kui::Widget()> _build;
            std::uint64_t _seen = 0;
            std::chrono::steady_clock::time_point _at {};
        };

        /**
         * One value an inspector shows — a field, where an entity is — followed as it changes: read ten times a second at
         * most, and its control alone built again when it has changed. A body physics moves changes its position and
         * velocity every frame, and an entity dragged with the gizmo its own; this is all that is built again for it.
         */
        class LiveValue final : public kui::StatefulWidget {
        public:
            LiveValue(std::shared_ptr<Session> session, std::function<Value()> read, std::function<kui::Widget(const Value&)> build)
                : _session(std::move(session)), _read(std::move(read)), _build(std::move(build)) {}

            void InitState() override
            {
                _shown = _read();
                Animate([this](float) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - _at < std::chrono::milliseconds(100)) return true;
                    _at = now;
                    if (Value value = _read(); value != _shown) SetState([&] { _shown = std::move(value); });
                    return true;
                });
            }
            void DidUpdateWidget(const kui::StatefulWidget& newer) override
            {
                const auto& given = static_cast<const LiveValue&>(newer);
                _read = given._read;
                _build = given._build;
                _shown = _read();
            }
            kui::Widget Build() override { return _build(_shown); }

        private:
            std::shared_ptr<Session> _session;
            std::function<Value()> _read;
            std::function<kui::Widget(const Value&)> _build;
            Value _shown;
            std::chrono::steady_clock::time_point _at {};
        };

        // ---- the root -------------------------------------------------------------------------------------------

        /**
         * The log: what the engine, the framework and the editor have said, and what has gone wrong and been mended, oldest
         * at the top and coloured by how serious it was. It follows what is logged by itself, ten times a second at most,
         * and is all that is built again for it. Errors, warnings and the rest are each ticked to be shown, and what is
         * typed keeps the lines that have it. A line of a node goes to it, pressed; right-clicked, a line is copied.
         */
        class LogPanel final : public kui::StatefulWidget {
        public:
            explicit LogPanel(std::shared_ptr<Session> session) : _session(std::move(session)) {}

            void InitState() override
            {
                Pull();
                Animate([this](float) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - _at < std::chrono::milliseconds(100) || kor::log::LastSequence() == _seen) return true;
                    _at = now;
                    SetState([&] { Pull(); });
                    return true;
                });
            }

            kui::Widget Build() override
            {
                using Level = kor::log::Level;
                const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                const std::string wanted = lower(_filter);
                std::size_t counts[3] {};
                _shownLines.clear();
                for (std::size_t i = 0; i < _log.size(); ++i) {
                    const auto level = static_cast<std::size_t>(_log[i].level);
                    ++counts[level];
                    if (_levels & (1u << level) && (wanted.empty() || lower(_log[i].message).find(wanted) != std::string::npos)) _shownLines.push_back(i);
                }
                const auto levelBox = [this](const Level level, const char* label, const std::size_t n) {
                    const unsigned bit = 1u << static_cast<unsigned>(level);
                    return kui::Checkbox(_levels & bit, [this, bit](const bool on) { SetState([&] { _levels = on ? _levels | bit : _levels & ~bit; }); },
                                         std::format("{}  {}", label, n));
                };
                kui::Widget bar = kui::Row({
                    levelBox(Level::eError, "Errors", counts[2]),
                    levelBox(Level::eWarn, "Warnings", counts[1]),
                    levelBox(Level::eInfo, "Messages", counts[0]),
                    kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = _filter, .placeholder = "Find in the log",
                        .onChanged = [this](const std::string& typed) { SetState([&] { _filter = typed; }); } }).Key("log filter")),
                    kui::Tooltip("Copies the lines shown", SmallIcon("content_copy", [this] {
                        std::string text;
                        for (const std::size_t i : _shownLines) text += std::format("[{:.2f}] {}\n", _log[i].time, _log[i].message);
                        kor::Window::SetClipboardText(text);
                    })),
                    kui::Tooltip("Clears the log: what is said from now on is shown", SmallIcon("delete_sweep", [this] { SetState([&] { _log.clear(); _following = true; }); })),
                }, kui::FlexOptions {}.SetGap(10.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                kui::Widget list = _shownLines.empty()
                    ? kui::Padding(kui::EdgeInsets::Symmetric(10.f, 6.f), kgui::Muted(_log.empty() ? "Nothing has been logged." : "Nothing logged is ticked, or has what is typed."))
                    : kui::LazyList(kui::LazyListOptions {
                          .count = _shownLines.size(), .estimatedExtent = 20.f, .gap = 2.f, .paddingStart = 4.f, .paddingEnd = 10.f,
                          // Scrolled up from its end, it stays where it is read as more is logged; back at its end, it follows again.
                          .onRange = [this, count = _shownLines.size()](std::size_t, const std::size_t last) { _following = last >= count; },
                          .jumpIndex = _shownLines.size() - 1, .jump = _jump },
                      [this](const std::size_t i) { return Line(_log[_shownLines[i]]); });
                return kui::Column({
                    kui::Padding(kui::EdgeInsets::Symmetric(10.f, 6.f), std::move(bar)),
                    kui::Expanded(std::move(list)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
            }

        private:
            /** What has been logged since it last looked: kept, the oldest let go of as the framework's history lets go of them. */
            void Pull()
            {
                if (kor::log::LastSequence() < _seen) { _log.clear(); _seen = 0; }     // the history was cleared under it
                auto fresh = kor::log::HistorySince(_seen);
                if (fresh.empty()) return;
                _seen = fresh.back().sequence;
                for (auto& record : fresh) _log.push_back(std::move(record));
                if (const std::size_t most = std::max<std::size_t>(kor::log::HistoryLimit(), 1); _log.size() > most)
                    _log.erase(_log.begin(), _log.begin() + static_cast<std::ptrdiff_t>(_log.size() - most));
                if (_following) ++_jump;    // read at its end, it stays at its end
            }

            kui::Widget Line(const kor::log::Record& record)
            {
                using Level = kor::log::Level;
                const kui::Theme& t = kui::Theme::Current();
                const kui::Color ink = record.level == Level::eError ? kui::Color::Hex(0xFF6B6B) : record.level == Level::eWarn ? kui::Color::Hex(0xE0A33A) : t.text;
                const char* icon = record.level == Level::eError ? "error" : record.level == Level::eWarn ? "warning" : "info";
                kui::Widget line = kui::Padding(kui::EdgeInsets::Symmetric(10.f, 1.f), kui::Row({
                    kui::SizedBox(48.f, 16.f, kui::Text(std::format("{:.2f}", record.time), kui::TextStyle {}.SetSize(11.f).SetColor(t.textMuted), kui::TextAlign::eEnd, false)),
                    kui::SizedBox(14.f, 14.f, kui::Icon(icon, kui::IconStyle::eFilled, record.level == Level::eInfo ? t.textMuted : ink)),
                    kui::Expanded(kui::Text(record.message, kui::TextStyle {}.SetSize(12.5f).SetColor(ink))),
                }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)));
                // What pressing it does: a node's line goes to the node; the project's scripts not running opens it with them.
                std::function<void()> press;
                const std::string_view message = std::string_view(record.message).starts_with("Resolved: ") ? std::string_view(record.message).substr(10) : std::string_view(record.message);
                if (const auto colon = message.find(':'); colon != std::string_view::npos && colon > 0) {
                    std::string node(message.substr(0, colon));
                    if (std::ranges::any_of(_session->Doc().Graph().nodes, [&](const auto& n) { return n.id == node; }))
                        press = [this, node = std::move(node)] { _session->GoTo(node); };
                }
                if (press) line = kui::GestureDetector(kui::GestureOptions {}.OnTap(std::move(press)), std::move(line));
                return kui::ContextMenu({ { .label = "Copy", .onSelected = [text = record.message] { kor::Window::SetClipboardText(text); } } }, std::move(line))
                    .Key(std::to_string(record.sequence));
            }

            std::shared_ptr<Session> _session;
            std::vector<kor::log::Record> _log;
            std::vector<std::size_t> _shownLines;      // of the log, those ticked and with what is typed
            std::uint64_t _seen = 0;
            unsigned _levels = 0b111;
            std::string _filter;
            bool _following = true;
            std::uint32_t _jump = 1;
            std::chrono::steady_clock::time_point _at {};
        };

        class Root final : public kui::StatefulWidget {
        public:
            explicit Root(std::shared_ptr<Session> session) : _session(std::move(session))
            {
                _layout = StartingLayout();
                // Where they were left, the run before.
                if (!std::getenv("KORAL_ENGINE_NO_LAYOUT")) {
                    std::ifstream saved(LayoutFile());
                    std::stringstream text;
                    text << saved.rdbuf();
                    if (saved && !text.str().empty() && _layout->Load(text.str()) && text.str().find("\"viewer\"") == std::string::npos)
                        _layout->Close("viewer");      // a layout from before there was a viewer: it opens when something is viewed
                    for (const char* panel : { "graph", "scene", "game", "inspector", "outliner", "problems", "assets", "passes", "here" })
                        if (!_layout->IsOpen(panel)) _layout->Open(panel);      // a layout from before there was such a panel
                    // From before the passes and what a page can use were panels: where they start.
                    if (text.str().find("\"passes\"") == std::string::npos) _layout->Dock("passes", kui::DockArea::eLeft, 1);
                    if (text.str().find("\"here\"") == std::string::npos) _layout->Dock("here", kui::DockArea::eBottomRight);
                    // From before the frame was a panel: a tab beside what is wrong.
                    if (text.str().find("\"frame\"") == std::string::npos) { _layout->Dock("frame", kui::DockSide::eCenter, "problems"); _layout->Activate("assets"); }
                    // The pass flow was a tab of its own: it is the Graph panel's, gone back up to the frame.
                    _layout->Close("flow");
                }
                _layout->Activate("scene");     // what it opens on, however it was left
            }

            /** Where the panels are before anybody has moved them. */
            static std::shared_ptr<kui::DockLayout> StartingLayout()
            {
                auto layout = std::make_shared<kui::DockLayout>();
                // The graph has the middle, and most of the window: it is what is worked on. The scene it makes is
                // beside it with the inspector under that, and what is wrong is a strip along the bottom.
                layout->Dock("graph", kui::DockArea::eCenter, 0)
                        // The scene the graph makes is beside it, the middle shared between them: a line to drag. What it
                        // looks like played is a tab beside that, brought forward by Play — or dragged to a part of its own.
                        .Dock("scene", kui::DockArea::eCenter, 1)
                        .Dock("game", kui::DockSide::eCenter, "scene")
                        .Dock("inspector", kui::DockSide::eRight, "graph", 0.2f)
                        .Dock("outliner", kui::DockSide::eLeft, "graph", 0.16f)
                        // The graph's passes under the entities, as their tree is: what is picked to be shown in the middle.
                        .Dock("passes", kui::DockArea::eLeft, 1)
                        // Along the bottom, the project's files, with room for their folders beside them; what is
                        // wrong is behind them, a press away.
                        .Dock("problems", kui::DockSide::eBottom, "graph", 0.27f)
                        .Dock("assets", kui::DockSide::eCenter, "problems")
                        // And the frame as it runs, a tab beside them: what each pass costs, in the order they run.
                        .Dock("frame", kui::DockSide::eCenter, "problems")
                        // Beside them, what the page shown can use of the passes before it: things to drag onto it, as files are.
                        .Dock("here", kui::DockArea::eBottomRight)
                        // What is looked at closely is a tab beside the scene, there once something is.
                        .Dock("viewer", kui::DockSide::eCenter, "scene");
                layout->Close("viewer");
                layout->Activate("graph");
                layout->Activate("assets");
                layout->Activate("scene");
                return layout;
            }

            void InitState() override
            {
                // The document changes under the panels — an edit here, a shader saved elsewhere: built again when it has.
                Animate([this](float) {
                    // What a page can use is pictured while its panel is in sight: not behind another's tab, nor closed.
                    if (const bool shown = _layout->IsShown("here"); shown != _session->DrawerShown()) _session->ShowDrawer(shown);
                    if (_session->TakeAskForNodes()) AskForNode(NewNodeAt());
                    if (_session->TakeAskForNewShader()) SetState([&] { _dialog = Dialog::eNewShader; _newId.clear(); _newKind = 0; });
                    if (_session->TakeAskForNewProject()) AskForNewProject();
                    if (_session->TakeAskToArrange()) Arrange();
                    // Every page put in order: each once it has been drawn as it is shown, its nodes measured — a frame on.
                    if (const auto& pages = _session->PagesToArrange(); !pages.empty()) {
                        if (ShownPass() != pages.front() || _placedPage != pages.front()) _arrangeWait = 0;
                        else if (++_arrangeWait > 2) {
                            _arrangeWait = 0;
                            Arrange();
                            _session->ArrangedPage();
                        }
                        SetState();
                    }
                    // A node gone to — found, or pressed among the problems — is brought into the graph's view.
                    if (const auto went = _session->Went(); went != _went) { _went = went; _view->Reveal(_session->selectedNode); SetState(); }
                    // A model to be imported, while what its meshes have is read: shown when it is known.
                    if (_dialog == Dialog::eImportOptions && !_assets._importAttributes && Doc().Runtime().ModelAttributes(_assets._importFile)) SetState();
                    // Something opened in the viewer: the viewer is brought forward.
                    // Played, the Game panel is brought forward; and it is drawn only while it is seen.
                    if (const auto played = _session->PlayStarted(); played != _played) {
                        _played = played;
                        _layout->Open("game");
                        _layout->Activate("game");
                        SetState();
                    }
                    if (GameScene* game = _session->Game()) game->SetShown(_layout->IsShown("game"));
                    if (const auto asked = _session->ViewAsked(); asked != _viewAsked) {
                        _viewAsked = asked;
                        _layout->Open("viewer");
                        _layout->Activate("viewer");
                        SetState();
                    }
                    if (const auto version = _session->Version(); version != _shown) SetState([&] { _shown = version; });
                    if (const auto now = std::chrono::steady_clock::now(); now - _problemsAt > std::chrono::milliseconds(250)) { _problemsAt = now; ReportProblems(); }
                    // Played, the picked entity's inspector follows what is done to it on its own (Following).
                    return true;
                });
            }

            kui::Widget Build() override
            {
                _shown = _session->Version();
                // The window's own title bar, drawn here in place of the system's: the menus at its start, then which
                // project is open, room to move the window by, and the window's buttons.
                const std::string prefab = _session->PrefabOpen();
                const std::string title = !prefab.empty()
                    ? std::format("{}{} — {} — Koral Engine", std::filesystem::path(prefab).filename().string(), _session->PrefabChanged() ? " (not saved)" : "", _session->CurrentProject().name)
                    : std::format("{}{} — Koral Engine", _session->HasProject() ? _session->CurrentProject().name : std::string("No project"), Doc().Dirty() ? " (not saved)" : "");
                auto body = kui::Column({
                    kui::TitleBar(title, TitleBarOptions()),
                    kui::Expanded(kui::DockSpace(_layout, {
                        Docked("graph", "Graph", "account_tree", Graph()),
                        ScenePanelDocked(),
                        GamePanelDocked(),
                        Docked("outliner", "Scene tree", "list", Outliner()),
                        Docked("passes", "Outline", "layers", PassList()),
                        Docked("here", "Here to use", "inventory_2", HereToUse()),
                        Docked("inspector", "Inspector", "tune", Inspector()),
                        Docked("problems", "Log", "terminal", kui::Make<LogPanel>(_session)),
                        Docked("frame", "Frame", "timeline", FramePanel()),
                        Docked("assets", "Assets", "folder", _assets.AssetsPanel()),
                        ViewerDocked(),
                    }, kui::DockOptions {}.OnChanged([this] { SaveLayout(); }).OnClosed([this](const std::string& panel) {
                        if (panel == "viewer") _session->StopViewing();     // closed, what it copied out and drew stops
                        SaveLayout();
                    }))),
                    kui::StatusBar(std::format("{}      {} {}{}", _session->Status(), Doc().World().EntityCount(), Doc().World().EntityCount() == 1 ? "entity" : "entities",
                                               Doc().Reading() == 0 ? std::string() : std::format("      reading {} {}...", Doc().Reading(), Doc().Reading() == 1 ? "file" : "files")),
                                   _session->Failed() ? kui::StatusLevel::eError : kui::StatusLevel::eInfo),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetMainAxisSize(kui::MainAxisSize::eMax));
                return kui::Modal(_dialog != Dialog::eNone, std::move(body), DialogBody(), [this] { SetState([&] { _dialog = Dialog::eNone; }); });
            }

        private:
            using Dialog = EditorDialog;
            static constexpr float ListSize = 28.f;     // the least the Assets panel shows a thing at: a line of a list

            /**
             * Koral's coral, in the window's top left corner: as big as the buttons down the side, and over them. From the
             * engine's package (`editor/logo.png`), read the first time it is shown; nothing, when it is not there.
             */
            kui::Widget Logo()
            {
                if (!_logoRead) {
                    _logoRead = true;
                    if (const auto file = kscript::PackageDirectory() / "editor" / "logo.png"; std::filesystem::exists(file))
                        if (auto logo = kimg::LoadImage(file, true); logo.Valid()) _logo = std::move(logo);
                }
                if (!_logo.Valid()) return kui::SizedBox(6.f, 30.f, kui::Container({}));
                return kui::Padding(kui::EdgeInsets::Only(6.f, 0.f, 2.f, 0.f), kui::Tooltip("Koral Engine",
                    kui::SizedBox(30.f, 30.f, kui::ClipRRect(8.f, kui::Image(kor::ResourceRef<const kor::Image>(_logo), kui::ImageFit::eCover)))));
            }
            kor::Resource<kor::Image> _logo;
            bool _logoRead = false;

            /**
             * The title bar: the logo and the menus at its start, the scene's play controls in its middle, and at its end the window's buttons — the editor's own,
             * shaped as the dock's side buttons are, the last of them over the right side's. (On macOS, the system's.)
             */
            kui::TitleBarOptions TitleBarOptions()
            {
                auto options = kui::TitleBarOptions {}.SetLeading(kui::Row({ Logo(), Menus() }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)))
                    .SetCenter(PlayControls());
#ifndef __APPLE__
                using Does = WindowButton::Does;
                options.SetButtons(false).SetTrailing(kui::Padding(kui::EdgeInsets::Only(0.f, 0.f, 6.f, 0.f), kui::Row({
                    kui::Tooltip("Minimize", kui::Make<WindowButton>(_session->window, Does::eMinimize)),
                    kui::Tooltip(_session->window && _session->window->IsMaximized() ? "Restore" : "Maximize", kui::Make<WindowButton>(_session->window, Does::eMaximize)),
                    kui::Tooltip("Close", kui::Make<WindowButton>(_session->window, Does::eClose)),
                }, kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
#endif
                return options;
            }

            /** Where the panels are, kept from one run to the next: this user's, not a project's. */
            static std::filesystem::path LayoutFile() { return ConfigDirectory() / "editor.layout"; }
            /** The project that is open, as the one the editor opens when given none; already that, the engine's own again. */
            void ToggleStartup()
            {
                const std::filesystem::path open = _session->CurrentProject().directory;
                const bool already = Same(Session::StartupProject(), open);
                if (!Session::SetStartupProject(already ? std::filesystem::path() : open)) { _session->Say("The project to start with could not be kept.", true); return; }
                _session->Say(already ? "The editor opens its own project when it is given none, again."
                                      : std::format("The editor opens {} when it is given none.", _session->CurrentProject().name));
                SetState();
            }
            void SaveLayout() const
            {
                if (std::getenv("KORAL_ENGINE_NO_LAYOUT")) return;
                std::error_code ignored;
                std::filesystem::create_directories(LayoutFile().parent_path(), ignored);
                std::ofstream(LayoutFile(), std::ios::trunc) << _layout->Save();
            }

            /**
             * What throws the scene away — another project, a scene made anew, the saved one put back — asks first
             * when there is something not saved in it.
             */
            std::function<void()> Asking(std::string what, std::function<void()> then)
            {
                return [this, what = std::move(what), then = std::move(then)] {
                    if (!Doc().Dirty()) { then(); return; }
                    SetState([&] { _dialog = Dialog::eConfirm; _confirmWhat = what; _confirmed = then; });
                };
            }
            /** The engine's own modules, a button each, and any other from a folder. */
            kui::Widget ImportModuleDialog()
            {
                const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
                const auto secondary = kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary);
                std::vector<kui::Widget> rows;
                rows.push_back(Heading("Import a module"));
                const auto official = keditor::Project::ModulesIn(keditor::Project::OfficialModules());
                rows.push_back(kgui::Line(official.empty() ? "The engine has no modules of its own beside it." : "The engine's own: what a project can do without."));
                std::vector<kui::Widget> buttons;
                for (const auto& module : official) {
                    const bool imported = std::ranges::contains(_session->CurrentProject().modules, _session->CurrentProject().Reference(module.directory));
                    buttons.push_back(kui::Button(imported ? module.name + " (imported)" : module.name,
                                                  [this, close, folder = module.directory] { close(); _session->ImportModule(folder); },
                                                  kui::ButtonOptions {}.SetStyle(imported ? kui::ButtonStyle::eSecondary : kui::ButtonStyle::ePrimary)));
                }
                if (!buttons.empty()) rows.push_back(kui::Wrap(std::move(buttons), 6.f, 6.f));
                rows.push_back(kui::Row({
                    kui::Button("From a folder...", [this] { SetState([&] { _dialog = Dialog::eImportModuleFolder; }); }, secondary),
                    kui::Button("Cancel", close, secondary),
                }, kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows),
                    kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(10.f)).Width(560.f));
            }

            kui::Widget ConfirmDialog()
            {
                const auto close = [this] { SetState([&] { _dialog = Dialog::eNone; }); };
                const auto secondary = kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary);
                std::vector<kui::Widget> buttons;
                if (_session->HasProject() && !_session->InPlayMode())
                    buttons.push_back(kui::Button("Save, then go on", [this, close] { const auto then = _confirmed; close(); if (_session->Save() && then) then(); }));
                buttons.push_back(kui::Button("Go on without saving", [this, close] { const auto then = _confirmed; close(); if (then) then(); }, secondary));
                buttons.push_back(kui::Button("Cancel", close, secondary));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading("The scene has changes that are not saved"),
                    kgui::Line(std::format("{} leaves this scene: what was done to it since it was saved would be lost.", _confirmWhat)),
                    kui::Row(std::move(buttons), kui::FlexOptions {}.SetGap(8.f)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(10.f)).Width(520.f));
            }
            std::string _confirmWhat;
            std::function<void()> _confirmed;

            /** The language a new project's scripts are in, asked once its folder is chosen: C# or Kotlin, all of them. */
            /** What a new project is to be, as its form has it so far. */
            struct NewProjectForm {
                std::string name;
                char language = 'k';                            ///< 'c' C#, 'k' Kotlin: every script of it, and of what it imports.
                bool module = false;                            ///< A module, for projects to import: its one scene is for trying it out.
                std::string folder;                             ///< What it is made in: at folder/name.
                std::vector<std::filesystem::path> picked;      ///< Modules picked from elsewhere: offered beside those found in the folder.
                std::vector<std::filesystem::path> imports;     ///< What it imports, of those offered.
            };
            NewProjectForm _newProject;

            /** Opens the form for a new project: named nothing yet, in the projects folder, in Kotlin, importing nothing. */
            void AskForNewProject()
            {
                SetState([&] {
                    _newProject = {};
                    _newProject.folder = Session::ProjectsDirectory().string();
                    _dialog = Dialog::eNewProject;
                });
            }

            kui::Widget NewProjectDialog()
            {
                NewProjectForm& form = _newProject;
                const auto muted = [](std::string text) { return kgui::Line(std::move(text), kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(12.f)); };
                const auto label = [](const char* text) { return kgui::Line(text, kui::TextStyle {}.SetSize(12.f)); };
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(form.module ? "A new module" : "A new project"));

                rows.push_back(label("Name"));
                rows.push_back(kui::TextField(kui::TextFieldOptions { .text = form.name, .placeholder = "What it is called: its folder's name too",
                                                                      .onChanged = [this](const std::string& typed) { _newProject.name = typed; }, .width = 520.f }));

                rows.push_back(label("Its scripts are in"));
                rows.push_back(kui::Row({
                    kui::RadioButton(form.language == 'c', [this] { SetState([&] { _newProject.language = 'c'; }); }, "C#"),
                    kui::RadioButton(form.language == 'k', [this] { SetState([&] { _newProject.language = 'k'; }); }, "Kotlin"),
                }, kui::FlexOptions {}.SetGap(16.f)));

                rows.push_back(label("It is"));
                rows.push_back(kui::RadioButton(!form.module, [this] { SetState([&] { _newProject.module = false; }); }, "A project: its scenes, and what they are made of"));
                rows.push_back(kui::RadioButton(form.module, [this] { SetState([&] { _newProject.module = true; }); },
                                                "A module: what projects import — scripts, shaders, functions, objects, assets — and a scene to try it in"));

                rows.push_back(label("In"));
                rows.push_back(kui::Row({
                    kui::TextField(kui::TextFieldOptions { .text = form.folder, .placeholder = "The folder it is made in",
                                                           .onChanged = [this](const std::string& typed) { _newProject.folder = typed; }, .width = 420.f }),
                    kui::Button("Browse...", [this] { SetState([&] { _dialog = Dialog::eNewProjectFolder; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)),
                }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                const std::filesystem::path at = std::filesystem::path(form.folder) / form.name;
                rows.push_back(muted(form.name.empty() ? "Made in a folder of its name, there." : std::format("Made at {}", at.generic_string())));

                // What it imports: the modules in the folder it is made in, and any picked from elsewhere — those of its language.
                rows.push_back(label("It imports"));
                std::vector<keditor::Project> offered = keditor::Project::ModulesIn(form.folder);
                for (auto& official : keditor::Project::ModulesIn(keditor::Project::OfficialModules()))     // the engine's own, wherever it is made
                    if (std::ranges::none_of(offered, [&](const auto& other) { return other.directory == official.directory; })) offered.push_back(std::move(official));
                for (const auto& folder : form.picked) {
                    keditor::Project module;
                    std::string ignored;
                    if (keditor::Project::Open(folder, module, ignored) && std::ranges::none_of(offered, [&](const auto& other) { return other.directory == module.directory; }))
                        offered.push_back(std::move(module));
                }
                std::erase_if(form.imports, [&](const auto& chosen) { return std::ranges::none_of(offered, [&](const auto& module) { return module.directory == chosen; }); });
                bool any = false;
                for (const auto& module : offered) {
                    const char language = module.Language();
                    if (language != 0 && language != form.language) continue;      // only what is in its language can be imported
                    any = true;
                    const bool on = std::ranges::contains(form.imports, module.directory);
                    rows.push_back(kui::Checkbox(on, [this, folder = module.directory](const bool ticked) {
                        SetState([&] {
                            std::erase(_newProject.imports, folder);
                            if (ticked) _newProject.imports.push_back(folder);
                        });
                    }, std::format("{}   ·   {}", module.name, module.directory.generic_string())));
                }
                if (!any) rows.push_back(muted(std::format("No module in {} there has its scripts in {}.", form.folder.empty() ? "the folder" : form.folder, form.language == 'k' ? "Kotlin" : "C#")));
                rows.push_back(kui::Button("Add a module from a folder...", [this] { SetState([&] { _dialog = Dialog::eNewProjectModule; }); },
                                           kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)));

                const auto make = [this] {
                    const NewProjectForm made = _newProject;
                    const bool named = !made.name.empty() && made.name.find_first_of("/\\:*?\"<>|") == std::string::npos && made.name != "." && made.name != "..";
                    if (!named) { _session->Say("A project's name is its folder's: some letters, and none of / \\ : * ? \" < > |.", true); SetState(); return; }
                    if (made.folder.empty()) { _session->Say("Say which folder it is made in.", true); SetState(); return; }
                    if (_session->NewProject(std::filesystem::path(made.folder) / made.name, made.language, made.module, made.imports)) SetState([&] { _dialog = Dialog::eNone; });
                    else SetState();
                };
                if (_session->Failed()) rows.push_back(Problem(_session->Status()));
                rows.push_back(kui::Row({ kui::Button(form.module ? "Make the module" : "Make the project", make),
                                          kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                        kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(640.f));
            }

            /** A module picked for the new project from a folder of its own: imported when it is one, and in the language. */
            void PickModuleForNewProject(const std::filesystem::path& folder)
            {
                keditor::Project module;
                std::string error;
                if (!keditor::Project::Open(folder, module, error)) _session->Say(error, true);
                else if (!module.module) _session->Say(std::format("{} is a project, not a module: only a module is imported.", module.name), true);
                else if (const char language = module.Language(); language != 0 && language != _newProject.language)
                    _session->Say(std::format("{}'s scripts are in {}: what is imported is in the new one's language.", module.name, language == 'k' ? "Kotlin" : "C#"), true);
                else {
                    if (!std::ranges::contains(_newProject.picked, module.directory)) _newProject.picked.push_back(module.directory);
                    if (!std::ranges::contains(_newProject.imports, module.directory)) _newProject.imports.push_back(module.directory);
                }
                SetState([&] { _dialog = Dialog::eNewProject; });
            }

            Document& Doc() { return _session->Doc(); }

            // ---- the scene ---------------------------------------------------------------------------------------

            /** A button of the scene's toolbar: lit while what it stands for is on. */
            /** A panel, with the picture its button in the stripe shows. */
            static kui::DockPanel Docked(const char* id, const char* title, const char* icon, kui::Widget content)
            {
                kui::DockPanel panel { id, title, std::move(content), false };
                panel.icon = kui::MaterialIcon(icon);
                return panel;
            }

            kui::DockPanel ViewerDocked()
            {
                kui::DockPanel panel { "viewer", "Viewer", ViewerPanel(_session), true };
                panel.icon = kui::MaterialIcon("image_search");
                return panel;
            }

            kui::DockPanel ScenePanelDocked()
            {
                // A model dropped on the scene from the Assets panel is imported into it.
                // Each is put on the surface it is dropped on, where there is one.
                kui::Widget scene = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this](const kui::DragData& dragged, kor::Vec2) {
                    const auto* id = dragged.As<std::string>();
                    const auto asset = id ? AssetOf(*id) : std::nullopt;
                    const auto pointer = _session->viewport->PointerPosition();
                    const kor::UVec2 size = _session->viewport->Size();
                    const float u = pointer && size.x > 0 ? pointer->x / static_cast<float>(size.x) : 0.5f, v = pointer && size.y > 0 ? pointer->y / static_cast<float>(size.y) : 0.5f;
                    if (asset && asset->kind == Session::Asset::Kind::eModel) _session->DropAt(u, v, [this, file = asset->file] { _assets.AskToImport(file); });
                    else if (asset && asset->kind == Session::Asset::Kind::ePrefab) _session->DropAt(u, v, [session = _session, id = asset->id] { session->PlacePrefab(id); });
                    else _session->Say("A model dropped on the scene is imported and a prefab placed; a mesh, a texture, a material or a script is dropped on the entity that is to have it.", true);
                }), ScenePanel());
                kui::DockPanel panel { "scene", "Scene", std::move(scene), false };
                panel.icon = kui::MaterialIcon("videocam");
                panel.titleBar = TitleTools();
                return panel;
            }

            /**
             * The scene as it plays, filling its panel: seen from its own camera, its scripts steered from here — and
             * nothing of the editor's over it. A click here picks nothing.
             */
            kui::DockPanel GamePanelDocked()
            {
                kui::Widget view = _session->Game() ? kgui::SceneView(*_session->Game()) : kui::Text("");
                kui::DockPanel panel { "game", "Game", std::move(view), false };
                panel.icon = kui::MaterialIcon("sports_esports");
                return panel;
            }

            static kui::Widget ToolSeparator() { return kui::SizedBox(1.f, 16.f, kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x606060))))); }

            /** What the Scene panel's title bar has, at its end: whose eyes it is seen through. */
            kui::Widget TitleTools()
            {
                return kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kui::Row({
                    kui::Expanded(kui::Text("")),
                    Tool("videocam", "Look through the scene's camera", _session->ThroughSceneCamera(), [this] { _session->SetLookThroughCamera(!_session->lookThroughCamera); }),
                }, kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
            }

            /**
             * Play, pause, a step and stop, in the middle of the window's title bar: the scene's, whichever panel shows
             * it. While it plays they sit on the theme's colour, and while it is paused on a dimmer one of it, so that
             * what is done to the scene then — and is lost at Stop — is not mistaken for an edit.
             */
            kui::Widget PlayControls()
            {
                const bool playMode = _session->InPlayMode(), playing = _session->IsPlaying();
                std::vector<kui::Widget> tools;
                if (playing) tools.push_back(Tool("pause", "Pause", false, [this] { _session->Pause(); }));
                else tools.push_back(Tool("play_arrow", playMode ? "Resume" : "Play: scripts and the scene's systems run", playMode, [this] { _session->StartPlaying(); }));
                tools.push_back(Tool("skip_next", "One fixed step, and paused: the scripts' fixedUpdate and the graph's fixed pages, once", false, [this] { _session->Step(); }, !playing));
                tools.push_back(Tool("stop", "Stop, and put the scene back as it was", false, [this] { _session->Stop(); }, playMode));
                const kui::Color backing = playing ? kui::Theme::Current().primary.WithAlpha(0.45f) : playMode ? kui::Theme::Current().primary.WithAlpha(0.2f) : kui::colors::Transparent;
                return kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)).SetDecoration(kui::Decoration {}.SetColor(backing).SetRadius(6.f)),
                                      kui::Row(std::move(tools), kui::FlexOptions {}.SetGap(2.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter).SetMainAxisSize(kui::MainAxisSize::eMin)));
            }

            /**
             * The scene as it runs, filling its panel: over it what the editor draws of its own — the line round what
             * is picked, the gizmo, the icons, a ruler — and in its corners its tools on a backing that reads over
             * whatever the scene shows behind it: what is picked and how it is moved at the top left, how it is seen at
             * the top right; its numbers at the foot, what is measured, and which way the view looks. Split, a second
             * view of it beside the first.
             */
            kui::Widget ScenePanel()
            {
                const kecs::Entity selected = _session->selectedEntity;
                const bool some = !_session->Selection().empty();
                const auto backing = [](kui::Widget content) {
                    return kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(5.f, 4.f))
                                              .SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x101012).WithAlpha(0.69f)).SetRadius(6.f)),
                                          std::move(content));
                };
                const auto row = [](std::vector<kui::Widget> tools) {
                    return kui::Row(std::move(tools), kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetMainAxisSize(kui::MainAxisSize::eMin));
                };

                // ---- what is picked, and how it is moved
                std::vector<kui::Widget> tools;
                tools.push_back(Tool("near_me", "Select only: no gizmo. A drag from where nothing is picks what is in its box; Ctrl or Shift adds", !_session->gizmoEnabled,
                                     [this] { _session->SetGizmo(false, _session->gizmo); }));
                for (const auto& [mode, icon, what] : { std::tuple { kor::GizmoMode::eTranslate, "open_with", "Move what is picked" },
                                                        std::tuple { kor::GizmoMode::eRotate, "3d_rotation", "Rotate what is picked" },
                                                        std::tuple { kor::GizmoMode::eScale, "open_in_full", "Scale what is picked" } })
                    tools.push_back(Tool(icon, what, _session->gizmoEnabled && _session->gizmo == mode, [this, mode] { _session->SetGizmo(true, mode); }));
                tools.push_back(Tool(_session->gizmoWorld ? "public" : "my_location", _session->gizmoWorld ? "Along the world's axes: press for its own" : "Along its own axes: press for the world's",
                                     _session->gizmoWorld, [this] { _session->gizmoWorld = !_session->gizmoWorld; SetState(); }));
                tools.push_back(SnapTool());
                tools.push_back(Tool("vertical_align_bottom", "Moved, put on the surface under the pointer", _session->surfaceSnap,
                                     [this] { _session->surfaceSnap = !_session->surfaceSnap; SetState(); }));
                tools.push_back(ToolSeparator());
                tools.push_back(Tool("center_focus_strong", "Frame what is picked (the view: right-drag turns, middle-drag moves, the wheel zooms)", false,
                                     [this, selected] { _session->Frame(selected); }, !selected.IsNull()));
                tools.push_back(Tool("visibility_off", "Hide what is picked in this view (H)", false, [this] { _session->HideSelected(); }, some));
                tools.push_back(Tool("filter_center_focus", "Show what is picked by itself (Shift+H)", _session->Isolated(), [this] { _session->Isolate(); }, some));
                if (!_session->ViewHidden().empty())
                    tools.push_back(Tool("visibility", std::format("Show the {} hidden again (Alt+H)", _session->ViewHidden().size()), true, [this] { _session->ShowAll(); }));
                tools.push_back(ToolSeparator());
                const auto measure = _session->measure;
                tools.push_back(Tool("straighten", "A ruler: click two points on a surface", measure == Session::Measure::eRuler, [this, measure] {
                    _session->SetMeasure(measure == Session::Measure::eRuler ? Session::Measure::eOff : Session::Measure::eRuler); }));
                tools.push_back(Tool("colorize", "What is under the pointer: the entity, where and which way its surface faces", measure == Session::Measure::eProbe, [this, measure] {
                    _session->SetMeasure(measure == Session::Measure::eProbe ? Session::Measure::eOff : Session::Measure::eProbe); }));
                tools.push_back(ToolSeparator());
                tools.push_back(ViewModeTool());
                tools.push_back(GridTool());
                tools.push_back(ToolSeparator());
                tools.push_back(kui::Make<FrameRate>(_session));
                // What the graph can be switched between — its picture, a debug view of it — without going to the node that says which.
                for (const auto& choice : _session->Choices()) {
                    tools.push_back(ToolSeparator());
                    tools.push_back(kui::Tooltip(std::format("{}: what the graph's Switch shows", choice.driver),
                                                 kui::SizedBox(150.f, 24.f, kui::Dropdown(choice.options, std::clamp(choice.chosen, 0, static_cast<int>(choice.options.size()) - 1),
                                                                                          [this, driver = choice.driver](const int index) { _session->Choose(driver, index); }))));
                }

                // ---- how it is seen
                std::vector<kui::Widget> seen;
                seen.push_back(kui::Make<LiveText>([session = _session] { return std::format("{:.2g}x", session->flySpeed); }, "How fast the view flies: the wheel while the right button is held"));
                seen.push_back(ViewsTool());
                seen.push_back(Tool("lightbulb", "Icons for the lights, cameras and what draws nothing: a click on one picks it", _session->iconsShown,
                                    [this] { _session->iconsShown = !_session->iconsShown; SetState(); }));
                seen.push_back(Tool("query_stats", "The frame's numbers: what was drawn, computed, traced, and what it took", _session->statsShown,
                                    [this] { _session->statsShown = !_session->statsShown; SetState(); }));
                seen.push_back(Tool("vertical_split", _session->splitView ? "One view" : "Two views side by side", _session->splitView,
                                    [this] { _session->SetSplit(!_session->splitView); }));

                std::vector<kui::Widget> layers;
                layers.push_back(kgui::SceneView(_session->Play(), _session->viewport));
                // Over the scene's picture: a DebugView's in its place, when one is chosen; the grid; and the overlay.
                const auto cover = [&](kor::ResourceRef<const kor::Image> picture) {
                    if (picture.Get())
                        layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 0.f, .right = 0.f, .bottom = 0.f }, kui::IgnorePointer(kui::Image(picture, kui::ImageFit::eFill))));
                };
                cover(_session->ViewModePicture());
                cover(_session->GridPicture());
                if (_session->Play().Over())
                    if (const auto picture = _session->OverPicture(); picture.Get())
                        layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 0.f, .right = 0.f, .bottom = 0.f }, kui::IgnorePointer(kui::Image(picture, kui::ImageFit::eFill))));
                // The two toolbars along the top, the second after the first where there is room and under it where
                // not; the first's tools a line after another where the panel is narrower than they are.
                layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 8.f, .top = 8.f, .right = 8.f },
                    kui::Wrap({ backing(kui::Wrap(std::move(tools), 4.f, 4.f)), backing(row(std::move(seen))) }, 8.f, 6.f)));
                // At the foot, on the left: what is measured, over the frame's numbers.
                std::vector<kui::Widget> foot;
                if (measure != Session::Measure::eOff) foot.push_back(backing(kui::Make<LiveText>([this] { return MeasureText(); }, std::string(), 15)));
                if (_session->statsShown) foot.push_back(backing(kui::Make<LiveText>([this] { return StatsText(); }, std::string(), 4)));
                if (!foot.empty())
                    layers.push_back(kui::Positioned(kui::PositionedOptions { .left = 8.f, .bottom = 8.f },
                        kui::Column(std::move(foot), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetMainAxisSize(kui::MainAxisSize::eMin))));
                layers.push_back(kui::Positioned(kui::PositionedOptions { .right = 8.f, .bottom = 8.f }, kui::Make<ViewCube>(_session)));
                kui::Widget first = kui::Stack(std::move(layers));
                if (!_session->splitView || !_session->SecondView()) return first;
                // The second view: the scene as the first draws it, from its own place, and nothing of the editor's over it.
                kui::Widget second = kui::Stack({
                    kgui::SceneView(*_session->SecondView(), _session->secondViewport),
                    kui::Positioned(kui::PositionedOptions { .left = 8.f, .top = 8.f }, backing(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f),
                        kui::Text("Second view", kui::TextStyle {}.SetSize(12.f).SetColor(kui::Color::Hex(0xE0E0E0)))))),
                    kui::Positioned(kui::PositionedOptions { .right = 8.f, .bottom = 8.f }, kui::Make<ViewCube>(_session, true)),
                });
                return kui::Row({ kui::Expanded(std::move(first)), kui::SizedBox(2.f, -1.f, kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(kui::Color::Hex(0x303034))))),
                                  kui::Expanded(std::move(second)) }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
            }

            /** The frame's numbers, as the Scene panel's corner shows them. */
            std::string StatsText()
            {
                const auto report = Doc().Runtime().Frame();
                int run = 0, draws = 0, dispatches = 0, rays = 0, copies = 0;
                double gpu = 0.0;
                for (const auto& pass : report.passes) {
                    ++run;
                    if (pass.work & kor::TimerWork::Draw) ++draws;
                    if (pass.work & kor::TimerWork::Dispatch) ++dispatches;
                    if (pass.work & kor::TimerWork::Rays) ++rays;
                    if (pass.work & kor::TimerWork::Transfer) ++copies;
                    if (pass.gpuMeasured) gpu += pass.gpuMs;
                }
                const kecs::World& world = Doc().World();
                const kecs::ComponentId meshes = world.Find("Mesh");
                std::size_t entities = 0, drawn = 0;
                world.EachEntity([&](const kecs::Entity entity) {
                    ++entities;
                    if (meshes != kecs::NoComponent && world.Has(entity, meshes) && world.IsEnabled(entity, meshes) && !_session->ViewHidden().contains(entity)) ++drawn;
                });
                std::string text = std::format("{} fps   {:.2f} ms a frame\nGPU {:.2f} ms over {} passes ({} left out)\n{} drawing   {} computing   {} tracing   {} copying\n"
                                               "{} entities, {} drawn",
                                               _session->Fps(), _session->FrameMs(), gpu, run, report.leftOut.size(), draws, dispatches, rays, copies, entities, drawn);
                if (!_session->ViewHidden().empty()) text += std::format(" ({} hidden here)", _session->ViewHidden().size());
                text += std::format("\n{:.0f} MB the frame made itself", static_cast<double>(report.bytes) / (1024.0 * 1024.0));
                return text;
            }

            /** What is measured: the ruler's length, or what is under the pointer. */
            std::string MeasureText()
            {
                const auto& probe = _session->Probed();
                if (_session->measure == Session::Measure::eRuler) {
                    const auto& ends = _session->ruler;
                    if (ends.size() == 2) {
                        const kor::Vec3 d = ends[1] - ends[0];
                        return std::format("{:.3f} m    x {:.3f}   y {:.3f}   z {:.3f}", kor::Length(d), d.x, d.y, d.z);
                    }
                    if (ends.size() == 1) return probe.hit ? std::format("{:.3f} m to the pointer: click the other end", kor::Length(probe.position - ends[0])) : std::string("Click the other end on a surface");
                    return "Click one end of what is measured, on a surface";
                }
                if (!probe.hit) return "Nothing under the pointer";
                const std::string name = Doc().World().Alive(probe.entity) ? Doc().NameOf(probe.entity) : std::string("(nothing picked)");
                return std::format("{}    at {:.3f}, {:.3f}, {:.3f}    facing {:.2f}, {:.2f}, {:.2f}    {:.2f} m away", name, probe.position.x, probe.position.y, probe.position.z,
                                   probe.normal.x, probe.normal.y, probe.normal.z, probe.distance);
            }

            /** How the gizmo snaps: on or off, and its settings beside it — how far a step moves, turns and scales. */
            kui::Widget SnapTool()
            {
                const Session::Snap snap = _session->snap;
                const auto change = [this](std::function<void(Session::Snap&)> how) { how(_session->snap); SetState(); };
                const auto line = [](const char* label, kui::Widget control) {
                    return kui::Padding(kui::EdgeInsets::Symmetric(8.f, 3.f), kui::Row({ kui::SizedBox(96.f, 22.f, kui::Align(kui::Alignment::CenterLeft(), kui::Text(label, kui::TextStyle {}.SetSize(12.f)))),
                                                                                         kui::Expanded(std::move(control)) }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                };
                std::vector<kui::Widget> lines;
                lines.push_back(PopupHeading("Snap: Ctrl held does the other"));
                lines.push_back(line("Snapped", kui::Switch(snap.on, [change](const bool on) { change([&](Session::Snap& s) { s.on = on; }); })));
                lines.push_back(line("Move (m)", kui::DragValue(snap.move, [change](const double v) { change([&](Session::Snap& s) { s.move = static_cast<float>(v); }); },
                                                                kui::DragValueOptions {}.SetRange(0.001f, 100.f).SetLogarithmic(true).SetDecimals(3))));
                lines.push_back(line("Turn (°)", kui::DragValue(snap.turn, [change](const double v) { change([&](Session::Snap& s) { s.turn = static_cast<float>(v); }); },
                                                                kui::DragValueOptions {}.SetRange(0.1f, 180.f).SetSpeed(0.5f).SetDecimals(1))));
                lines.push_back(line("Scale by", kui::DragValue(snap.scale, [change](const double v) { change([&](Session::Snap& s) { s.scale = static_cast<float>(v); }); },
                                                                kui::DragValueOptions {}.SetRange(0.001f, 10.f).SetLogarithmic(true).SetDecimals(3))));
                return kui::Row({
                    Tool("grid_4x4", snap.on ? "Snapped (Ctrl held: free)" : "Free (Ctrl held: snapped)", snap.on, [change] { change([](Session::Snap& s) { s.on = !s.on; }); }),
                    kui::Column({
                        Tool("expand_more", "How far a step snaps", _snapOpen, [this] { SetState([&] { const bool open = !_snapOpen; ClosePopups(); _snapOpen = open; }); }),
                        kui::PopupAnchor(_snapOpen, PopupPanel(kui::Column(std::move(lines), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)), 240.f),
                                         [this] { SetState([&] { _snapOpen = false; }); }, { 0.f, 4.f }),
                    }, kui::FlexOptions {}.SetMainAxisSize(kui::MainAxisSize::eMin)),
                }, kui::FlexOptions {}.SetGap(0.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetMainAxisSize(kui::MainAxisSize::eMin));
            }

            /** Which way the view looks — along an axis, in perspective or straight on — and its bookmarks. */
            kui::Widget ViewsTool()
            {
                const kui::Theme& t = kui::Theme::Current();
                const auto button = [&](const std::string& label, const std::string& tip, const bool on, std::function<void()> pressed, const bool enabled = true, const float width = 60.f) {
                    return kui::Tooltip(tip, kui::Button(
                        kui::Container(kui::ContainerOptions {}.SetSize(width, 22.f).SetAlignment(kui::Alignment::Center())
                                           .SetDecoration(kui::Decoration {}.SetColor(on ? t.primary.WithAlpha(0.35f) : t.surfaceHover).SetRadius(4.f)),
                                       kui::Text(label, kui::TextStyle {}.SetSize(12.f).SetColor(enabled ? t.text : t.textMuted))),
                        [this, pressed = std::move(pressed)] { pressed(); SetState([&] { _viewsOpen = false; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain).SetEnabled(enabled)));
                };
                const auto line = [](std::vector<kui::Widget> items) {
                    return kui::Padding(kui::EdgeInsets::Symmetric(8.f, 3.f), kui::Row(std::move(items), kui::FlexOptions {}.SetGap(4.f).SetMainAxisSize(kui::MainAxisSize::eMin)));
                };
                using Axis = Session::Axis;
                std::vector<kui::Widget> lines;
                lines.push_back(PopupHeading("Look along"));
                lines.push_back(line({ button("Top", "From above, straight on", false, [this] { _session->LookAlong(Axis::eTop); }),
                                       button("Front", "From the front (+Z), straight on", false, [this] { _session->LookAlong(Axis::eFront); }),
                                       button("Right", "From the right (+X), straight on", false, [this] { _session->LookAlong(Axis::eRight); }) }));
                lines.push_back(line({ button("Bottom", "From below, straight on", false, [this] { _session->LookAlong(Axis::eBottom); }),
                                       button("Back", "From the back (-Z), straight on", false, [this] { _session->LookAlong(Axis::eBack); }),
                                       button("Left", "From the left (-X), straight on", false, [this] { _session->LookAlong(Axis::eLeft); }) }));
                lines.push_back(PopupHeading("Projection"));
                const bool straight = _session->camera.orthographic;
                lines.push_back(line({ button("Perspective", "Nearer things bigger", !straight, [this] { _session->SetOrthographic(false); }, true, 92.f),
                                       button("Orthographic", "Seen straight on: a plan, an elevation", straight, [this] { _session->SetOrthographic(true); }, true, 92.f) }));
                lines.push_back(PopupHeading("Bookmarks: a digit goes to one, Ctrl and a digit saves it"));
                std::vector<kui::Widget> go, save;
                for (int slot = 0; slot < 9; ++slot) {
                    const bool has = _session->HasBookmark(slot);
                    go.push_back(button(std::to_string(slot + 1), has ? std::format("Go to bookmark {}", slot + 1) : "None here yet", false,
                                        [this, slot] { _session->GoToBookmark(slot); }, has, 22.f));
                    save.push_back(button("+", std::format("Save the view as bookmark {}", slot + 1), false, [this, slot] { _session->SaveBookmark(slot); }, true, 22.f));
                }
                lines.push_back(line(std::move(go)));
                lines.push_back(line(std::move(save)));
                return kui::Column({
                    Tool(straight ? "crop_square" : "visibility", straight ? "Seen straight on: which way the view looks, and its bookmarks" : "Which way the view looks, and its bookmarks",
                         _viewsOpen, [this] { SetState([&] { const bool open = !_viewsOpen; ClosePopups(); _viewsOpen = open; }); }),
                    kui::PopupAnchor(_viewsOpen, PopupPanel(kui::Column(std::move(lines), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)), 300.f),
                                     [this] { SetState([&] { _viewsOpen = false; }); }, { 0.f, 4.f }),
                }, kui::FlexOptions {}.SetMainAxisSize(kui::MainAxisSize::eMin));
            }

            /** Every one of the Scene panel's popups shut: one is open at a time. */
            void ClosePopups() { _viewModesOpen = _gridOpen = _snapOpen = _viewsOpen = false; }
            std::string _shaderPicking;     // the slot of a pipeline whose list of shaders is open: `<node>/<slot>`

            /** A heading over a group of lines in one of the Scene panel's popups. */
            static kui::Widget PopupHeading(const std::string& text)
            {
                return kui::Padding(kui::EdgeInsets::Only(8.f, 8.f, 8.f, 2.f), kui::Text(text, kui::TextStyle {}.SetSize(11.f).SetColor(kui::Theme::Current().textMuted)));
            }
            /** A popup's backing: the theme's surface, edged. */
            static kui::Widget PopupPanel(kui::Widget body, const float width)
            {
                const kui::Theme t = kui::Theme::Current();
                return kui::Container(kui::ContainerOptions {}.SetWidth(width).SetPadding(kui::EdgeInsets::All(4.f))
                                          .SetDecoration(kui::Decoration {}.SetColor(t.surface).SetRadius(6.f).SetBorderWidth(1.f).SetBorderColor(t.border)),
                                      std::move(body));
            }

            /**
             * How the Scene panel shows the scene — lit, a channel of its materials, one of the editor's ways, a
             * DebugView of its graph — as a button naming the one it is, and under it the list of them by heading.
             */
            kui::Widget ViewModeTool()
            {
                const auto modes = _session->ViewModes();
                const auto now = std::ranges::find(modes, _session->CurrentViewMode(), &Session::ViewModeChoice::mode);
                const std::string name = now == modes.end() ? "Lit" : now->name;
                std::vector<kui::Widget> lines;
                std::string group = "-";
                for (const auto& choice : modes) {
                    if (choice.group != group) {
                        group = choice.group;
                        if (!group.empty()) lines.push_back(PopupHeading(group));
                    }
                    const bool chosen = choice.mode == _session->CurrentViewMode();
                    std::string label = choice.name;
                    std::string tip;
                    if (choice.mode.kind == Session::ViewMode::Kind::eMaterial) tip = "What the shaders that import koral_debug show of their materials; the rest are drawn lit";
                    else if (choice.mode.kind == Session::ViewMode::Kind::eEditor) tip = std::format("A DebugView of {}", choice.mode.file.filename().string());
                    else if (choice.mode.kind == Session::ViewMode::Kind::eGraph) tip = std::format("The scene graph's DebugView '{}'", choice.mode.id);
                    else tip = "The scene as its graph draws it";
                    kui::Widget line = kui::Button(kui::Row({ kui::SizedBox(16.f, 16.f, chosen ? kui::Icon("check", kui::IconStyle::eFilled, kui::Theme::Current().text) : kui::Text("")),
                                                              kui::Text(std::move(label), kui::TextStyle {}.SetSize(13.f)) },
                                                            kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)),
                                                   [this, mode = choice.mode] { _session->SetViewMode(mode); SetState([&] { _viewModesOpen = false; }); },
                                                   kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain));
                    lines.push_back(kui::Tooltip(std::move(tip), std::move(line)));
                }
                if (_session->CurrentViewMode().kind == Session::ViewMode::Kind::eMaterial)
                    if (const auto without = _session->WithoutMaterialViews(); !without.empty())
                        lines.push_back(kui::Padding(kui::EdgeInsets::Only(8.f, 8.f, 6.f, 4.f), kui::Text(std::format("Lit still: {} — its shaders do not import koral_debug.",
                            [&] { std::string all; for (const auto& id : without) all += (all.empty() ? "" : ", ") + id; return all; }()),
                            kui::TextStyle {}.SetSize(11.f).SetColor(kui::Theme::Current().textMuted))));
                const kui::Color ink = kui::Color::Hex(0xE0E0E0);
                // (As wide as what it says: centred in a box of no width, it would be as wide as the line it is in.)
                kui::Widget face = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(4.f, 4.f))
                                                      .SetDecoration(kui::Decoration {}.SetColor(_viewModesOpen ? kui::Theme::Current().primary.WithAlpha(0.35f) : kui::colors::Transparent).SetRadius(3.f)),
                    kui::Row({ kui::SizedBox(16.f, 16.f, kui::Icon("layers", kui::IconStyle::eFilled, ink)), kui::Text(name, kui::TextStyle {}.SetSize(12.f).SetColor(ink)),
                               kui::SizedBox(14.f, 14.f, kui::Icon("expand_more", kui::IconStyle::eFilled, ink)) },
                             kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter).SetMainAxisSize(kui::MainAxisSize::eMin)));
                return kui::Column({
                    kui::Tooltip("How the scene is shown here: lit, a channel of its materials, or a debug view", kui::Button(std::move(face), [this] { SetState([&] { const bool open = !_viewModesOpen; ClosePopups(); _viewModesOpen = open; }); },
                                                                                                                    kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain))),
                    kui::PopupAnchor(_viewModesOpen, PopupPanel(kui::Column(std::move(lines), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)), 220.f), [this] { SetState([&] { _viewModesOpen = false; }); }, { 0.f, 4.f }),
                }, kui::FlexOptions {}.SetMainAxisSize(kui::MainAxisSize::eMin));
            }

            /** The grid: on or off, and beside it its settings — which plane, how far apart its lines, how far it reaches. */
            kui::Widget GridTool()
            {
                const Grid grid = _session->SceneGrid();
                const auto change = [this](std::function<void(Grid&)> how) { Grid grid = _session->SceneGrid(); how(grid); _session->SetGrid(grid); };
                const auto row = [](const char* label, kui::Widget control) {
                    return kui::Padding(kui::EdgeInsets::Symmetric(8.f, 3.f), kui::Row({ kui::SizedBox(96.f, 22.f, kui::Align(kui::Alignment::CenterLeft(), kui::Text(label, kui::TextStyle {}.SetSize(12.f)))),
                                                                                        kui::Expanded(std::move(control)) }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                };
                std::vector<kui::Widget> lines;
                lines.push_back(PopupHeading("Grid"));
                lines.push_back(row("Shown", kui::Switch(grid.shown, [change](const bool on) { change([&](Grid& g) { g.shown = on; }); })));
                // Buttons, not a Dropdown: its list would be a popup of its own, in place of this one.
                std::vector<kui::Widget> planes;
                for (const auto& [plane, name, what] : { std::tuple { Grid::Plane::eXZ, "XZ", "The floor: Y up" }, std::tuple { Grid::Plane::eXY, "XY", "Facing Z" },
                                                         std::tuple { Grid::Plane::eYZ, "YZ", "Facing X" } }) {
                    const bool on = grid.plane == plane;
                    planes.push_back(kui::Tooltip(what, kui::Button(
                        kui::Container(kui::ContainerOptions {}.SetSize(36.f, 22.f).SetAlignment(kui::Alignment::Center())
                                           .SetDecoration(kui::Decoration {}.SetColor(on ? kui::Theme::Current().primary.WithAlpha(0.35f) : kui::Theme::Current().surfaceHover).SetRadius(4.f)),
                                       kui::Text(name, kui::TextStyle {}.SetSize(12.f))),
                        [change, plane] { change([&](Grid& g) { g.plane = plane; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain))));
                }
                lines.push_back(row("Plane", kui::Row(std::move(planes), kui::FlexOptions {}.SetGap(4.f).SetMainAxisSize(kui::MainAxisSize::eMin))));
                lines.push_back(row("Cell (m)", kui::DragValue(grid.cell, [change](const double v) { change([&](Grid& g) { g.cell = static_cast<float>(v); }); },
                                                                kui::DragValueOptions {}.SetRange(0.001f, 1000.f).SetLogarithmic(true))));
                lines.push_back(row("Major every", kui::DragValue(grid.major, [change](const double v) { change([&](Grid& g) { g.major = static_cast<float>(v); }); },
                                                                   kui::DragValueOptions {}.SetRange(1.f, 100.f).SetSpeed(0.1f).SetType(kui::NumberType::eUInt))));
                lines.push_back(row("Fades at (m)", kui::DragValue(grid.fade, [change](const double v) { change([&](Grid& g) { g.fade = static_cast<float>(v); }); },
                                                                    kui::DragValueOptions {}.SetRange(1.f, 10000.f).SetLogarithmic(true))));
                lines.push_back(row("Opacity", kui::Slider(grid.opacity, [change](const float v) { change([&](Grid& g) { g.opacity = v; }); })));
                lines.push_back(row("Behind things", kui::Checkbox(grid.depthTested, [change](const bool on) { change([&](Grid& g) { g.depthTested = on; }); })));
                return kui::Row({
                    Tool("grid_on", grid.shown ? "Hide the grid" : "Show the grid", grid.shown, [change] { change([](Grid& g) { g.shown = !g.shown; }); }),
                    kui::Column({
                        Tool("tune", "The grid's settings", _gridOpen, [this] { SetState([&] { const bool open = !_gridOpen; ClosePopups(); _gridOpen = open; }); }),
                        kui::PopupAnchor(_gridOpen, PopupPanel(kui::Column(std::move(lines), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)), 250.f), [this] { SetState([&] { _gridOpen = false; }); }, { 0.f, 4.f }),
                    }, kui::FlexOptions {}.SetMainAxisSize(kui::MainAxisSize::eMin)),
                }, kui::FlexOptions {}.SetGap(2.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetMainAxisSize(kui::MainAxisSize::eMin));
            }

            // ---- menus and dialogs ------------------------------------------------------------------------------

            kui::Widget Menus()
            {
                const kecs::Entity selected = Doc().World().Alive(_session->selectedEntity) ? _session->selectedEntity : kecs::NullEntity;
                const bool some = !selected.IsNull(), playMode = _session->InPlayMode();
                const auto needsProject = [this](std::function<void()> then) {
                    return [this, then = std::move(then)] { if (!_session->HasProject()) AskForNewProject(); else then(); };
                };
                // What there is to make: each object, at the top and — with something picked — under it.
                std::vector<kui::MenuItem> create = ObjectMenu(kecs::NullEntity);
                if (some) {
                    create.push_back({ .separator = true });
                    create.push_back({ .label = std::format("Under {}", Doc().NameOf(selected)), .items = ObjectMenu(selected) });
                }
                // What the project imports, each to stop importing.
                std::vector<kui::MenuItem> imported;
                for (const auto& reference : _session->CurrentProject().modules)
                    imported.push_back({ .label = reference, .onSelected = [this, reference] { _session->ForgetModule(reference); SetState(); } });
                const auto ticked = [](const bool on, const char* label) { return std::string(on ? "✓  " : "     ") + label; };
                // The project's scenes, each to open: the one open ticked.
                std::vector<kui::MenuItem> scenes;
                for (const auto& scene : _session->CurrentProject().scenes)
                    scenes.push_back({ .label = ticked(scene == _session->SceneName(), scene.c_str()), .onSelected = [this, scene] { _session->OpenScene(scene); SetState(); } });
                return kui::MenuBar({
                    { "File", {
                        { "New project...", Asking("Making another project", [this] { AskForNewProject(); }) },
                        { "Open project...", Asking("Opening another project", [this] { SetState([&] { _dialog = Dialog::eOpenProject; }); }) },
                        { "Import a module...", [this] { SetState([&] { _dialog = Dialog::eImportModule; }); }, _session->HasProject() && !playMode },
                        { .label = "Stop importing", .enabled = !imported.empty() && !playMode, .items = std::move(imported) },
                        { .separator = true },
                        { .label = "Open scene", .enabled = scenes.size() > 1 && !playMode, .items = std::move(scenes) },
                        { "Add a scene...", needsProject([this] { SetState([&] { _dialog = Dialog::eNewScene; _newId.clear(); }); }), !playMode },
                        { "New scene", Asking("Making the scene anew", needsProject([this] { _session->NewStandardScene(); })), !playMode },
                        { "New lit scene", Asking("Making the scene anew, lit under a sky", needsProject([this] { _session->NewLitScene(); })), !playMode },
                        { "Revert to saved", Asking("Putting the saved scene back", [this] { _session->Revert(); }), _session->HasProject() && !playMode },
                        { "Save scene", needsProject([this] { _session->Save(); }), !playMode },
                    } },
                    { "Edit", {
                        { "Undo   Ctrl+Z", [this] { _session->Undo(); }, _session->CanUndo() && !playMode },
                        { "Redo   Ctrl+Y", [this] { _session->Redo(); }, _session->CanRedo() && !playMode },
                        { .separator = true },
                        { "Duplicate", [this, selected] { _session->Duplicate(selected); }, some },
                        { "Move to the top", [this, selected] { Doc().SetParent(selected, kecs::NullEntity); }, some && !Doc().World().ParentOf(selected).IsNull() },
                        { "Save as object", [this, selected] { _session->SaveAsObject(selected); }, some },
                        { "Save as prefab", [this, selected] { _session->SaveAsPrefab(selected); }, some },
                        { .separator = true },
                        { "Delete", [this, selected] { _session->Delete(selected); }, some },
                    } },
                    { "Create", std::move(create) },
                    { "View", {
                        { "Frame the selection", [this, selected] { _session->Frame(selected); }, some },
                        { ticked(_session->ThroughSceneCamera(), "Look through the scene's camera"), [this] { _session->SetLookThroughCamera(!_session->lookThroughCamera); } },
                        { .separator = true },
                        { ticked(!_session->gizmoEnabled, "Select only"), [this] { _session->SetGizmo(false, _session->gizmo); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eTranslate, "Move"), [this] { _session->SetGizmo(true, kor::GizmoMode::eTranslate); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eRotate, "Rotate"), [this] { _session->SetGizmo(true, kor::GizmoMode::eRotate); } },
                        { ticked(_session->gizmoEnabled && _session->gizmo == kor::GizmoMode::eScale, "Scale"), [this] { _session->SetGizmo(true, kor::GizmoMode::eScale); } },
                        { .separator = true },
                        { "Put the panels back where they start", [this] { SetState([&] { _layout = StartingLayout(); }); SaveLayout(); } },
                    } },
                    { "Assets", {
                        { "Import a model or a picture...", [this] { SetState([&] { _dialog = Dialog::eImportModel; }); } },
                        { "New shader...", needsProject([this] { SetState([&] { _dialog = Dialog::eNewShader; _newId.clear(); _newKind = 0; }); }) },
                        { "New script...", needsProject([this] { SetState([&] { _dialog = Dialog::eNewScript; _newId.clear(); _newKind = 0; }); }) },
                        { .separator = true },
                        { "Compress textures...", needsProject([this] { _assets.OpenCompressTextures(); }) },
                    } },
                    { "Scene", {
                        _session->IsPlaying() ? kui::MenuItem { "Pause", [this] { _session->Pause(); } }
                                              : kui::MenuItem { playMode ? "Resume" : "Play", [this] { _session->StartPlaying(); } },
                        { "Stop", [this] { _session->Stop(); }, playMode },
                    } },
                    { "Project", {
                        { "Run", needsProject([this] { _session->Run(); }) },
                        { ticked(_session->HasProject() && Same(Session::StartupProject(), _session->CurrentProject().directory), "Open at startup"),
                          [this] { ToggleStartup(); }, _session->HasProject() },
                        { "Open in Visual Studio Code", needsProject([this] { _session->OpenInCode(); }) },
                        { "Export the game...", needsProject([this] {
                            SetState([&] { _dialog = Dialog::eExportGame; _newId = (_session->CurrentProject().directory / "build" / _session->CurrentProject().name).generic_string(); });
                        }) },
                    } },
                    { "Graph", {
                        { "Add node...   Ctrl+Space", [this] { AskForNode(NewNodeAt()); } },
                        { "Paste   Ctrl+V", [this] { const kor::Vec2 at = NewNodeAt(); _session->PasteNodes(at.x, at.y); }, _session->HasCopiedNodes() },
                        { "Duplicate   Ctrl+D", [this] { if (!_session->selectedNode.empty()) _session->DuplicateNodes({ _session->selectedNode }); }, !_session->selectedNode.empty() },
                        { "Arrange this page", [this] { Arrange(); } },
                        { "Arrange every page", [this] { _session->ArrangeEveryPage(); } },
                        { "Frame everything   F", [this] { _view->Frame(); SetState(); } },
                    } },
                    { "Settings", {
                        { .label = "Theme", .items = ThemeMenu() },
                    } },
                    { "Entity", {
                        { "New entity", [this] { _session->Select(Doc().CreateEntity()); } },
                        { "New tag...", [this] { SetState([&] { _dialog = Dialog::eNone; _session->Say("Give the tag a name in the inspector of an entity."); }); } },
                    } },
                });
            }

            /** Each design the editor can be drawn in, and dark or light: the one it is in ticked. */
            std::vector<kui::MenuItem> ThemeMenu()
            {
                const kui::Theme& now = kui::Theme::Current();
                const auto ticked = [](const bool on, const char* label) { return std::string(on ? "✓  " : "     ") + label; };
                const bool dark = now.IsDark();
                std::vector<kui::MenuItem> items;
                for (const auto& [label, design] : { std::pair { "Koral", kui::ThemeDesign::eKoral }, std::pair { "Material", kui::ThemeDesign::eMaterial },
                                                     std::pair { "Cupertino", kui::ThemeDesign::eCupertino }, std::pair { "Windows", kui::ThemeDesign::eFluent } })
                    items.push_back({ ticked(now.design == design, label), [this, design, dark] { _session->ChooseTheme(design, dark); } });
                items.push_back({ .separator = true });
                items.push_back({ ticked(dark, "Dark"), [this, design = now.design] { _session->ChooseTheme(design, true); } });
                items.push_back({ ticked(!dark, "Light"), [this, design = now.design] { _session->ChooseTheme(design, false); } });
                return items;
            }

            kui::Widget DialogBody()
            {
                switch (_dialog) {
                case Dialog::eCompressTextures: return _assets.CompressTexturesDialog();
                case Dialog::eAddNode: return AddNodeDialog();
                case Dialog::ePortChoice: return PortChoiceDialog();
                case Dialog::eNewProject: return NewProjectDialog();
                case Dialog::eNewProjectFolder:
                    // Where the new one is made: back to its form, with the folder picked.
                    return kgui::FileBrowser({ .start = _newProject.folder, .directories = true, .createDirectories = true, .confirm = "Make it in here" },
                        [this](const std::filesystem::path& chosen) { SetState([&] { _newProject.folder = chosen.string(); _dialog = Dialog::eNewProject; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNewProject; }); });
                case Dialog::eNewProjectModule:
                    return kgui::FileBrowser({ .start = _newProject.folder, .directories = true, .confirm = "Import this module" },
                        [this](const std::filesystem::path& chosen) { PickModuleForNewProject(chosen); },
                        [this] { SetState([&] { _dialog = Dialog::eNewProject; }); });
                case Dialog::eImportModule: return ImportModuleDialog();
                case Dialog::eImportModuleFolder:
                    return kgui::FileBrowser({ .start = _session->CurrentProject().directory.parent_path(), .directories = true, .confirm = "Import this module" },
                        [this](const std::filesystem::path& chosen) { _session->ImportModule(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eImportModule; }); });
                case Dialog::eOpenProject:
                    return kgui::FileBrowser({ .start = Session::ProjectsDirectory(), .directories = true, .confirm = "Open" },
                        [this](const std::filesystem::path& chosen) { _session->OpenProject(chosen); SetState([&] { _dialog = Dialog::eNone; }); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eConfirm: return ConfirmDialog();
                case Dialog::eImportOptions: return _assets.ImportOptionsDialog();
                case Dialog::eNewFolder: return _assets.NewFolderDialog();
                case Dialog::eNewScene: return _assets.NewSceneDialog();
                case Dialog::eNewPrefab: return _assets.NewPrefabDialog();
                case Dialog::eTrash: return _assets.TrashDialog();
                case Dialog::eMoveAsset: return _assets.MoveAssetDialog();
                case Dialog::eExportGame: return _assets.ExportGameDialog();
                case Dialog::eImportModel:
                    // Into the project, as one of its assets: into the scene only when it is dragged there.
                    return kgui::FileBrowser({ .confirm = "Import" },
                        [this](const std::filesystem::path& chosen) { SetState([&] { _dialog = Dialog::eNone; }); _session->ImportAsset(chosen); },
                        [this] { SetState([&] { _dialog = Dialog::eNone; }); });
                case Dialog::eNewShader: return NewFileDialog("A new shader", "Its name: the module a pipeline names", Session::ShaderKinds(), true);
                case Dialog::eNewScript: return NewFileDialog("A new script", "Its name: the class, and the component", _session->ScriptKindsHere(), false);
                case Dialog::eNewFunction: return FunctionDialog();
                default: return {};
                }
            }

            /** The name of a function to make of the nodes picked: they become it, and one node in their place calls it. */
            kui::Widget FunctionDialog()
            {
                const auto make = [this] {
                    if (!_session->MakeFunction(_functionOf, _newId).empty()) SetState([&] { _dialog = Dialog::eNone; });
                    else SetState();
                };
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(std::format("A function of {} nodes", _functionOf.size())));
                rows.push_back(kgui::Line("They become a function of the project's — functions/<name>.kfunc — and one node calls it where they were. Each wire into them "
                                          "is an input of it, each wire out of them an output, and the call is wired as they were.",
                                          kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(12.f)));
                rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name: Instances",
                                                                      .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                      .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(); }, .width = 420.f }));
                if (_session->Failed()) rows.push_back(Problem(_session->Status()));
                rows.push_back(kui::Row({ kui::Button("Make it", make), kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                        kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(460.f));
            }

            /** A kind of shader as the New shader dialog says it. */
            static std::string ShaderKindLabel(const std::string& kind)
            {
                return kind == "Vertex" ? "Vertex + fragment" : kind == "MeshShading" ? "Mesh + fragment" : kind == "RayTracing" ? "Ray tracing" : kind;
            }
            /** What a shader of @p kind can have besides what each of its kind has: a tick each, or why this device cannot run it. */
            std::vector<kui::Widget> ShaderOptions(const std::string& kind)
            {
                std::vector<kui::Widget> ticks;
                const auto tick = [&](const char* option, const char* label, const char* what, bool Session::ShaderChoice::* field) {
                    if (const std::string why = Session::ShaderMissing(kind, option); !why.empty()) {
                        ticks.push_back(kgui::Muted(std::format("{}: {}", label, why), 12.f));
                        return;
                    }
                    ticks.push_back(kui::Tooltip(what, kui::Checkbox(_shaderChoice.*field, [this, field](const bool on) { SetState([&] { _shaderChoice.*field = on; }); }, label)));
                };
                using Choice = Session::ShaderChoice;
                if (kind == "Vertex") {
                    tick("wholePicture", "Over the whole picture", "One triangle over the whole picture, and no mesh: a pass that works on another picture, which its fragment shader reads.", &Choice::wholePicture);
                    tick("tessellation", "Tessellation", "Tessellation control and evaluation shaders: each triangle cut finer, as the pipeline's `constant.level` says.", &Choice::tessellation);
                    tick("geometry", "Geometry shader", "A geometry shader: each triangle passed on, or more made of it.", &Choice::geometry);
                    tick("fragment", "Fragment shader", "Without one, only depth is drawn: a shadow map, a depth prepass.", &Choice::fragment);
                } else if (kind == "MeshShading") {
                    tick("task", "Task shader", "A task shader: how many mesh shader groups there are, and what each is handed.", &Choice::task);
                    tick("fragment", "Fragment shader", "Without one, only depth is drawn: a shadow map, a depth prepass.", &Choice::fragment);
                } else if (kind == "RayTracing") {
                    tick("anyHit", "Any hit shader", "An any hit shader in the hit group: every hit, not only the nearest — and a ray let pass through what is cut out.", &Choice::anyHit);
                    tick("intersection", "Intersection shader", "An intersection shader in the hit group: for what is no triangles, a sphere in each of an acceleration structure's boxes.", &Choice::intersection);
                }
                return ticks;
            }

            /** A name and one of a few kinds, for a file to be made from: a shader, or a script. */
            kui::Widget NewFileDialog(const char* title, const char* placeholder, const std::vector<std::pair<std::string, std::string>>& kinds, const bool shader)
            {
                if (_newKind >= static_cast<int>(kinds.size())) _newKind = 0;
                const auto make = [this, kinds, shader] {
                    const std::string& kind = kinds[static_cast<std::size_t>(_newKind)].first;
                    if (shader ? _session->NewShader(_newId, kind, _shaderChoice) : _session->NewScript(_newId, kind)) SetState([&] { _dialog = Dialog::eNone; });
                    else SetState();
                };
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(title));
                rows.push_back(kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = placeholder,
                                                                      .onChanged = [this](const std::string& typed) { _newId = typed; },
                                                                      .onSubmitted = [this, make](const std::string& typed) { _newId = typed; make(); }, .width = 420.f }));
                for (std::size_t i = 0; i < kinds.size(); ++i) {
                    const std::string& kind = kinds[i].first;
                    const std::string missing = shader ? Session::ShaderMissing(kind) : std::string();
                    rows.push_back(kui::RadioButton(static_cast<int>(i) == _newKind, [this, i] { SetState([&] { _newKind = static_cast<int>(i); }); }, shader ? ShaderKindLabel(kind) : kind));
                    rows.push_back(kui::Padding(kui::EdgeInsets { 28.f, 0.f, 0.f, 4.f }, kgui::Line(missing.empty() ? kinds[i].second : kinds[i].second + " " + missing,
                        kui::TextStyle {}.SetColor(missing.empty() ? kui::Theme::Current().textMuted : kui::Color::Hex(0xE5484D)).SetSize(12.f))));
                    // What a shader of the kind picked has besides: each a tick, or why this device cannot run it.
                    if (shader && static_cast<int>(i) == _newKind && missing.empty())
                        for (auto& option : ShaderOptions(kind)) rows.push_back(kui::Padding(kui::EdgeInsets { 28.f, 0.f, 0.f, 6.f }, std::move(option)));
                }
                if (_session->Failed()) rows.push_back(Problem(_session->Status()));
                rows.push_back(kui::Row({ kui::Button("Make it", make), kui::Button("Cancel", [this] { SetState([&] { _dialog = Dialog::eNone; }); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary)) },
                                        kui::FlexOptions {}.SetGap(8.f)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(8.f)).Width(shader ? 620.f : 460.f));
            }

            void AddOf(const std::string& type)
            {
                const std::string id = Doc().AddNode(type, _addAt.x, _addAt.y, _newId, ShownPass());
                _session->Select(id);
                _session->Say(std::format("Added {}.", id));
                if (_wire) WireTo(id);
                SetState([&] { _dialog = Dialog::eNone; });
            }

            /** A wire let go over nothing: where it came from, and what it carries. */
            struct LooseWire {
                kui::PortRef from;
                bool fromOutput = true;
                std::string type;
            };

            /** Where a node asked for from a menu or a key goes: the middle of what the graph shows, each a little off the last. */
            kor::Vec2 NewNodeAt()
            {
                const float off = 24.f * static_cast<float>(_made++ % 6);
                return { std::round(_view->centre.x - 90.f + off), std::round(_view->centre.y - 60.f + off) };
            }

            /** Opens the list of nodes there are to add, for one at @p at — wired to @p wire, when it comes of a wire let go. */
            void AskForNode(const kor::Vec2 at, std::optional<LooseWire> wire = std::nullopt)
            {
                SetState([&] { _dialog = Dialog::eAddNode; _addAt = at; _filter.clear(); _newId.clear(); _wire = std::move(wire); });
            }

            /** Opens the list of @p node's ports that @p wire could go to, let go on it. */
            void AskForPort(LooseWire wire, const std::string& node)
            {
                SetState([&] { _dialog = Dialog::ePortChoice; _filter.clear(); _wire = std::move(wire); _wireOnto = node; });
            }

            /** Wires the wire let go to @p port of the node it was let go on, and closes the list. */
            void WireToPort(const std::string& port)
            {
                const LooseWire wire = *std::exchange(_wire, std::nullopt);
                if (wire.fromOutput) Doc().Connect(wire.from.node, wire.from.port, _wireOnto, port);
                else Doc().Connect(_wireOnto, port, wire.from.node, wire.from.port);
                SetState([&] { _dialog = Dialog::eNone; });
            }

            /**
             * The ports of the node a wire was let go on, for it to go to: those that fit, the first ready for Enter, and
             * under them, greyed, those that don't, each with why. Typing narrows both.
             */
            kui::Widget PortChoiceDialog()
            {
                if (!_wire) return kui::SizedBox(0.f, 0.f);
                const LooseWire& wire = *_wire;
                std::vector<PortChoice> choices = PortChoices(Doc(), wire.from.node, wire.from.port, wire.fromOutput, _wireOnto);
                const auto lowered = [](std::string text) {
                    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return text;
                };
                const std::string typed = lowered(_filter);
                std::erase_if(choices, [&](const PortChoice& choice) { return !typed.empty() && lowered(choice.label).find(typed) == std::string::npos && lowered(choice.port).find(typed) == std::string::npos; });
                const auto first = std::ranges::find_if(choices, &PortChoice::fits);

                std::vector<kui::Widget> rows;
                bool greyed = false;
                for (const auto& choice : choices) {
                    if (!choice.fits) {
                        if (!greyed) { greyed = true; rows.push_back(kgui::Muted("Can't take it", 11.f)); }
                        rows.push_back(kgui::Muted(std::format("{}   ·   {}", choice.label, choice.why), 12.f));
                        continue;
                    }
                    const std::string text = choice.wiredFrom.empty() ? choice.label : std::format("{}   ·   now from {}", choice.label, choice.wiredFrom);
                    rows.push_back(kui::Selectable(text, &choice == &*first, [this, port = choice.port] { WireToPort(port); }));
                }
                if (choices.empty()) rows.push_back(kgui::Muted(_filter.empty() ? "It has no ports." : std::format("No port of it is called '{}'.", _filter)));
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading(wire.fromOutput ? std::format("Wire {} of {} to {}", PortLabel(wire.from.port), wire.from.node, _wireOnto)
                                            : std::format("Wire {} of {} from {}", PortLabel(wire.from.port), wire.from.node, _wireOnto)),
                    kui::TextField(kui::TextFieldOptions { .text = _filter, .placeholder = "Type part of a port's name; Enter wires the first",
                                                           .onChanged = [this](const std::string& text) { SetState([&] { _filter = text; }); },
                                                           .onSubmitted = [this, port = first == choices.end() ? std::string() : first->port](const std::string&) {
                                                               if (!port.empty()) WireToPort(port);
                                                           }, .width = 420.f, .focus = 1 }),
                    kui::SizedBox(420.f, 360.f, kui::ScrollView(kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(2.f)))),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(12.f)));
            }

            /** Whether a node of kind @p type has somewhere for @p wire to go. */
            static bool Takes(const kgraph::NodeType& type, const LooseWire& wire)
            {
                const bool value = wire.type.starts_with("v:");
                // A value goes into anything that has a setting: what is offered is what works on values, and what has one of its kind.
                if (value && wire.fromOutput && (type.category == "Values" || type.category == "Flow")) return true;
                for (const auto& port : wire.fromOutput ? type.inputs : type.outputs) {
                    const std::string kind = WireType(port);
                    if (value ? kind == wire.type : wire.fromOutput ? WireFits(wire.type, kind) : WireFits(kind, wire.type)) return true;
                }
                return false;
            }

            /** Wires the node just made to the wire it was made for: the first of its ports the wire fits, one of its own kind before any other. */
            void WireTo(const std::string& id)
            {
                const LooseWire wire = *std::exchange(_wire, std::nullopt);
                const kgraph::Node* node = Doc().Graph().Find(id);
                if (!node) return;
                std::string best;
                int bestScore = -1;
                const auto consider = [&](const kgraph::Port& port, const bool fits) {
                    if (!fits) return;
                    const int score = (WireType(port) == wire.type ? 2 : 0) + (port.optional ? 0 : 1);
                    if (score > bestScore) { bestScore = score; best = port.name; }
                };
                if (wire.fromOutput) {
                    std::vector<kgraph::Port> more;
                    for (const auto& shown : InputsOf(Doc().Runtime(), *node, [](const std::string&) { return false; }, &more)) consider(shown.port, WireFits(wire.type, WireType(shown.port)));
                    for (const auto& port : more) if (port.name.find("[]") == std::string::npos) consider(port, WireFits(wire.type, WireType(port)));
                    if (!best.empty()) Doc().Connect(wire.from.node, wire.from.port, id, best);
                } else {
                    for (const auto& port : Doc().Runtime().OutputsOf(id)) consider(port, WireFits(WireType(port), wire.type));
                    if (!best.empty()) Doc().Connect(id, best, wire.from.node, wire.from.port);
                }
                if (best.empty()) _session->Say(std::format("Added {}. Nothing of it takes that wire: it is not wired.", id), true);
            }

            /** What a Reference stands for, however many References away; the node itself, when it is not one. */
            const kgraph::Node* StandsFor(const kgraph::Node& node)
            {
                const kgraph::Node* at = &node;
                for (int hops = 0; at && at->type == "Reference" && hops < 8; ++hops) {
                    const auto target = at->inputs.find("node");
                    at = target != at->inputs.end() && target->second.value.String() ? Doc().Graph().Find(*target->second.value.String()) : nullptr;
                }
                return at;
            }

            /** What a node holds, in a few words: an image's size and format, a buffer's size. Its kind otherwise. */
            std::string Holds(const std::string& id)
            {
                const kgraph::Runtime& runtime = Doc().Runtime();
                const auto bare = [](std::string name) { return name.size() > 1 && name[0] == 'e' && std::isupper(static_cast<unsigned char>(name[1])) ? name.substr(1) : name; };
                if (const auto image = runtime.ImageOf(id); image.Get()) {
                    const auto extent = image->Extent();
                    std::string text = extent.z > 1 ? std::format("{}×{}×{}", extent.x, extent.y, extent.z) : std::format("{}×{}", extent.x, extent.y);
                    if (const Value format = runtime.ValueOf(id, "format"); format.String()) text += "  ·  " + bare(*format.String());
                    if (image->MipLevels() > 1) text += std::format("  ·  {} levels", image->MipLevels());
                    if (image->ArrayLayers() > 1) text += std::format("  ·  {} layers", image->ArrayLayers());
                    if (image->IsPerFrame()) text += "  ·  per frame";
                    return text;
                }
                const auto held = runtime.ResourceOf(id);
                if (!held) return {};
                if (held->screen) return "the window";
                if (held->kind == "Buffer" || held->kind == "BufferView") {
                    // Kept: a number is read from inside the value it is of.
                    const Value size = runtime.ValueOf(id, "bytes"), count = runtime.ValueOf(id, "count");
                    std::string text = held->kind;
                    if (const double* bytes = size.Number())
                        text += *bytes >= 1024.0 * 1024.0 ? std::format("  ·  {:.1f} MiB", *bytes / (1024.0 * 1024.0))
                              : *bytes >= 1024.0 ? std::format("  ·  {:.1f} KiB", *bytes / 1024.0) : std::format("  ·  {} bytes", static_cast<std::uint64_t>(*bytes));
                    if (const double* elements = count.Number()) text += std::format("  ·  {} elements", static_cast<std::uint64_t>(*elements));
                    return text;
                }
                return held->kind;
            }

            /** How a page uses what a node made: which pass nodes wrote it before the page begins, and what the page does with it. */
            struct UseHere {
                std::vector<std::string> before;    // the pass nodes that wrote it on earlier pages, in order, each once
                bool read = false, write = false;   // what this page's own do with it
            };
            UseHere UsesAt(const std::string& id, const std::string& page)
            {
                UseHere here;
                const auto& passes = Doc().Passes();
                const auto at = std::ranges::find(passes, page);
                for (const auto& use : Doc().Runtime().UsesOf(id)) {
                    if (use.pass == page) { here.read = here.read || use.read; here.write = here.write || use.write; continue; }
                    const auto theirs = std::ranges::find(passes, use.pass);
                    if (!use.write || theirs == passes.end() || theirs >= at) continue;
                    // A node of a loop runs as its turns, `down [0]`: it is said once, by its own name.
                    const std::string name = use.node.ends_with(']') && use.node.contains(" [") ? use.node.substr(0, use.node.rfind(" [")) : use.node;
                    if (!std::ranges::contains(here.before, name)) here.before.push_back(name);
                }
                return here;
            }

            /** What wrote a node's resource before a page, said in a line. */
            static std::string WrittenBefore(const UseHere& here)
            {
                if (here.before.empty()) return "Nothing writes it before this page.";
                std::string names;
                for (std::size_t i = 0; i < here.before.size() && i < 3; ++i) names += (i ? ", " : "") + here.before[i];
                if (here.before.size() > 3) names += std::format(" and {} more", here.before.size() - 3);
                return "After " + names + ".";
            }

            /** How a page uses it, said as a few words. */
            static std::string UsedHere(const UseHere& here)
            {
                return here.read && here.write ? "read and written here" : here.write ? "written here" : here.read ? "read here" : "not used here yet";
            }

            /** Puts the page's nodes in order, left to right by what is wired to what. */
            void Arrange()
            {
                // As the page is shaped: as wide, for how tall, as the panel it is in.
                const float aspect = _view->size.y > 1.f ? std::clamp(_view->size.x / _view->size.y, 0.4f, 4.f) : 1.6f;
                // Each as big as its card came out, where it has been laid out: what the page was built with is a guess.
                std::vector<PlacedNode> placed = _placed;
                for (auto& node : placed)
                    if (const auto size = _view->sizes.find(node.id); size != _view->sizes.end() && size->second.x > 0.f) {
                        node.width = size->second.x;
                        node.height = size->second.y;
                    }
                const auto where = ArrangeNodes(placed, _wires, aspect);
                if (where.empty()) return;
                // Another page's node shown here is where it is on this page only — the editor's, not the document's: kept
                // as this page has it, where it was dragged to from there forgotten.
                std::map<std::string, std::pair<double, double>> own;
                for (const auto& [id, at] : where)
                    if (_placedOutside.contains(id)) {
                        _outsideArranged[_placedPage][id] = kor::Vec2(static_cast<float>(at.first), static_cast<float>(at.second));
                        _outsideMoved[_placedPage].erase(id);
                    } else {
                        own.emplace(id, at);
                    }
                if (_overview) Doc().PlaceInOverview(own);
                else Doc().PlaceNodes(own);
                _view->Frame();
                _session->Say("The page's nodes are in the order they are wired in. Ctrl+Z puts them back.");
                SetState();
            }

            /**
             * The nodes there are to add. With nothing typed, every kind, under its category. As something is typed,
             * the kinds it finds, best first, each with what it does — and the nodes of this graph it names, to go to.
             * Enter adds the first.
             */
            kui::Widget AddNodeDialog()
            {
                auto found = FindNodeTypes(_filter, Doc().Presets());
                // For a wire let go over nothing: only what has somewhere for it to go.
                if (_wire) std::erase_if(found, [&](const kgraph::NodeType* type) { return !Takes(*type, *_wire); });
                // The categories in three sections: the functions first, each under what it is for; then what the scene
                // holds and what is worked out of it; then Vulkan's own — what is made on the device, and what runs.
                std::map<std::string, std::size_t> counts;
                for (const auto* type : found) ++counts[type->category];
                const std::vector<std::string> sections = NodeListSections(counts, Doc().RateOf(ShownPass()));
                std::vector<std::string> categories;
                for (const auto& section : sections) if (!section.starts_with('#')) categories.push_back(section);
                const std::string category = std::ranges::contains(categories, _menuCategory) ? _menuCategory : std::string();

                // Left: every category, with how many it has of what is found, under its section.
                std::vector<kui::Widget> left;
                const auto categoryRow = [&](const std::string& name, const std::string& label, const std::size_t count) {
                    return kui::Selectable(std::format("{}   {}", label, count), category == name, [this, name] { SetState([&] { _menuCategory = name; }); });
                };
                left.push_back(categoryRow({}, _filter.empty() ? "Everything" : "All that is found", found.size()));
                for (const auto& name : sections)
                    if (name.starts_with('#')) left.push_back(kui::Padding(kui::EdgeInsets::Only(0.f, 8.f, 0.f, 2.f), kgui::Muted(name.substr(1), 11.f)));
                    else left.push_back(kui::Row({ kui::Container(kui::ContainerOptions { .width = 4.f, .height = 18.f, .decoration = { .color = CategoryColor(name), .radius = 2.f } }),
                                                   kui::Expanded(categoryRow(name, name, counts[name])) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));

                // Middle: the nodes of the category — or, as something is typed, what it finds, best first.
                std::vector<const kgraph::NodeType*> listed;
                for (const auto* type : found) if (category.empty() || type->category == category) listed.push_back(type);
                if (_filter.empty() && category.empty())
                    std::ranges::stable_sort(listed, [&](const auto* a, const auto* b) {
                        const auto at = [&](const std::string& c) { const auto it = std::ranges::find(categories, c); return it - categories.begin(); };
                        return at(a->category) < at(b->category);
                    });
                const std::string preview = std::ranges::any_of(listed, [&](const auto* t) { return t->name == _menuPreview; }) ? _menuPreview
                                          : listed.empty() ? std::string() : listed.front()->name;
                std::vector<kui::Widget> middle;
                std::string under;
                for (std::size_t i = 0; i < listed.size(); ++i) {
                    const auto* type = listed[i];
                    if (category.empty() && _filter.empty() && type->category != under) { under = type->category; middle.push_back(kgui::Muted(under, 11.f)); }
                    const std::string first = type->doc.substr(0, std::min(type->doc.find(". "), type->doc.size()));
                    middle.push_back(kui::GestureDetector(kui::GestureOptions {}.OnEnter([this, name = type->name] { if (_menuPreview != name) SetState([&] { _menuPreview = name; }); }), kui::Column({
                            // Found among every kind, a node says which kind it is.
                            kui::Selectable(_filter.empty() || !category.empty() ? type->name : std::format("{}   ·   {}", type->name, type->category),
                                            type->name == preview && !_filter.empty() && i == 0, [this, name = type->name] { AddOf(name); }),
                            kui::Padding(kui::EdgeInsets { 12.f, 0.f, 0.f, 4.f }, kgui::Line(first, kui::TextStyle {}.SetColor(kui::Theme::Current().textMuted).SetSize(11.f))),
                        }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(0.f))));
                }
                if (middle.empty()) middle.push_back(kgui::Muted(std::format("Nothing is called, or does, '{}'.", _filter)));
                // The nodes this graph has, by name: picked, to go on working with.
                if (!_filter.empty()) {
                    std::string lowered = _filter;
                    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    std::size_t here = 0;
                    for (const auto& node : Doc().Graph().nodes) {
                        std::string id = node.id;
                        std::ranges::transform(id, id.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (id.find(lowered) == std::string::npos || ++here > 6) continue;
                        if (here == 1) middle.push_back(kgui::Muted("In this graph", 11.f));
                        middle.push_back(kui::Selectable(std::format("{}   ·   {}", node.id, node.type), false, [this, picked = node.id] {
                            _session->GoTo(picked);
                            SetState([&] { _dialog = Dialog::eNone; });
                        }));
                    }
                }

                // Right: the node pointed at, as it will be on the graph, and what it is.
                std::vector<kui::Widget> right;
                const auto* shown = std::ranges::find_if(listed, [&](const auto* t) { return t->name == preview; }) != listed.end()
                    ? *std::ranges::find_if(listed, [&](const auto* t) { return t->name == preview; }) : nullptr;
                if (shown) {
                    kui::NodeGraph one;
                    one.nodes.push_back(PreviewCard(*shown));
                    auto view = std::make_shared<kui::NodeEditorView>();
                    view->page = "preview " + shown->name;
                    view->Frame();
                    right.push_back(kui::Container(kui::ContainerOptions { .height = 230.f, .decoration = { .color = kui::Theme::Current().background, .radius = 6.f } },
                                                   kui::IgnorePointer(kui::NodeEditor(std::move(one), kui::NodeEditorOptions {}.SetView(view).SetGridSize(20.f)))));
                    right.push_back(kui::Row({ kui::Container(kui::ContainerOptions { .width = 4.f, .height = 16.f, .decoration = { .color = CategoryColor(shown->category), .radius = 2.f } }),
                                               Heading(shown->name), kgui::Muted(shown->category, 11.f) }, kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                    right.push_back(kui::Text(shown->doc, kui::TextStyle {}.SetSize(12.f)));
                    const auto names = [&](const std::vector<kgraph::Port>& ports, const bool outputs) {
                        std::string text;
                        for (const auto& port : ports)
                            if (!port.optional || outputs) text += (text.empty() ? "" : ", ") + (outputs && port.name == "out" ? TypeLabel(port.type) : PortLabel(port.name));
                        return text;
                    };
                    if (const std::string in = names(shown->inputs, false); !in.empty()) right.push_back(kgui::Labeled("Takes", kgui::Line(in, kui::TextStyle {}.SetSize(11.f)), 70.f));
                    if (const std::string out = names(shown->outputs, true); !out.empty()) right.push_back(kgui::Labeled("Gives", kgui::Line(out, kui::TextStyle {}.SetSize(11.f)), 70.f));
                    right.push_back(Small(std::format("Add {}", shown->name), [this, name = shown->name] { AddOf(name); }));
                }

                const auto column = [](std::vector<kui::Widget> children, const float width, const float gap) {
                    return kui::SizedBox(width, 460.f, kui::ScrollView(kui::Column(std::move(children), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(gap))));
                };
                return kui::Padding(kui::EdgeInsets::All(16.f), kui::Column({
                    Heading(_wire ? (_wire->fromOutput ? std::format("Add a node, wired from {} of {}", PortLabel(_wire->from.port), _wire->from.node)
                                                       : std::format("Add a node, wired to {} of {}", PortLabel(_wire->from.port), _wire->from.node))
                                  : std::string("Add a node")),
                    kui::Row({
                        kui::TextField(kui::TextFieldOptions { .text = _filter, .placeholder = "Type what you are after: image, blur, clamp, light...",
                                                               .onChanged = [this](const std::string& typed) { SetState([&] { _filter = typed; }); },
                                                               .onSubmitted = [this](const std::string& typed) {
                                                                   auto first = FindNodeTypes(typed, Doc().Presets());
                                                                   if (_wire) std::erase_if(first, [&](const kgraph::NodeType* type) { return !Takes(*type, *_wire); });
                                                                   if (!_menuCategory.empty()) std::erase_if(first, [&](const kgraph::NodeType* type) { return type->category != _menuCategory; });
                                                                   if (!typed.empty() && !first.empty()) AddOf(first.front()->name);
                                                               }, .width = 420.f, .focus = 1 }),
                        kui::TextField(kui::TextFieldOptions { .text = _newId, .placeholder = "Its name (a pipeline's names its component)",
                                                               .onChanged = [this](const std::string& typed) { _newId = typed; }, .width = 320.f }),
                    }, kui::FlexOptions {}.SetGap(8.f)),
                    kui::Row({ column(std::move(left), 180.f, 2.f), column(std::move(middle), 330.f, 0.f),
                               kui::SizedBox(380.f, 460.f, kui::Column(std::move(right), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(8.f))) },
                             kui::FlexOptions {}.SetGap(14.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart)),
                }, kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(12.f)));
            }

            /**
             * A node of a type as it is when it is made: what the menu shows of a node before it is added. Its inputs
             * that are set on the node, with their controls as they start; the rest it cannot do without, as ports.
             */
            kui::GraphNode PreviewCard(const kgraph::NodeType& type)
            {
                kgraph::Node node;
                node.id = "preview";
                node.type = type.name;
                kui::GraphNode card;
                card.id = "preview";
                card.title = type.name;
                card.icon = type.icon;
                card.accent = CategoryColor(type.category);
                card.level = static_cast<int>(type.level);
                card.composite = type.composite;
                for (const auto& port : type.inputs) {
                    if (port.name.find("[]") != std::string::npos) {
                        if (port.optional) continue;
                    }
                    const kgraph::Placement placement = kgraph::PlacementOf(port, node);
                    const bool asPort = port.kind == kgraph::PortKind::eResource || placement != kgraph::Placement::eInspector;
                    if (!asPort || (port.optional && placement != kgraph::Placement::eNode)) continue;
                    kgraph::Port shown = port;
                    if (const auto open = shown.name.find("[]"); open != std::string::npos) shown.name.insert(open + 1, "0");
                    kui::NodePort row { shown.name, PortLabel(shown.name), WireType(shown) };
                    row.color = PortColor(row.type); row.shape = PortShapeOf(row.type);
                    if (placement == kgraph::Placement::eNode) row.control = NodeControl(node, shown, shown.initial);
                    card.inputs.push_back(std::move(row));
                }
                for (const auto& port : type.outputs)
                    if (!port.optional) {
                        kui::NodePort row { port.name, port.name == "out" ? TypeLabel(port.type) : PortLabel(port.name), WireType(port) };
                        row.color = PortColor(row.type); row.shape = PortShapeOf(row.type);
                        card.outputs.push_back(std::move(row));
                    }
                std::vector<kui::Widget> body { kgui::Muted(type.name, 11.f) };
                if (type.name == "VertexLayout") {
                    const auto initial = std::ranges::find(type.inputs, "attributes", &kgraph::Port::name);
                    if (initial != type.inputs.end()) body.push_back(LayoutPicture(AttributesOf(initial->initial)));
                }
                card.body = kui::Column(std::move(body), kui::FlexOptions {}.SetGap(6.f));
                return card;
            }

            // ---- the graph --------------------------------------------------------------------------------------

            // ---- what the lists down the graph's sides are made of ---------------------------------------------

            /**
             * A few words in a pill at a line's end: how many, how many wrong, that it is here already. Every pill is the
             * panel's own grey — what matters more is said brighter (@p ink), never in a colour of its own.
             */
            static kui::Widget ListPill(std::string text, const kui::Color& ink)
            {
                const kui::Theme& t = kui::Theme::Current();
                return kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Symmetric(6.f, 1.f)).SetDecoration(kui::Decoration {}.SetColor(t.text.WithAlpha(0.07f)).SetRadius(8.f)),
                                      kui::Text(std::move(text), kui::TextStyle {}.SetSize(11.f).SetColor(ink), kui::TextAlign::eStart, false));
            }
            /** A line of a list: picked, it is lit and marked down its left; under the pointer, lit a little. */
            kui::Widget ListLine(const std::string& key, const bool picked, std::vector<kui::Widget> line, std::function<void()> onTap)
            {
                const kui::Theme& t = kui::Theme::Current();
                const bool over = _passHover == key;
                kui::Widget row = kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::Only(10.f, 0.f, 6.f, 0.f))
                                                     .SetDecoration(kui::Decoration {}.SetColor(picked ? t.primary.WithAlpha(0.16f) : over ? t.surfaceHover : kui::colors::Transparent).SetRadius(5.f)),
                    kui::SizedBox(-1.f, 28.f, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(8.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
                if (picked)
                    row = kui::Stack({ std::move(row), kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 6.f, .bottom = 6.f, .width = 3.f },
                        kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary).SetRadius(2.f))))) });
                return kui::GestureDetector(kui::GestureOptions {}.OnTap(std::move(onTap))
                                                .OnEnter([this, key] { if (_passHover != key) SetState([&] { _passHover = key; }); })
                                                .OnExit([this, key] { if (_passHover == key) SetState([&] { _passHover.clear(); }); }), std::move(row));
            }
            static std::string Upper(std::string text)
            {
                std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
                return text;
            }
            static kui::Widget ListIcon(const std::string_view name, const kui::Color& tint) { return kui::SizedBox(15.f, 15.f, kui::Icon(name, kui::IconStyle::eFilled, tint)); }
            static kui::Widget ListName(std::string text, const bool picked)
            {
                const kui::Theme& t = kui::Theme::Current();
                return kui::Expanded(kui::Text(std::move(text), kui::TextStyle {}.SetSize(13.f).SetColor(picked ? t.text : t.text.WithAlpha(0.86f)), kui::TextAlign::eStart, false, 1, true));
            }
            /** What a side list is in: the panel's surface, its lines down it, as wide as it is. */
            static kui::Widget SideList(std::vector<kui::Widget> rows)
            {
                return kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(kui::Theme::Current().surface)),
                    kui::ScrollView(kui::Padding(kui::EdgeInsets::Symmetric(10.f, 10.f),
                        kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(3.f)))));
            }

            /** The pass whose page is shown: none when every node is, or when the graph has no passes. */
            std::string ShownPass()
            {
                const auto& passes = Doc().Passes();
                if (_session->ShowsEveryPass() || passes.empty()) return {};
                if (std::ranges::find(passes, _session->Pass()) == passes.end()) _session->ShowPass(passes.front());
                return _session->Pass();
            }

            /**
             * The passes a frame is made in, in the order they are drawn: pressed, a pass's page is shown. Under them,
             * for the one shown: something to make in it, and what the passes before it made, to bring in.
             */
            kui::Widget PassList()
            {
                const kui::Theme& t = kui::Theme::Current();
                const auto passes = Doc().Passes();
                const std::string shown = ShownPass();
                const kgraph::Runtime& runtime = Doc().Runtime();
                // The side lists' own parts: a heading, a pill, a line, its icon and its name — the drawer beside the page has them too.
                const auto section = [&](std::string title, kui::Widget trailing = {}) { return ListHeading(std::move(title), std::move(trailing)); };
                const auto chip = [](std::string text, const kui::Color& tint) { return ListPill(std::move(text), tint); };
                const auto entry = [&](const std::string& key, const bool picked, std::vector<kui::Widget> line, std::function<void()> onTap) {
                    return ListLine(key, picked, std::move(line), std::move(onTap));
                };
                const auto icon = [](const std::string_view name, const kui::Color& tint) { return ListIcon(name, tint); };
                const auto name = [](std::string text, const bool picked) { return ListName(std::move(text), picked); };

                std::vector<kui::Widget> rows;
                rows.push_back(kui::TextField(kui::TextFieldOptions { .placeholder = "Find a node",
                    .onSubmitted = [this](const std::string& typed) {
                        if (typed.empty()) return;
                        const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                        const std::string wanted = lower(typed);
                        // The one so named, or else the first whose name or kind has it: after the one that is picked, so that Enter again goes on to the next.
                        const auto& nodes = Doc().Graph().nodes;
                        const auto picked = std::ranges::find(nodes, _session->selectedNode, &kgraph::Node::id);
                        const std::size_t from = picked == nodes.end() ? 0 : static_cast<std::size_t>(picked - nodes.begin()) + 1;
                        for (std::size_t i = 0; i < nodes.size(); ++i) {
                            const kgraph::Node& node = nodes[(from + i) % nodes.size()];
                            if (lower(node.id).find(wanted) == std::string::npos && lower(node.type).find(wanted) == std::string::npos) continue;
                            _session->GoTo(node.id);
                            _session->Say(std::format("Found '{}'{}.", node.id, node.pass.empty() ? std::string() : std::format(", in {}", node.pass)));
                            return;
                        }
                        _session->Say(std::format("No node's name or kind has '{}' in it.", typed), true);
                    } }).Key("find node"));

                // The whole frame, and every node at once: the two ways of seeing more than a page.
                const bool atFrame = _session->ShowsFrame();
                rows.push_back(kui::Tooltip("The whole frame: every page as a card, in a lane for how often it runs, wired by what each uses of the others.",
                    entry("@frame", atFrame, { icon("account_tree", atFrame ? t.primary : t.textMuted), name("Frame", atFrame),
                                               chip(std::to_string(passes.size()), t.textMuted) }, [this] { _session->ShowFrame(); })));
                const bool everyNode = !atFrame && shown.empty();
                rows.push_back(kui::Tooltip("Every node of every pass, on one page laid out by itself: moving a node here does not move it on its pass's page.",
                    entry("*", everyNode, { icon("grid_view", everyNode ? t.primary : t.textMuted), name("Every node at once", everyNode),
                                            chip(std::to_string(Doc().Graph().nodes.size()), t.textMuted) }, [this] { _session->ShowEveryPass(); })));

                // The pages by how often they run, a lane each as the frame has them: setup, the fixed step, every frame.
                // Under the page shown, what runs on it: its passes and its functions' calls.
                // (Dragged up or down, a page runs earlier or later among its lane's: until the order is the data's alone.)
                using Rate = kgraph::PassRate;
                const auto unchosen = _session->PagesNotChosen();
                std::size_t leftOut = 0;
                bool anyUnchosen = false;
                int offset = 0;
                for (const Rate rate : { Rate::eSetup, Rate::eFixed, Rate::eFrame }) {
                    std::vector<std::string> listed;
                    for (const auto& pass : passes) if (Doc().RateOf(pass) == rate) listed.push_back(pass);
                    const std::string laneKey = std::format("lane {}", static_cast<int>(rate));
                    const bool open = !_lanesShut.contains(laneKey);
                    const char* title = rate == Rate::eSetup ? "SETUP  ·  MADE ONCE" : rate == Rate::eFixed ? "FIXED STEP" : "EVERY FRAME";
                    rows.push_back(section(listed.empty() ? std::string(title) : std::format("{}  ·  {}", title, listed.size()), kui::Row({
                        kui::Tooltip(rate == Rate::eSetup ? "A new setup page, made after the rest of setup" : rate == Rate::eFixed ? "A new page of the fixed step, run after the rest of it" : "A new page, drawn after the rest",
                                     SmallIcon("add", [this, rate] { SetState([&] { _addingPass = true; _addingRate = rate; ++_passFocus; _lanesShut.erase(std::format("lane {}", static_cast<int>(rate))); }); })),
                        kui::Tooltip(open ? "Fold it" : "Unfold it", SmallIcon(open ? "expand_less" : "expand_more", [this, laneKey, open] { SetState([&] { if (open) _lanesShut.insert(laneKey); else _lanesShut.erase(laneKey); }); })) },
                        kui::FlexOptions {}.SetGap(2.f))));
                    const int laneOffset = offset;
                    offset += static_cast<int>(listed.size());
                    if (!open) continue;
                    if (listed.empty() && !(_addingPass && _addingRate == rate))
                        rows.push_back(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted(rate == Rate::eSetup
                            ? "Nothing is made once yet: a noise texture, a table worked out on the GPU, a buffer cleared to start with. + makes a page."
                            : rate == Rate::eFixed ? "Nothing runs on the fixed step yet: a simulation that must not go faster or slower with the frame rate. + makes a page."
                                                   : "None yet: every node is on one page. + makes the first.", 11.f)));
                    for (std::size_t i = 0; i < listed.size(); ++i) {
                        const std::string& pass = listed[i];
                        const bool notChosen = unchosen.contains(pass);
                        anyUnchosen = anyUnchosen || notChosen;
                        if (notChosen && !_everyChoice && pass != shown) { ++leftOut; continue; }
                        std::size_t nodes = 0, failed = 0;
                        for (const auto& node : Doc().Graph().nodes) {
                            if (node.pass != pass) continue;
                            ++nodes;
                            if (const auto* status = runtime.Status(node.id); status && !status->ok) ++failed;
                        }
                        const bool picked = !atFrame && pass == shown;
                        const int index = laneOffset + static_cast<int>(i);   // among them all: what the document moves it by
                        std::vector<kui::Widget> line;
                        line.push_back(kui::SizedBox(18.f, -1.f, kui::Text(std::to_string(i + 1), kui::TextStyle {}.SetSize(11.f).SetColor(picked ? t.primary : t.textMuted), kui::TextAlign::eEnd, false)));
                        line.push_back(notChosen ? kui::Tooltip("Of what the choices do not choose now: its passes idle", kui::Opacity(0.5f, name(pass, picked))) : name(pass, picked));
                        if (failed) line.push_back(kui::Tooltip(std::format("{} of its nodes could not be made", failed), chip(std::format("{} wrong", failed), t.text)));
                        // What the page's passes cost the GPU together, coloured by their share of the frame.
                        line.push_back(kui::Tooltip("What its passes take of the GPU together, averaged over recent frames", kui::Make<LiveValue>(_session, [this, pass] {
                            const Timings timings = Doc().Runtime().Timings();
                            double ms = 0.0;
                            bool measured = false;
                            for (const auto& node : Doc().Graph().nodes)
                                if (node.pass == pass)
                                    if (const auto cost = CostOf(timings, node.id)) { ms += cost->gpuMs; measured = measured || cost->gpuMeasured; }
                            const double frame = FrameGpuMs(timings);
                            return measured ? Value(Value::List { Value(ms), Value(frame > 0.0 ? ms / frame : 0.0) }) : Value();
                        }, [](const Value& said) -> kui::Widget {
                            const auto* both = said.Items();
                            if (!both || both->size() != 2) return kui::SizedBox(0.f, 0.f);
                            return ListPill(std::format("{:.2f} ms", *(*both)[0].Number()), HeatColor(*(*both)[1].Number()));
                        })));
                        line.push_back(chip(std::to_string(nodes), t.textMuted));
                        std::vector<kui::MenuItem> menu = OrderItems(pass);
                        menu.push_back(MenuLine());
                        menu.push_back({ "Run it every frame", [this, pass] { if (Doc().SetPassRate(pass, Rate::eFrame)) _session->Say(std::format("{} is done every frame now.", pass)); },
                                         rate != Rate::eFrame });
                        menu.push_back({ "Run it on the fixed step", [this, pass] {
                            if (Doc().SetPassRate(pass, Rate::eFixed))
                                _session->Say(std::format("{} runs on the fixed step now: on frames with a step in them, while the scene plays.", pass));
                        }, rate != Rate::eFixed });
                        menu.push_back({ "Make it once, as setup", [this, pass] {
                            if (Doc().SetPassRate(pass, Rate::eSetup))
                                _session->Say(std::format("{} is setup now: made when it is built, and again only when what it uses is.", pass));
                        }, rate != Rate::eSetup });
                        menu.push_back(MenuLine());
                        menu.push_back({ nodes ? std::format("Delete, with its {} nodes", nodes) : std::string("Delete"), [this, pass] { Doc().RemovePass(pass); } });
                        menu.push_back({ "Delete the pass, keep its nodes", [this, pass] { Doc().RemovePass(pass, true); }, nodes > 0 });
                        // In the order it runs, which what it uses and its links say: not moved by hand.
                        kui::Widget row = kui::ContextMenu(std::move(menu),
                            kui::Tooltip(std::format("{}: {} nodes. Pressed, its page is shown; right-click to say what it runs after, and more.", pass, nodes),
                                         entry("pass " + pass, picked, std::move(line), [this, pass] { _session->ShowPass(pass); })));
                        rows.push_back(std::move(row));
                        // The page shown: what runs on it, a line each, gone to when pressed.
                        if (picked)
                            for (const auto& node : Doc().Graph().nodes) {
                                if (node.pass != pass) continue;
                                const kgraph::NodeType* type = kgraph::FindType(node.type);
                                const bool call = node.type == "Function";
                                if (!call && !(type && type->category == "Passes")) continue;
                                const bool here = node.id == _session->selectedNode;
                                std::string what = node.type;
                                if (const auto function = node.inputs.find("function"); call && function != node.inputs.end())
                                    if (const auto* text = function->second.value.String()) what = *text;
                                rows.push_back(kui::Padding(kui::EdgeInsets::Only(22.f, 0.f, 0.f, 0.f),
                                    entry("run " + node.id, here, { icon(call ? "functions" : type && !type->icon.empty() ? std::string_view(type->icon) : std::string_view("bolt"), call ? t.primary : t.textMuted),
                                                                    name(node.title.empty() ? node.id : node.title, here), chip(what, t.textMuted) },
                                          [this, id = node.id] { _session->GoTo(id); })));
                            }
                    }
                    if (_addingPass && _addingRate == rate)
                        rows.push_back(kui::TextField(kui::TextFieldOptions { .placeholder = "Its name, then Enter",
                            .onSubmitted = [this, rate](const std::string& typed) {
                                if (typed.empty()) { SetState([&] { _addingPass = false; }); return; }
                                if (Doc().AddPass(typed, -1, rate)) { _session->ShowPass(typed); SetState([&] { _addingPass = false; }); }
                                else _session->Say(std::format("There is a pass called '{}' already.", typed), true);
                            }, .focus = _passFocus, .onEscape = [this] { SetState([&] { _addingPass = false; }); } }).Key(std::format("new pass {} {}", static_cast<int>(rate), passes.size())));
                }
                if (leftOut > 0 || (_everyChoice && anyUnchosen))
                    rows.push_back(kui::Tooltip("Pages whose passes all idle as the choices are — another renderer's, say — are left out of the list. Choose another in the frame's Switch, or the scene view's switch, and its pages are listed.",
                        kui::Row({ kui::Expanded(kgui::Muted(_everyChoice ? std::string("Listing what the choices do not choose too") : std::format("{} {} of what the choices do not choose", leftOut, leftOut == 1 ? "page" : "pages"), 11.f)),
                                   Chip(_everyChoice ? "Hide them" : "Show them", [this] { SetState([&] { _everyChoice = !_everyChoice; }); }) },
                                 kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));

                // A function's page: what every call of it takes and gives, in order, each to rename, retype or move.
                if (!shown.empty() && !atFrame)
                    if (const auto* opened = Doc().OpenedOn(shown))
                        for (auto& row : SignatureRows(*opened)) rows.push_back(std::move(row));
                if (!shown.empty() && !atFrame) {
                    rows.push_back(section("THIS PASS"));
                    rows.push_back(kui::Tooltip("Its name: typed over, then Enter",
                        kui::TextField(kui::TextFieldOptions { .text = shown,
                            .onSubmitted = [this, shown](const std::string& typed) { if (typed != shown && Doc().RenamePass(shown, typed)) _session->ShowPass(typed); } }).Key("rename " + shown)));
                    // Something to make in it: the kinds of thing a pass has of its own.
                    std::vector<std::string> kinds;
                    for (const auto& type : kgraph::Catalog())
                        if (type.listed && (type.category == "Resources" || type.category == "Pipelines" || type.category == "Passes" || type.category == "Scene" || type.category == "Flow") && type.name != "Reference")
                            kinds.push_back(type.name);
                    rows.push_back(kui::Tooltip("Makes a node in this pass. Right-click the graph, or Control+Space, for every kind there is.",
                        kui::Dropdown(kinds, -1, [this, kinds, shown](const int index) {
                            const kor::Vec2 at = NewNodeAt();
                            _session->Select(Doc().AddNode(kinds[static_cast<std::size_t>(index)], at.x, at.y, {}, shown));
                        }, kui::DropdownOptions {}.SetPlaceholder("Create a node"))));
                }
                // As wide as its panel, whatever is in it: what is longer ends in an ellipsis, not past the edge.
                return SideList(std::move(rows));
            }

            /**
             * The graph, at the depth it is looked at: the whole frame — its pages as cards in a lane for each rate, wired by
             * what each uses — or a page gone into, or the function opened on it. Where it is, over it, a step each to go back to.
             */
            kui::Widget Graph()
            {
                return kui::Column({ GraphBar(), kui::Separator(), kui::Expanded(_session->ShowsFrame() ? FlowPanel() : GraphPage()) },
                                   kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
            }

            /**
             * Over the graph: where it is — the frame, the page, the function opened on it — each step pressed to go back to;
             * and at its end what is done where it is.
             */
            kui::Widget GraphBar()
            {
                const kui::Theme& t = kui::Theme::Current();
                const bool atFrame = _session->ShowsFrame();
                const std::string page = atFrame ? std::string() : ShownPass();
                const auto* opened = page.empty() ? nullptr : Doc().OpenedOn(page);
                const bool inFunction = opened != nullptr;
                std::vector<kui::Widget> steps;
                const auto step = [&](const std::string_view icon, std::string label, const bool last, std::function<void()> go) {
                    kui::Widget face = kui::Padding(kui::EdgeInsets::Symmetric(5.f, 3.f), kui::Row({
                        kui::SizedBox(14.f, 14.f, kui::Icon(icon, kui::IconStyle::eFilled, last ? t.primary : t.textMuted)),
                        kui::Text(std::move(label), kui::TextStyle {}.SetSize(12.f).SetColor(last ? t.text : t.textMuted), kui::TextAlign::eStart, false, 1, true) },
                        kui::FlexOptions {}.SetGap(5.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                    if (!steps.empty()) steps.push_back(kgui::Muted("›", 13.f));
                    steps.push_back(last ? std::move(face) : kui::Button(std::move(face), std::move(go), kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)));
                };
                step("account_tree", "Frame", atFrame, [this] { _session->ShowFrame(); });
                if (!atFrame) {
                    if (page.empty()) step("grid_view", "Every node at once", true, {});
                    else if (inFunction) {
                        if (!opened->call.pass.empty()) step("layers", opened->call.pass, false, [this, back = opened->call.pass] { _session->ShowPass(back); });
                        step("functions", std::format("{}  ·  every call", opened->function), true, {});
                    } else step(Doc().RateOf(page) == kgraph::PassRate::eSetup ? "build" : Doc().RateOf(page) == kgraph::PassRate::eFixed ? "timer" : "layers", page, true, {});
                }
                steps.push_back(kui::Expanded(kui::Text("")));
                if (inFunction) {
                    // A function open in place: what is done on its page is the function's, kept or put back as it was.
                    // Written as it is done: closed, the call is put back; discarded, the function is as it was when it was opened.
                    steps.push_back(kui::Tooltip("Closes it: the call is put back, the function as its page is (written as it was done)", Chip("Close", [this] { _session->CloseFunction(true); })));
                    steps.push_back(kui::Tooltip("Puts the function back as it was when it was opened, and every call with it", Chip("Discard", [this] { _session->CloseFunction(false); })));
                } else if (atFrame) {
                    steps.push_back(kui::Tooltip("Every node of every page, on one page laid out by itself", Chip("Every node", [this] { _session->ShowEveryPass(); })));
                    steps.push_back(kui::Tooltip("Every card where the flow puts it, what was dragged forgotten", SmallIcon("auto_fix_high", [this] { ArrangeFlow(); })));
                    steps.push_back(kui::Tooltip("Frame everything", SmallIcon("fit_screen", [this] { _flowView->Frame(); SetState(); })));
                } else if (!page.empty())
                    steps.push_back(kui::Tooltip("Back to the whole frame (the page stays where it was left)", SmallIcon("zoom_out_map", [this] { _session->ShowFrame(); })));
                return kui::Padding(kui::EdgeInsets { 6.f, 3.f, 6.f, 3.f }, kui::SizedBox(-1.f, 26.f,
                    kui::Row(std::move(steps), kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter))));
            }

            /** A name for a new page, of none there is: `page`, `page 2`... */
            std::string FreePageName() const
            {
                const auto& passes = const_cast<Root*>(this)->Doc().Passes();
                std::string name = "page";
                for (int n = 2; std::ranges::contains(passes, name); ++n) name = std::format("page {}", n);
                return name;
            }

            /**
             * What a page runs after and before, as menus of the other pages of its lane: each pressed is a link made — or,
             * marked with a dot, taken away. Those it ties with (both write something, and nothing orders them) first.
             */
            std::vector<kui::MenuItem> OrderItems(const std::string& page)
            {
                const auto links = _session->Links();
                std::set<std::string> tied;
                for (const auto& tie : _session->Ties())
                    if (tie.first == page || tie.second == page) tied.insert(tie.first == page ? tie.second : tie.first);
                std::vector<kui::MenuItem> after, before;
                std::vector<std::string> others;
                for (const auto& other : Doc().Passes())
                    if (other != page && Doc().RateOf(other) == Doc().RateOf(page)) others.push_back(other);
                std::ranges::stable_partition(others, [&](const std::string& other) { return tied.contains(other); });
                for (const auto& other : others) {
                    const bool runsAfter = std::ranges::contains(links, std::pair { page, other });
                    const bool runsBefore = std::ranges::contains(links, std::pair { other, page });
                    const std::string mark = tied.contains(other) ? "  (tied)" : "";
                    after.push_back({ (runsAfter ? "•  " : "") + other + mark, [this, page, other, runsAfter] {
                        if (runsAfter) Doc().StopRunningAfter(page, other);
                        else if (!Doc().RunAfter(page, other)) _session->Say(std::format("{} runs before {} already, by the links: that would be a ring.", page, other), true);
                    } });
                    before.push_back({ (runsBefore ? "•  " : "") + other + mark, [this, page, other, runsBefore] {
                        if (runsBefore) Doc().StopRunningAfter(other, page);
                        else if (!Doc().RunAfter(other, page)) _session->Say(std::format("{} runs after {} already, by the links: that would be a ring.", page, other), true);
                    } });
                }
                return {
                    { .label = "Run after", .enabled = !after.empty(), .items = std::move(after) },
                    { .label = "Run before", .enabled = !before.empty(), .items = std::move(before) },
                };
            }

            /**
             * The signature of the function opened as @p opened: its inputs, then its outputs, in the order every call has
             * them as ports — each its name to type over, its kind to choose, arrows to move it, and pressed, its edge picked.
             */
            std::vector<kui::Widget> SignatureRows(const Document::OpenedFunction& opened)
            {
                const kui::Theme& t = kui::Theme::Current();
                std::vector<kui::Widget> rows;
                const std::vector<std::string> kinds { "eValue", "eEntities", "eMesh", "eBuffer", "eImage", "eResource" };
                // In the order a call has them: what each says (`order`), then where it is on the page.
                const auto edgesOf = [&](const char* type) {
                    std::vector<const kgraph::Node*> edges;
                    for (const auto& node : Doc().Graph().nodes)
                        if (node.pass == opened.pass && node.type == type) edges.push_back(&node);
                    const auto orderOf = [](const kgraph::Node* edge) {
                        const auto it = edge->inputs.find("order");
                        const double* n = it == edge->inputs.end() || it->second.link ? nullptr : it->second.value.Number();
                        return n ? *n : 1e300;
                    };
                    std::ranges::stable_sort(edges, [&](const kgraph::Node* a, const kgraph::Node* b) {
                        if (orderOf(a) != orderOf(b)) return orderOf(a) < orderOf(b);
                        return a->y != b->y ? a->y < b->y : a->x < b->x;
                    });
                    std::vector<std::string> ids;
                    for (const auto* edge : edges) ids.push_back(edge->id);
                    return ids;
                };
                const auto side = [&](const char* type, const char* title) {
                    const std::vector<std::string> ids = edgesOf(type);
                    const bool input = std::string_view(type) == "FunctionInput";
                    rows.push_back(ListHeading(std::format("{}  ·  {}", title, ids.size()),
                        kui::Tooltip(input ? "A new input: a port more of every call" : "A new output: a port more of every call", SmallIcon("add", [this, type, input, page = opened.pass, count = ids.size()] {
                            const std::string id = Doc().AddNode(type, input ? -400.0 : 900.0, 90.0 * static_cast<double>(count), {}, page);
                            if (id.empty()) return;
                            Doc().SetInput(id, "name", std::format("{} {}", input ? "input" : "output", count + 1));
                            Doc().SetInput(id, "order", static_cast<double>(count));
                            _session->Select(id);
                        }))));
                    // Moved, every edge of its side is given the place it has now: the order is theirs, not the page's.
                    const auto move = [this, ids](const std::size_t from, const std::size_t to) {
                        std::vector<std::string> order = ids;
                        std::swap(order[from], order[to]);
                        for (std::size_t i = 0; i < order.size(); ++i) Doc().SetInput(order[i], "order", static_cast<double>(i));
                    };
                    for (std::size_t i = 0; i < ids.size(); ++i) {
                        const kgraph::Node* edge = Doc().Graph().Find(ids[i]);
                        if (!edge) continue;
                        const auto text = [&](const char* input) { const auto it = edge->inputs.find(input); return it != edge->inputs.end() && it->second.value.String() ? *it->second.value.String() : std::string(); };
                        const std::string portName = text("name").empty() ? edge->id : text("name");
                        const std::string kind = text("kind").empty() ? std::string("eValue") : text("kind");
                        const int kindAt = static_cast<int>(std::ranges::find(kinds, kind) - kinds.begin());
                        const bool picked = _session->selectedNode == edge->id;
                        rows.push_back(kui::Row({
                            kui::Tooltip("Pressed, its edge is picked", SmallIcon(input ? "login" : "logout", [this, id = edge->id] { _session->GoTo(id); })),
                            kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = portName,
                                .onSubmitted = [this, id = edge->id, portName](const std::string& typed) { if (!typed.empty() && typed != portName) Doc().SetInput(id, "name", typed); } }).Key("port " + edge->id)),
                            kui::Dropdown(kinds, std::min<int>(kindAt, static_cast<int>(kinds.size()) - 1), [this, id = edge->id, kinds](const int index) { Doc().SetInput(id, "kind", kinds[static_cast<std::size_t>(index)]); },
                                          kui::DropdownOptions {}.SetWidth(96.f)),
                            kui::Tooltip("Earlier among the ports", SmallIcon("arrow_upward", [move, i] { if (i > 0) move(i, i - 1); })),
                            kui::Tooltip("Later among the ports", SmallIcon("arrow_downward", [move, i, n = ids.size()] { if (i + 1 < n) move(i, i + 1); })),
                        }, kui::FlexOptions {}.SetGap(4.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                        if (picked) rows.back() = kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary.WithAlpha(0.12f)).SetRadius(5.f)), std::move(rows.back()));
                    }
                    if (ids.empty()) rows.push_back(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted(input ? "It takes nothing: + gives it an input." : "It gives nothing: + gives it an output.", 11.f)));
                };
                rows.push_back(ListHeading(std::format("{}  ·  SIGNATURE", Upper(opened.function))));
                side("FunctionInput", "TAKES");
                side("FunctionOutput", "GIVES");
                return rows;
            }

            /** A new page of @p rate, after the rest of its lane, gone into: its name is typed over in the Outline. */
            void NewPage(const kgraph::PassRate rate)
            {
                const std::string name = FreePageName();
                if (!Doc().AddPass(name, -1, rate)) return;
                _session->ShowPass(name);
                _session->Say(std::format("A new page, {}: its name is typed over in the Outline, its nodes made with Control+Space.", name));
            }

            /**
             * What there is to use on page @p page: what the passes before it made — setup's first — and what is in no
             * pass, by the page it is on. Each is dragged onto the page, or pressed, to be brought in: a Reference to it
             * there, wired from as the thing itself. One brought in already says so, and pressed is picked.
             */
            kui::Widget HereToUse()
            {
                const std::string page = _session->ShowsFrame() ? std::string() : ShownPass();
                if (page.empty())
                    return SideList({ kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted(_session->ShowsFrame()
                        ? "The whole frame is shown. Go into a page — press its card twice — and what it can use of the pages before it is here, to drag onto it."
                        : Doc().Passes().empty()
                        ? "What a pass can use of the passes before it is shown here, once the graph has passes."
                        : "Pick a pass's page, in Passes: what it can use of the passes before it is shown here, to drag onto it.", 11.f)) });
                const kui::Theme& t = kui::Theme::Current();
                const kgraph::Graph& graph = Doc().Graph();
                const kgraph::Runtime& runtime = Doc().Runtime();
                const bool setupPage = Doc().IsSetup(page);
                // As the Assets panel shows its things: a picture over a name, or — as a list — a line each.
                constexpr float Picture = 56.f;
                const bool list = _session->drawerList;
                const float size = list ? ListSize : Picture, preview = list ? 18.f : Picture;
                // What is brought in here already, by what it stands for.
                std::map<std::string, std::string> brought;
                for (const auto& node : graph.nodes)
                    if (node.type == "Reference" && node.pass == page)
                        if (const kgraph::Node* target = StandsFor(node); target && target != &node) brought.emplace(target->id, node.id);
                // By the page each is on, in the order the pages are; those in no pass last. Resources, then values.
                std::map<std::string, std::vector<std::string>> resources, values;
                for (const auto& id : Doc().Importable(page)) {
                    const kgraph::Node* node = graph.Find(id);
                    if (!node) continue;
                    (runtime.ResourceOf(id) ? resources : values)[node->pass].push_back(id);
                }
                std::vector<std::string> order(Doc().Passes().begin(), Doc().Passes().end());
                order.emplace_back();

                std::vector<kui::Widget> rows;
                rows.push_back(kui::Tooltip(setupPage ? "What setup made before this page. Drag one onto the page, or press it, to use it here."
                                                      : "What setup made, what the passes before this one drew, and what is in no pass. Drag one onto the page, or press it, to use it here.",
                    ListHeading(std::format("WHAT {} CAN USE", Upper(page)),
                        kui::Tooltip(list ? "Pictures" : "A list", SmallIcon(list ? "grid_view" : "view_list", [this] { _session->SetDrawerList(!_session->drawerList); })))));
                // One thing: its picture — an image as it was last drawn, a mesh by itself, or what kind of thing it is — and its name. The rest is said when the pointer rests on it.
                const auto tile = [&](const std::string& id) {
                    const kgraph::Node* node = graph.Find(id);
                    const auto there = brought.find(id);
                    const bool here = there != brought.end();
                    const kgraph::NodeType* type = kgraph::FindType(node->type);
                    kui::Widget picture;
                    // An image's picture is made anew every few seconds, and the last let go of: drawn as a node's card draws it, found again each time.
                    if (runtime.ImageOf(id).Get() && _session->ThumbnailOf(id).Get()) picture = kui::Make<NodePicture>(_session, id, false, kor::Vec2 { preview, preview }, false);
                    else picture = kui::Make<MeshThumbnail>(_session, [session = _session, id] { return session->NodePicture(id); },
                        [preview, icon = type && !type->icon.empty() ? type->icon : std::string("category"), tint = here ? t.primary : t.textMuted](const kor::ResourceRef<const kor::Image> made) {
                            return kui::SizedBox(preview, preview, made.Get() ? kui::Image(made, kui::ImageFit::eContain) : kui::Icon(icon, kui::IconStyle::eFilled, tint));
                        });
                    const std::string holds = Holds(id);
                    std::string said = holds.empty() || holds == node->type ? node->type : std::format("{}  ·  {}", node->type, holds);
                    if (runtime.ResourceOf(id)) said += "\n" + WrittenBefore(UsesAt(id, page));
                    said += here ? "\nBrought in already: pressed, it is picked." : "\nDrag it onto the page, or press it: a node here that stands for it, to wire from.";
                    const auto bring = [this, id, page] { const kor::Vec2 at = NewNodeAt(); _session->Select(Doc().Import(id, page, at.x, at.y)); };
                    std::vector<kui::MenuItem> menu {
                        here ? kui::MenuItem { "Pick it here", [this, reference = there->second] { _session->Select(reference); } } : kui::MenuItem { "Bring it in", bring },
                        { "Go to where it is made", [this, id] { _session->GoTo(id); } },
                    };
                    kui::Widget made = _assets.Tile("use " + id, std::move(picture), node->title.empty() ? id : node->title, here, false, size,
                                            [this, here, bring, reference = here ? there->second : std::string()] { if (here) _session->Select(reference); else bring(); },
                                            std::move(menu));
                    return kui::Tooltip(std::move(said), kui::Draggable({ "resource", id }, std::move(made)));
                };
                const auto group = [&](const std::map<std::string, std::vector<std::string>>& of) {
                    for (const auto& pass : order) {
                        const auto ids = of.find(pass);
                        if (ids == of.end() || ids->second.empty()) continue;
                        std::string title = pass.empty() ? std::string("IN NO PASS") : Upper(pass);
                        if (Doc().IsSetup(pass)) title += "  ·  SETUP";
                        else if (Doc().RateOf(pass) == kgraph::PassRate::eFixed) title += "  ·  FIXED STEP";
                        rows.push_back(ListHeading(std::move(title), ListPill(std::to_string(ids->second.size()), t.textMuted)));
                        std::vector<kui::Widget> tiles;
                        for (const auto& id : ids->second) tiles.push_back(tile(id));
                        rows.push_back(list ? Rows(std::move(tiles), 1.f) : kui::Wrap(std::move(tiles), 4.f, 4.f));
                    }
                };
                group(resources);
                if (resources.empty()) rows.push_back(kui::Padding(kui::EdgeInsets::Symmetric(4.f, 2.f), kgui::Muted(setupPage ? "Nothing yet: this is the first page of setup." : "Nothing yet: no pass before this one made anything.", 11.f)));
                if (!values.empty()) {
                    const std::size_t count = std::ranges::fold_left(values | std::views::values, std::size_t(0), [](const std::size_t n, const auto& ids) { return n + ids.size(); });
                    rows.push_back(ListHeading(std::format("VALUES  ·  {}", count),
                        kui::Tooltip(_drawerValues ? "Hide them" : "The numbers, vectors and matrices the passes before this one worked out, too",
                                     SmallIcon(_drawerValues ? "expand_less" : "expand_more", [this] { SetState([&] { _drawerValues = !_drawerValues; }); }))));
                    if (_drawerValues) group(values);
                }
                return SideList(std::move(rows));
            }

            kui::Widget GraphPage()
            {
                const kgraph::Graph& graph = Doc().Graph();
                const kgraph::Runtime& runtime = Doc().Runtime();
                const std::string page = ShownPass();
                const bool everything = page.empty();
                // Every node at once, of passes: a layout of its own, not the passes' pages over one another.
                const bool overview = everything && !Doc().Passes().empty();
                Doc().MakeInOverview(overview);
                _overview = overview;
                const auto places = overview ? Doc().OverviewPlaces() : std::map<std::string, std::pair<double, double>> {};
                const auto here = [&](const kgraph::Node& node) { return everything || node.pass == page; };
                kui::NodeGraph shown;
                std::set<std::pair<std::string, std::string>> wiredFrom;
                for (const auto& node : graph.nodes)
                    for (const auto& input : node.inputs | std::views::values)
                        if (input.link) wiredFrom.insert({ input.link->node, input.link->output });
                // What @p of gives — its type's outputs, and an Entities node's fields — as outputs of @p node.
                const auto outputsOf = [&](const kgraph::Node& node, const kgraph::Node& of, kui::GraphNode& card) {
                    for (const auto& output : runtime.OutputsOf(of.id))
                        if (!output.optional || wiredFrom.contains({ node.id, output.name }) || _session->Asked(node.id, "out:" + output.name)) {
                            kui::NodePort port { output.name, output.name == "out" ? TypeLabel(output.type) : PortLabel(output.name), WireType(output) };
                            port.color = PortColor(port.type); port.shape = PortShapeOf(port.type);
                            card.outputs.push_back(std::move(port));
                        }
                };
                // About how big a card comes out, from what is on it: for putting the page in order.
                const auto wide = [](const std::string& text, const float each) { return static_cast<float>(text.size()) * each; };
                const auto measured = [&](const kui::GraphNode& card, const float bodyWidth, const float bodyHeight) {
                    PlacedNode placed { card.id, card.position.x, card.position.y };
                    // A row a port: its outputs, then its inputs.
                    const std::size_t rows = card.inputs.size() + card.outputs.size();
                    placed.width = std::max({ kui::nodes::MinWidth, wide(card.title, 7.6f) + 24.f + (card.icon.empty() ? 0.f : 21.f) + (card.level >= 0 ? 28.f : 0.f), bodyWidth + 16.f });
                    for (const auto& input : card.inputs)
                        placed.width = std::max(placed.width, 20.f + wide(input.label, 6.6f) + (input.control ? 132.f : input.value.empty() ? 0.f : 28.f + wide(input.value, 6.6f)));
                    for (const auto& output : card.outputs) placed.width = std::max(placed.width, 20.f + wide(output.label, 6.6f));
                    placed.height = kui::nodes::TitleHeight + kui::nodes::RowHeight * static_cast<float>(rows) + bodyHeight + 16.f + (card.error.empty() ? 0.f : 44.f);
                    return placed;
                };
                _placed.clear();
                _wires.clear();
                _placedOutside.clear();
                _placedPage = page;
                std::set<std::string> visible;
                // The loops: each Repeat, and with it every node that is there once a turn because of it. And each ForEach,
                // with what is worked out once an element of it.
                std::map<std::string, std::vector<std::string>> loops, eaches;
                const std::function<std::string(const kgraph::Node&, int)> loopOf = [&](const kgraph::Node& node, const int depth) -> std::string {
                    if (node.type == "Repeat") return node.id;
                    if (depth > 24) return {};
                    for (const auto& input : node.inputs | std::views::values)
                        if (input.link)
                            if (const kgraph::Node* from = graph.Find(input.link->node); from && (from->type == "Repeat" || !runtime.TurnsOf(from->id).empty()))
                                if (std::string found = loopOf(*from, depth + 1); !found.empty()) return found;
                    return {};
                };
                const Timings costs = runtime.Timings();   // which nodes have run something, to say what it costs
                for (const auto& node : graph.nodes) {
                    if (!here(node)) continue;
                    visible.insert(node.id);
                    const kgraph::NodeType* type = kgraph::FindType(node.type);
                    kui::GraphNode card;
                    card.id = node.id;
                    card.title = node.title.empty() ? node.id : node.title;
                    // One of a loop is there once a turn: its node says how many it stands for.
                    const std::size_t turns = runtime.TurnsOf(node.id).size();
                    if (turns > 0) {
                        card.title += std::format("   x{}", turns);
                        if (const std::string loop = loopOf(node, 0); !loop.empty()) loops[loop].push_back(node.id);
                    } else if (node.type == "Repeat") {
                        if (const auto count = node.inputs.find("count"); count != node.inputs.end() && count->second.value.Number()) card.title += std::format("   {} turns", static_cast<int>(*count->second.value.Number()));
                        loops[node.id];
                    }
                    if (const std::string each = runtime.EachOf(node.id); !each.empty()) {
                        // Worked out once an element of a ForEach (or a Filter), a buffer of an element each, a draw of
                        // each: in the node that holds them, with it.
                        eaches[each];
                        if (each != node.id) eaches[each].push_back(node.id);
                    }
                    card.position = { static_cast<float>(node.x), static_cast<float>(node.y) };
                    if (const auto at = places.find(node.id); at != places.end())
                        card.position = { static_cast<float>(at->second.first), static_cast<float>(at->second.second) };
                    card.accent = CategoryColor(type ? type->category : std::string());
                    if (type) card.icon = type->icon;
                    // How abstract it is, as steps in its title bar; made of other nodes, a stack of cards (kgraph::Level).
                    if (type) { card.level = static_cast<int>(type->level); card.composite = type->composite; }
                    for (const auto& input : InputsOf(runtime, node, [&](const std::string& name) { return _session->Asked(node.id, name); }))
                        if (ShowsAsPort(input, node)) {
                            kui::NodePort port { input.port.name, PortLabel(input.port.name), WireType(input.port) };
                            port.color = PortColor(port.type); port.shape = PortShapeOf(port.type);
                            const auto given = node.inputs.find(input.port.name);
                            const bool wired = given != node.inputs.end() && given->second.link;
                            // Set on the node: its control, while nothing is wired to it — what it is, or what it starts as.
                            if (!wired && kgraph::PlacementOf(input.port, node) == kgraph::Placement::eNode)
                                port.control = NodeControl(node, input.port, given != node.inputs.end() && !given->second.value.IsNull() ? given->second.value : input.port.initial);
                            // A field given a value where a wire could go says the value.
                            else if (given != node.inputs.end() && !wired && !given->second.value.IsNull())
                                port.value = Brief(given->second.value, 18);
                            card.inputs.push_back(std::move(port));
                        }
                    std::string kind = node.type;
                    if (node.type == "Reference") {
                        // What was brought in from another pass: it gives what the node it stands for gives.
                        const kgraph::Node* target = StandsFor(node);
                        card.title = target ? target->id : card.title;
                        card.accent = kui::Color::Hex(0x5A6472);
                        // As abstract as what it brings in.
                        if (const kgraph::NodeType* of = target ? kgraph::FindType(target->type) : nullptr) { card.level = static_cast<int>(of->level); card.composite = of->composite; }
                        if (target) {
                            outputsOf(node, *target, card);
                            // Its main output is the thing itself, whatever else of it is wired from.
                            kind = target->pass.empty() ? std::format("{}, imported", target->type)
                                 : Doc().IsSetup(target->pass) ? std::format("{}, made once in {}", target->type, target->pass)
                                 : Doc().RateOf(target->pass) == kgraph::PassRate::eFixed ? std::format("{}, from {} (on the fixed step)", target->type, target->pass)
                                                               : std::format("{}, from {}", target->type, target->pass);
                            if (Doc().IsSetup(target->pass)) card.accent = kui::Color::Hex(0x3E9FA8);
                            if (const std::string holds = Holds(target->id); !holds.empty() && holds != target->type) kind += "  ·  " + holds;
                        }
                        if (card.outputs.empty()) card.outputs.push_back({ "out", "Resource", "r:Resource", PortColor("r:Resource"), PortShapeOf("r:Resource") });
                    } else {
                        // What it gives: what it makes, and whatever else of it something is wired to or was asked for.
                        outputsOf(node, node, card);
                    }
                    if (const auto* status = runtime.Status(node.id); status && !status->ok) card.error = status->error;
                    if (turns > 0) kind += ", once a turn";
                    // And how abstract it is, in words, beside the steps in its title bar.
                    if (card.level >= 0) kind += std::format("  ·  {}{}", kgraph::LevelName(static_cast<kgraph::Level>(card.level)), card.composite ? ", of nodes" : "");
                    float bodyWidth = wide(kind, 6.f) + 16.f, bodyHeight = 18.f;
                    if (node.type == "Note") {
                        // Words: what it says is all it is.
                        const auto text = node.inputs.find("text");
                        const std::string words = text != node.inputs.end() && text->second.value.String() ? *text->second.value.String() : std::string();
                        card.accent = kui::Color::Hex(0x8A7A3A);
                        card.level = -1;        // words, of no level
                        card.body = kui::SizedBox(220.f, -1.f, kui::Text(words, kui::TextStyle {}.SetSize(12.f)));
                        bodyWidth = 236.f;
                        bodyHeight = 12.f + 16.f * std::ceil((wide(words, 6.6f) + 1.f) / 220.f);
                    } else {
                        std::vector<kui::Widget> said;
                        said.push_back(kgui::Muted(kind, 11.f));
                        // A vertex layout is drawn: its buffers, and the attributes along each.
                        if (node.type == "VertexLayout") {
                            const auto attributes = node.inputs.find("attributes");
                            const auto read = AttributesOf(attributes != node.inputs.end() ? attributes->second.value : Value {});
                            said.push_back(LayoutPicture(read));
                            std::set<int> buffers;
                            float widest = 0.f;
                            std::map<int, float> along;
                            for (const auto& a : read) { buffers.insert(a.buffer); widest = std::max(widest, along[a.buffer] += std::max(static_cast<float>(BytesOf(a)) * 4.f, 30.f) + 2.f); }
                            bodyWidth = std::max(bodyWidth, widest + 16.f);
                            bodyHeight += 40.f * static_cast<float>(std::max<std::size_t>(buffers.size(), 1));
                        }
                        // What it is set to, said on it in a line: what it is most for someone reading the graph — a
                        // shader, a format, a size, a number, which entities it runs for.
                        if (node.type != "Reference") {
                            std::vector<std::string> parts;
                            for (const auto& [name, input] : node.inputs) {
                                if (input.link || input.value.IsNull() || input.value.Bool()) continue;
                                if (std::ranges::contains(card.inputs, name, &kui::NodePort::id)) continue;
                                const std::string value = Brief(input.value, 20);
                                if (value.empty() || value == "...") continue;
                                const std::string label = PortLabel(name);
                                // Which entities it runs for is said as that: with these, not those.
                                if (label == "with") parts.push_back("with " + value);
                                else if (label == "without") parts.push_back("not " + value);
                                else if (std::ranges::any_of(std::initializer_list<std::string_view> { "module", "format", "extent", "ize", "value", "count", "component", "file", "text", "parameter" },
                                                             [&](const std::string_view word) { return label.find(word) != std::string::npos; })) parts.push_back(value);
                            }
                            std::string line;
                            for (const auto& part : parts) {
                                if (line.size() + part.size() > 36 && !line.empty()) { line += " ..."; break; }
                                line += (line.empty() ? "" : "  ·  ") + part;
                            }
                            if (!line.empty()) {
                                said.push_back(kui::Text(line, kui::TextStyle {}.SetSize(11.f)));
                                bodyWidth = std::max(bodyWidth, wide(line, 6.f) + 16.f);
                                bodyHeight += 17.f;
                            }
                        }
                        // What it costs, as it runs, coloured by its share of the frame: a pass's own, and a Function's call's or a
                        // loop's every pass inside it added up.
                        if ((type && type->category == "Passes") || node.type == "Function" || CostOf(costs, node.id)) {
                            said.push_back(kui::Make<LiveValue>(_session, [this, id = node.id] {
                                const Timings timings = Doc().Runtime().Timings();
                                const auto cost = CostOf(timings, id);
                                if (!cost) return Value("not run");
                                const double frame = FrameGpuMs(timings);
                                // The words, and the share of the frame they are coloured by.
                                return Value(Value::List { Value(CostText(*cost, frame)), Value(cost->gpuMeasured && frame > 0.0 ? cost->gpuMs / frame : 0.0) });
                            }, [](const Value& said) {
                                const auto* both = said.Items();
                                const bool ran = both && both->size() == 2;
                                const std::string text = ran ? *(*both)[0].String() : said.String() ? *said.String() : std::string();
                                return kgui::Line(text, kui::TextStyle {}.SetSize(11.f).SetColor(ran ? HeatColor(*(*both)[1].Number()) : kui::Theme::Current().textMuted));
                            }));
                            bodyWidth = std::max(bodyWidth, 150.f);
                            bodyHeight += 17.f;
                        }
                        // What it says while it is fine: what it waits for — a script that fills what it makes, which nothing runs yet.
                        if (const auto* status = runtime.Status(node.id); status && status->ok && !status->note.empty()) {
                            said.push_back(kui::SizedBox(240.f, -1.f, kui::Text(status->note, kui::TextStyle {}.SetSize(11.f).SetColor(kui::Theme::Current().textMuted))));
                            bodyWidth = std::max(bodyWidth, 256.f);
                            bodyHeight += 16.f * std::ceil((wide(status->note, 6.f) + 1.f) / 240.f);
                        }
                        // What it stands for, where it is: what it holds, what this page does with it, what was done to it before.
                        if (const kgraph::Node* target = node.type == "Reference" ? StandsFor(node) : nullptr; target && target != &node && runtime.ResourceOf(node.id)) {
                            const UseHere use = UsesAt(target->id, node.pass);
                            const kui::Theme& t = kui::Theme::Current();
                            // A line more, of a card that was one line less: a graph laid out before has room for it.
                            said.push_back(kui::Row({ ListPill(UsedHere(use), use.write ? t.text : t.textMuted),
                                                      kui::Expanded(kui::Text(WrittenBefore(use), kui::TextStyle {}.SetSize(11.f).SetColor(kui::Theme::Current().textMuted), kui::TextAlign::eStart, false, 1, true)) },
                                                    kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                            bodyWidth = std::max(bodyWidth, wide(UsedHere(use), 6.f) + wide(WrittenBefore(use), 6.f) + 40.f);
                            bodyHeight += 21.f;
                        }
                        // An image is always seen on its node: as it was when its turn last came to be copied out — a
                        // Reference's, as its page begins.
                        if (node.type != "Screen" && runtime.ImageOf(node.id).Get()) {
                            const auto picture = _session->ThumbnailOf(node.id);
                            const std::string& says = _session->ThumbnailSaysOf(node.id);
                            // As big from the first, its picture there or not: a card that grew once it came would lie over
                            // what the page was put in order round it.
                            if (picture.Get()) said.push_back(kui::Make<NodePicture>(_session, node.id, false));
                            else said.push_back(kui::SizedBox(240.f, 146.f, kui::Align(kui::Alignment::Center(),
                                                    kgui::Muted(says.empty() ? std::string("Its picture is on its way.") : says, 11.f))));
                            bodyWidth = std::max(bodyWidth, 256.f);
                            bodyHeight += 156.f;
                            card.width = 264.f;     // as wide as its picture: a card is otherwise as narrow as its words
                            if (wide(kind, 6.f) + 16.f > card.width) bodyHeight += 15.f;    // and its kind, wider than that, in two lines
                        }
                        card.body = kui::Column(std::move(said), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
                    }
                    _placed.push_back(measured(card, bodyWidth, bodyHeight));
                    shown.nodes.push_back(std::move(card));
                }
                // A wire from another page's node, not brought in by a Reference: the node is shown beside what it
                // is wired to, as what it is — a thing of another pass.
                if (!everything) {
                    std::map<std::string, kor::Vec2> outside;
                    for (const auto& node : graph.nodes) {
                        if (!here(node)) continue;
                        for (const auto& input : node.inputs | std::views::values)
                            if (input.link && !visible.contains(input.link->node) && graph.Find(input.link->node)) {
                                const auto [at, fresh] = outside.try_emplace(input.link->node, kor::Vec2(static_cast<float>(node.x) - 260.f, static_cast<float>(node.y)));
                                if (!fresh) at->second = { std::min(at->second.x, static_cast<float>(node.x) - 260.f), std::min(at->second.y, static_cast<float>(node.y)) };
                            }
                    }
                    for (const auto& [id, at] : outside) {
                        const kgraph::Node& node = *graph.Find(id);
                        const kgraph::Node* target = StandsFor(node);
                        kui::GraphNode card;
                        card.id = id;
                        card.title = node.title.empty() ? id : node.title;
                        // Beside what it is wired to — or where the page was last put in order with it — and wherever it
                        // was dragged to from there, on this page.
                        const auto arranged = _outsideArranged[page].find(id);
                        const kor::Vec2 dragged = _outsideMoved[page][id];
                        card.position = (arranged != _outsideArranged[page].end() ? arranged->second : at) + dragged;
                        card.accent = kui::Color::Hex(0x5A6472);
                        card.icon = "link";
                        outputsOf(node, target ? *target : node, card);
                        const std::string where = node.pass.empty() ? std::format("{}, in no pass", node.type) : std::format("{}, in {}", node.type, node.pass);
                        card.body = kui::Padding(kui::EdgeInsets::Symmetric(0.f, 0.f), kgui::Muted(where, 11.f));
                        // Put in order with the page's own: it is on the page as much as they are.
                        _placed.push_back(measured(card, wide(where, 6.f) + 16.f, 18.f));
                        _placedOutside.insert(id);
                        shown.nodes.push_back(std::move(card));
                    }
                }
                std::map<std::string, const kui::GraphNode*> drawn;
                for (const auto& card : shown.nodes) drawn[card.id] = &card;
                // How far down its card a port is: where its row is, as kui::nodes lays a card out. None shown: the middle.
                const auto portY = [&](const std::string& id, const std::string& port, const bool output) {
                    const kui::GraphNode& card = *drawn.at(id);
                    const auto& ports = output ? card.outputs : card.inputs;
                    const auto it = std::ranges::find(ports, port, &kui::NodePort::id);
                    if (it == ports.end()) return -1.f;
                    return kui::nodes::PortOffset(output, static_cast<std::size_t>(it - ports.begin()), 0.f, card.outputs.size()).y;
                };
                for (const auto& node : graph.nodes)
                    if (drawn.contains(node.id) && visible.contains(node.id))
                        for (const auto& [name, input] : node.inputs)
                            if (input.link && drawn.contains(input.link->node)) {
                                shown.wires.push_back({ { input.link->node, input.link->output }, { node.id, name } });
                                _wires.push_back({ input.link->node, node.id, portY(input.link->node, input.link->output, true), portY(node.id, name, false) });
                            }
                // A node is of one loop: one there once a turn of a Repeat is in that loop's, though it also runs for each
                // element of a ForEach — a shadow's draw, once a face and once an entity. Two loops are never one.
                std::set<std::string> ofTurns;
                for (const auto& [repeat, members] : loops) {
                    ofTurns.insert(repeat);
                    ofTurns.insert(members.begin(), members.end());
                }
                // (A ForEach that is itself once a turn is of the loop as a whole: its frame is inside the loop's.)
                for (auto& [each, members] : eaches)
                    if (!ofTurns.contains(each)) std::erase_if(members, [&](const std::string& id) { return ofTurns.contains(id); });
                // What a Filter keeps some of — an Entities node, another Filter — is in its node too, when it is that
                // Filter's alone: one that feeds several stands by itself.
                std::map<std::string, std::vector<std::string>> fedTo;
                for (const auto& node : graph.nodes)
                    if (node.type == "Filter")
                        if (const auto from = node.inputs.find("entities"); from != node.inputs.end() && from->second.link) fedTo[from->second.link->node].push_back(node.id);
                for (auto& [each, members] : eaches) {
                    std::string at = each;
                    for (int hops = 0; hops < 16; ++hops) {
                        const kgraph::Node* node = graph.Find(at);
                        const auto from = node && node->type == "Filter" ? node->inputs.find("entities") : decltype(node->inputs.end()) {};
                        if (!node || node->type != "Filter" || from == node->inputs.end() || !from->second.link) break;
                        const std::string& upstream = from->second.link->node;
                        if (fedTo[upstream].size() != 1 || (eaches.contains(upstream) && !eaches[upstream].empty())) break;
                        members.push_back(upstream);
                        at = upstream;
                    }
                }
                // Each loop is a node that holds its nodes: a ForEach's, a Filter's, a Repeat's.
                std::vector<kui::GraphComment> frames;
                for (auto& [each, members] : eaches) {
                    if (members.empty()) continue;
                    const kgraph::Node* node = graph.Find(each);
                    const auto read = [&](const char* input) -> const kgraph::Value* {
                        if (!node) return nullptr;
                        const auto it = node->inputs.find(input);
                        return it == node->inputs.end() ? nullptr : &it->second.value;
                    };
                    std::string what = "entity";
                    if (const kgraph::Value* name = read("component"); name && name->String() && !name->String()->empty()) what = *name->String();
                    else if (const kgraph::Value* with = read("with"); with && with->Items() && !with->Items()->empty()) {
                        what = "entity with";
                        for (const auto& one : *with->Items()) if (one.String()) what += (what.ends_with("with") ? " " : ", ") + *one.String();
                    }
                    const auto count = runtime.ValueOf(each, "count");
                    kui::GraphComment frame;
                    frame.id = "each " + each;
                    frame.text = std::format("{}: for each {}{}", node && !node->title.empty() ? node->title : each, what,
                                             count.Number() ? std::format("  ·  {}", static_cast<int>(*count.Number())) : std::string());
                    frame.color = kui::Color::Hex(0x3BA0C2);
                    frame.container = true;
                    frame.around = std::move(members);
                    if (visible.contains(each)) frame.around.push_back(each);
                    std::erase_if(frame.around, [&](const std::string& id) { return !visible.contains(id); });
                    frames.push_back(std::move(frame));
                }
                // A loop of turns: its Repeat, and what is there once a turn because of it.
                for (auto& [repeat, members] : loops) {
                    if (members.empty()) continue;
                    const kgraph::Node* node = graph.Find(repeat);
                    const auto count = node ? node->inputs.find("count") : decltype(node->inputs.end()) {};
                    kui::GraphComment frame;
                    frame.id = "loop " + repeat;
                    frame.text = node && count != node->inputs.end() && count->second.value.Number()
                        ? std::format("{}: {} turns", repeat, static_cast<int>(*count->second.value.Number())) : std::format("{}: a loop", repeat);
                    frame.color = kui::Color::Hex(0x9C6ADE);
                    frame.container = true;
                    frame.around = std::move(members);
                    if (visible.contains(repeat)) frame.around.push_back(repeat);
                    frames.push_back(std::move(frame));
                }
                // One that holds all of another's nodes, and more, is round it: further out, and drawn first.
                for (bool grew = true; grew;) {
                    grew = false;
                    for (auto& outer : frames)
                        for (const auto& inner : frames)
                            if (&outer != &inner && outer.around.size() > inner.around.size() && outer.depth <= inner.depth
                                && std::ranges::all_of(inner.around, [&](const std::string& id) { return std::ranges::contains(outer.around, id); })) {
                                outer.depth = inner.depth + 1;
                                grew = true;
                            }
                }
                std::ranges::stable_sort(frames, std::greater {}, &kui::GraphComment::depth);
                // Arranged, each frame is a block of its own with what is in it — those inside it too — and room round it for
                // how far out it is drawn: the frames as they are drawn are the groups the page is put in order by.
                for (auto& placed : _placed) { placed.group.clear(); placed.depth = 0; }
                for (const auto& frame : frames)
                    for (const auto& id : frame.around)
                        if (const auto at = std::ranges::find(_placed, id, &PlacedNode::id); at != _placed.end() && at->group.empty()) {
                            at->group = frame.id;
                            at->depth = frame.depth;
                        }
                for (auto& frame : frames) shown.comments.push_back(std::move(frame));
                // What each port carries, for a wire let go over nothing.
                auto kinds = std::make_shared<std::map<std::tuple<std::string, std::string, bool>, std::string>>();
                for (const auto& card : shown.nodes) {
                    for (const auto& port : card.inputs) (*kinds)[{ card.id, port.id, false }] = port.type;
                    for (const auto& port : card.outputs) (*kinds)[{ card.id, port.id, true }] = port.type;
                }
                // What is here to be moved and deleted: the page's own nodes, not those shown from other pages.
                const auto own = [visible](const std::vector<std::string>& nodes) {
                    std::vector<std::string> kept;
                    for (const auto& id : nodes)
                        if (visible.contains(id)) kept.push_back(id);
                    return kept;
                };
                _view->page = everything ? std::string("*") : page;
                // A texture or a shader dropped on the graph from the Assets panel is the node it makes, there; what the
                // drawer has to use here, a Reference to it.
                return kui::DropTarget(kui::DropTargetOptions {}
                    .Accepts([everything](const kui::DragData& dragged) { return dragged.type == "asset" || (dragged.type == "resource" && !everything); })
                    .OnDrop([this, page](const kui::DragData& dragged, const kor::Vec2 local) {
                    const auto* id = dragged.As<std::string>();
                    if (!id) return;
                    const kor::Vec2 at = _view->centre + (local - _view->size * 0.5f) / std::max(_view->zoom, 0.01f);
                    if (dragged.type == "resource") _session->Select(Doc().Import(*id, page, std::round(at.x), std::round(at.y)));
                    else _session->AssetToNode(*id, std::round(at.x), std::round(at.y));
                }), kui::NodeEditor(std::move(shown), kui::NodeEditorOptions {}
                    .SetView(_view)
                    .CanConnect([](const kui::NodePort& from, const kui::NodePort& to) { return WireFits(from.type, to.type); })
                    .OnConnect([this](const kui::GraphWire& wire) { Doc().Connect(wire.from.node, wire.from.port, wire.to.node, wire.to.port); })
                    .OnDisconnect([this](const kui::GraphWire& wire) { Doc().ClearInput(wire.to.node, wire.to.port); })
                    // Built again at once: a frame with the nodes back where they were is a frame of them jumping.
                    .OnMove([this, own, overview, page, visible](const std::vector<std::string>& nodes, const kor::Vec2 by) {
                        if (overview) Doc().MoveInOverview(own(nodes), by.x, by.y);
                        else Doc().MoveNodes(own(nodes), by.x, by.y);
                        // A node of another page shown here, wired straight: moved on this page only, as the editor shows it.
                        for (const auto& id : nodes) if (!visible.contains(id)) _outsideMoved[page][id] = _outsideMoved[page][id] + by;
                        SetState();
                    })
                    .OnDelete([this, own](const std::vector<std::string>& picked, const std::vector<kui::GraphWire>& wires, const std::vector<std::string>&) {
                        const std::vector<std::string> nodes = own(picked);
                        for (const auto& wire : wires) Doc().ClearInput(wire.to.node, wire.to.port);
                        if (!nodes.empty()) Doc().RemoveNodes(nodes);
                        if (std::ranges::find(nodes, _session->selectedNode) != nodes.end()) _session->Select(std::string());
                    })
                    .OnCopy([this, own](const std::vector<std::string>& picked) { _session->CopyNodes(own(picked)); })
                    .OnPaste([this](const kor::Vec2 at) { _session->PasteNodes(std::round(at.x), std::round(at.y)); })
                    .OnDuplicate([this, own](const std::vector<std::string>& picked) { _session->DuplicateNodes(own(picked)); })
                    .OnSelectionChanged([this](const std::vector<std::string>& nodes) { if (!nodes.empty()) _session->Select(nodes.front()); })
                    // A wire let go over nothing asks for what it should go to: a node made there, and wired.
                    .OnWireDropped([this, kinds](const kui::PortRef& from, const bool fromOutput, const kor::Vec2 at) {
                        const auto kind = kinds->find({ from.node, from.port, fromOutput });
                        if (kind == kinds->end()) return;
                        AskForNode({ std::round(fromOutput ? at.x : at.x - 180.f), std::round(at.y - 40.f) }, LooseWire { from, fromOutput, kind->second });
                    })
                    // A wire let go on a node asks which of its ports it goes to: every one, shown on the node or not.
                    .OnWireDroppedOnNode([this, kinds](const kui::PortRef& from, const bool fromOutput, const std::string& node, kor::Vec2) {
                        const auto kind = kinds->find({ from.node, from.port, fromOutput });
                        AskForPort(LooseWire { from, fromOutput, kind == kinds->end() ? std::string() : kind->second }, node);
                    })
                    // Pressed twice: gone to where it is made, when it is of another page; its shader opened, when it has one; looked at, when it holds something.
                    .OnActivate([this, visible](const std::string& id) { Activate(id, visible.contains(id)); })
                    .Menu([this, own](const std::vector<std::string>& picked, const kor::Vec2 at) { return picked.empty() ? PageMenu(at) : NodeMenu(picked, own(picked)); })
                    .OnContextMenu([this](const kor::Vec2 at, kor::Vec2) { AskForNode(at); })));
            }

            /** A node pressed twice. */
            void Activate(const std::string& id, const bool ownPage)
            {
                const kgraph::Node* node = Doc().Graph().Find(id);
                if (!node) return;
                if (!ownPage) { _session->GoTo(id); return; }
                if (node->type == "Reference") {
                    if (const kgraph::Node* target = StandsFor(*node); target && target != node) _session->GoTo(target->id);
                    return;
                }
                if (node->type == "Function") { _session->OpenFunction(id); return; }
                if (const auto shader = _session->ShaderOf(id); !shader.empty()) { _session->OpenInCode(shader); return; }
                if (const auto held = Doc().Runtime().ResourceOf(id); held && (Doc().Runtime().ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView"))
                    _session->View(id);
            }

            static kui::MenuItem MenuLine() { return { {}, {}, true, true }; }

            /** The menu of the graph itself, where nothing is: what can be put there, and the page put in order. */
            std::vector<kui::MenuItem> PageMenu(const kor::Vec2 at)
            {
                return {
                    { "Add node...   Ctrl+Space", [this, at] { AskForNode(at); } },
                    { "Add a note", [this, at] { _session->Select(Doc().AddNode("Note", std::round(at.x), std::round(at.y), {}, ShownPass())); } },
                    { "Paste   Ctrl+V", [this, at] { _session->PasteNodes(std::round(at.x), std::round(at.y)); }, _session->HasCopiedNodes() },
                    MenuLine(),
                    { "Arrange this page", [this] { Arrange(); } },
                    { "Frame everything   F", [this] { _view->Frame(); SetState(); } },
                };
            }

            /** The menu of what is picked: @p picked, of which @p own are this page's to change. */
            std::vector<kui::MenuItem> NodeMenu(const std::vector<std::string>& picked, const std::vector<std::string>& own)
            {
                std::vector<kui::MenuItem> items;
                if (picked.size() == 1) {
                    const std::string id = picked.front();
                    const kgraph::Node* node = Doc().Graph().Find(id);
                    if (!node) return items;
                    if (const auto held = Doc().Runtime().ResourceOf(id); held && (Doc().Runtime().ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView" || held->kind == "Mesh"))
                        items.push_back({ "Open in the viewer", [this, id] { _session->View(id); } });
                    if (const auto shader = _session->ShaderOf(id); !shader.empty())
                        items.push_back({ std::format("Open {} in Visual Studio Code", shader.filename().string()), [this, shader] { _session->OpenInCode(shader); } });
                    if (const kgraph::Node* target = StandsFor(*node); own.empty() || (target && target != node)) {
                        const std::string to = own.empty() || !target ? id : target->id;
                        const kgraph::Node* made = Doc().Graph().Find(to);
                        items.push_back({ made && !made->pass.empty() ? std::format("Go to where it is made, in {}", made->pass) : std::string("Go to where it is made"), [this, to] { _session->GoTo(to); } });
                    }
                    if (!items.empty()) items.push_back(MenuLine());
                }
                const bool some = !own.empty();
                // A call: its function opened, to be changed for every call; or written out, its nodes in its place.
                if (picked.size() == 1)
                    if (const kgraph::Node* node = Doc().Graph().Find(picked.front()); node && node->type == "Function" && some) {
                        const std::string id = node->id;
                        items.push_back({ "Open the function", [this, id] { _session->OpenFunction(id); } });
                        items.push_back({ "Write it out here", [this, id] { _session->WriteOut(id); } });
                        // One of the engine's or a module's, copied into the project: its own from then on, called in their place.
                        const auto named = node->inputs.find("function");
                        const std::string function = named != node->inputs.end() && named->second.value.String() ? *named->second.value.String() : std::string();
                        const std::filesystem::path from = function.empty() ? std::filesystem::path() : Doc().FunctionFile(function);
                        const std::filesystem::path to = Doc().FunctionsDirectory().empty() ? std::filesystem::path() : Doc().FunctionsDirectory() / (function + ".kfunc");
                        std::error_code ignored;
                        if (!from.empty() && !to.empty() && !std::filesystem::equivalent(from, to, ignored))
                            items.push_back({ "Make it the project's own", [this, from, to, function] {
                                std::error_code failed;
                                std::filesystem::create_directories(to.parent_path(), failed);
                                if (!std::filesystem::copy_file(from, to, std::filesystem::copy_options::skip_existing, failed) || failed) {
                                    _session->Say(std::format("{} could not be copied into the project: {}.", function, failed ? failed.message() : std::string("there is one there")), true);
                                    return;
                                }
                                Doc().ReloadFunctions();
                                _session->Say(std::format("functions/{}.kfunc is the project's own now: every call of {} calls it, and what is done to it is the project's.", function, function));
                            } });
                        items.push_back(MenuLine());
                    }
                items.push_back({ own.size() > 1 ? std::format("Make a function of {} nodes...", own.size()) : std::string("Make a function of it..."), [this, own] {
                    SetState([&] { _functionOf = own; _newId.clear(); _dialog = Dialog::eNewFunction; });
                }, some && _session->HasProject() });
                items.push_back({ "Copy   Ctrl+C", [this, own] { _session->CopyNodes(own); }, some });
                items.push_back({ "Duplicate   Ctrl+D", [this, own] { _session->DuplicateNodes(own); }, some });
                // To another pass: moved there (what they take of others, and what others take of them, brought in through
                // References), or copied there.
                if (some) {
                    const std::string here = ShownPass();
                    std::vector<kui::MenuItem> moveTo, copyTo;
                    for (const auto& pass : Doc().Passes()) {
                        moveTo.push_back({ pass, [this, own, pass] { _session->MoveNodesToPass(own, pass); }, pass != here });
                        copyTo.push_back({ pass, [this, own, pass] { _session->CopyNodesToPass(own, pass); } });
                    }
                    moveTo.push_back({ "A new pass", [this, own] { _session->MoveNodesToPass(own, {}); } });
                    const std::string what = own.size() == 1 ? std::string("it") : std::format("{} nodes", own.size());
                    items.push_back({ .label = std::format("Move {} to a pass", what), .items = std::move(moveTo) });
                    items.push_back({ .label = std::format("Copy {} to a pass", what), .items = std::move(copyTo) });
                }
                items.push_back({ "Take its wires off", [this, own] {
                    for (const auto& id : own) {
                        const kgraph::Node* node = Doc().Graph().Find(id);
                        if (!node) continue;
                        std::vector<std::string> wired;
                        for (const auto& [name, input] : node->inputs) if (input.link) wired.push_back(name);
                        for (const auto& name : wired) Doc().ClearInput(id, name);
                        for (const auto& output : Doc().Runtime().OutputsOf(id)) Doc().DisconnectOutput(id, output.name);
                    }
                }, some });
                items.push_back(MenuLine());
                items.push_back({ picked.size() > 1 ? std::format("Delete {} nodes   Del", own.size()) : std::string("Delete   Del"), [this, own] {
                    Doc().RemoveNodes(own);
                    if (std::ranges::find(own, _session->selectedNode) != own.end()) _session->Select(std::string());
                }, some });
                return items;
            }

            // ---- the outliner -----------------------------------------------------------------------------------

            /** The picture an entity is shown by, for what it is. */
            static std::pair<const char*, kui::Color> EntityIcon(const std::string& kind)
            {
                if (kind == "camera") return { "videocam", kui::Color::Hex(0x62C3E8) };
                if (kind == "sun") return { "wb_sunny", kui::Color::Hex(0xE8C562) };
                if (kind == "light") return { "lightbulb", kui::Color::Hex(0xE8C562) };
                if (kind == "mesh") return { "view_in_ar", kui::Color::Hex(0x7DBB4B) };
                if (kind == "script") return { "data_object", kui::Color::Hex(0x5B9BD5) };
                if (kind == "group") return { "workspaces", kui::Color::Hex(0x9AA4B0) };     // nothing of its own but what is under it
                return { "radio_button_unchecked", kui::Color::Hex(0x9AA4B0) };
            }

            /** Something of the Assets panel, by its id: what was dropped is looked up for what kind of thing it is. */
            std::optional<Session::Asset> AssetOf(const std::string& id)
            {
                const auto assets = _session->Assets();
                const auto it = std::ranges::find(assets, id, &Session::Asset::id);
                return it == assets.end() ? std::nullopt : std::optional(*it);
            }

            /**
             * A field that holds a mesh or a picture, @p component 's or a material asset's of that kind: picked from what
             * it can hold, shown small beside its name, and given what is dropped on it from the Assets panel when that is
             * the kind of thing it holds.
             */
            kui::Widget HeldField(const std::string& component, const std::string& fieldName, const std::string& type, const Value& value,
                                  const std::function<void(const Value&)>& set)
            {
                kui::Widget editor;
                // What it can hold, and nothing it cannot: a mesh's field the meshes, a picture's the pictures —
                // and of the graph only what is made before the pass that would use it.
                std::vector<std::string> choices { std::string() };
                int selected = 0;
                const std::string holds = value.String() ? *value.String() : std::string();
                // A procedural mesh, however it is tuned, is the one choice of its shape.
                const std::string shape = type == "Mesh" && holds.starts_with('$') ? std::string(kgraph::ShapeOf(std::string_view(holds).substr(1))) : std::string();
                const std::string chosen = shape.empty() ? holds : kgraph::DefaultMeshId(shape);
                for (const auto& choice : _session->FieldChoices(component, type)) {
                    if (value.String() && chosen == choice) selected = static_cast<int>(choices.size());
                    choices.push_back(choice);
                }
                // What it holds that is none of those is listed all the same: it is what is so.
                if (selected == 0 && !holds.empty()) { selected = static_cast<int>(choices.size()); choices.push_back(holds); }
                // An entity's mesh can be made by a generator of the project's: picked, the entity is given it, and it
                // makes the mesh — the one it gave before is taken away.
                const kecs::Entity entity = _session->selectedEntity;
                const auto generators = component == "Mesh" && !entity.IsNull() ? Doc().ScriptsTaking("MeshGenerator") : std::vector<std::string> {};
                const std::size_t picked = choices.size();
                for (const auto& generator : generators) {
                    if (holds.starts_with(std::format("$script/gen/{}/", generator))) selected = static_cast<int>(choices.size());
                    choices.push_back("generator:" + generator);
                }
                std::vector<std::string> labels;
                for (const auto& choice : choices)
                    labels.push_back(choice.empty() ? std::string("(nothing)") : choice.starts_with("generator:") ? std::format("{} (procedural)", choice.substr(10)) : ChoiceLabel(choice));
                editor = kui::Dropdown(std::move(labels), selected, [this, set, choices, generators, entity, picked](const int index) {
                    for (const auto& generator : generators)
                        if (std::ranges::contains(Doc().ComponentsOf(entity), generator)) Doc().RemoveComponent(entity, generator);
                    if (index >= static_cast<int>(picked)) { Doc().AddComponent(entity, choices[static_cast<std::size_t>(index)].substr(10)); return; }
                    set(index <= 0 ? Value {} : Value(choices[static_cast<std::size_t>(index)]));
                });
                // What it holds is shown beside its name, small: a picture by itself, a mesh by the picture of it.
                kui::Widget shownBeside;
                if (type == "Mesh") {
                    // By its kind until its picture is drawn.
                    // (A tuned one by its shape's: a picture for every number it is dragged through would fill the cache.)
                    if (!holds.empty())
                        shownBeside = kui::Make<MeshThumbnail>(_session, [session = _session, chosen] { return session->MeshPicture(chosen); }, [](const kor::ResourceRef<const kor::Image> picture) {
                            return picture.Get() ? kui::Image(picture, kui::ImageFit::eContain) : kui::Icon("view_in_ar", kui::IconStyle::eFilled, kui::Color::Hex(0x6FA87A).WithAlpha(0.55f));
                        });
                } else if (!holds.empty()) {
                    if (const auto held = Doc().Runtime().ResourceOf(holds); held && held->handle && (held->kind == "Image" || held->kind == "ImageView") && Doc().Runtime().ImageOf(holds).Get())
                        shownBeside = kui::Image(Doc().Runtime().ImageOf(holds), kui::ImageFit::eContain);
                    else if (holds.starts_with('@'))
                        if (const auto file = PictureOf(Doc().Runtime().ResourceOf(holds).get()); file.Get()) shownBeside = kui::Image(file, kui::ImageFit::eContain);
                }
                if (holds.starts_with('@')) editor = kui::Tooltip(holds, std::move(editor));
                if (shownBeside)
                    editor = kui::Row({ kui::SizedBox(48.f, 48.f, std::move(shownBeside)), kui::Expanded(std::move(editor)) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                // And what is dropped on it from the Assets panel is what it holds from then on — when it is
                // the kind of thing the field holds.
                editor = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, set, type, fieldName](const kui::DragData& dragged, kor::Vec2) {
                    const auto* id = dragged.As<std::string>();
                    const auto asset = id ? AssetOf(*id) : std::nullopt;
                    if (!asset) return;
                    const bool mesh = asset->kind == Session::Asset::Kind::eMesh, picture = asset->kind == Session::Asset::Kind::eTexture;
                    if (type == "Mesh" ? mesh : type == "Image" && picture) set(Value(*id));
                    else _session->Say(std::format("'{}' holds a {}, and {} is not one.", fieldName, type == "Mesh" ? "mesh" : type == "Image" ? "picture" : type, asset->label), true);
                }), std::move(editor));
                if (const auto& parameters = kgraph::ShapeParameters(shape); !parameters.empty()) {
                    // What this entity's is made from: tuned, it holds another mesh, shared with any of the same numbers.
                    const std::vector<float> values = kgraph::ShapeValues(std::string_view(holds).substr(1));
                    std::vector<kui::Widget> rows { std::move(editor) };
                    for (std::size_t i = 0; i < parameters.size(); ++i) {
                        const kgraph::MeshParameter& parameter = parameters[i];
                        const kui::NumberType number = parameter.whole ? kui::NumberType::eUInt : kui::NumberType::eFloat;
                        rows.push_back(kui::Tooltip(parameter.says, kgui::Labeled(parameter.name, kui::DragValue(values[i], [set, shape, values, i](const double v) {
                            auto tuned = values;
                            tuned[i] = static_cast<float>(v);
                            set(Value(kgraph::DefaultMeshId(kgraph::ShapeName(shape, tuned))));
                        }, Dragging({ parameter.least, parameter.most, false }, number)), LabelWidth)));
                    }
                    if (holds != chosen) rows.push_back(Chip("Back to how it starts", [set, chosen] { set(Value(chosen)); }));
                    return kui::Column(std::move(rows), kui::FlexOptions {}.SetGap(4.f));
                }
                return editor;
            }

            /** Where on an entity's line something is held, as where it would go: over its top edge, before it; its foot, after it; its middle, under it. */
            static Session::Beside BesideAt(const kor::Vec2 local, const bool openWithChildren)
            {
                constexpr float Line = 26.f;
                if (local.y < 7.f) return Session::Beside::eBefore;
                if (local.y > Line - 7.f && local.y <= Line + 2.f && !openWithChildren) return Session::Beside::eAfter;
                return Session::Beside::eUnder;
            }

            /**
             * An entity in the outliner — what it is, its name, what is wrong with it, whether it is drawn — and under
             * it, further in, everything that is under it. @p flat: by itself, as something found.
             */
            kui::Widget EntityRow(const kecs::Entity entity, const bool flat = false)
            {
                const kui::Theme& t = kui::Theme::Current();
                const auto& children = Doc().World().ChildrenOf(entity);
                const bool open = !flat && !_session->Folded(entity);
                std::vector<kui::Widget> under;
                if (open)
                    for (const kecs::Entity child : children) under.push_back(EntityRow(child));
                const std::string name = Doc().NameOf(entity);
                const std::string key = std::format("entity {}.{}", entity.index, entity.generation);
                const bool picked = entity == _session->selectedEntity;
                const bool can = _session->CanShow(entity), shown = !can || _session->IsShown(entity);
                const auto [icon, tint] = EntityIcon(_session->EntityKind(entity));

                std::vector<kui::Widget> line;
                line.push_back(kui::SizedBox(15.f, 15.f, kui::Icon(icon, kui::IconStyle::eFilled, shown ? tint : tint.WithAlpha(0.45f))));
                if (_renaming == entity) {
                    line.push_back(kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = name,
                        .onSubmitted = [this, entity](const std::string& typed) { if (!typed.empty()) Doc().Rename(entity, typed); SetState([&] { _renaming = kecs::NullEntity; }); }, .focus = 1 }).Key("rename " + key)));
                } else {
                    line.push_back(kui::Expanded(kui::Text(name, kui::TextStyle {}.SetSize(13.f).SetColor(picked ? t.primary : shown ? t.text : t.textMuted), kui::TextAlign::eStart, false, 1, true)));
                }
                // Shut, it says how many are under it; one a pass could not run for says so, and why to the pointer.
                if (!flat && !open && !children.empty()) line.push_back(kgui::Muted(std::to_string(children.size()), 11.f));
                // A prefab's instance: which prefab, and whether anything of it is its own.
                if (const std::string prefab = Doc().PrefabOf(entity); !prefab.empty()) {
                    const std::size_t own = Doc().OverridesOf(entity).size();
                    line.push_back(kui::Tooltip(std::format("An instance of {}{}", std::filesystem::path(prefab).stem().string(), own ? std::format(", {} of it its own", own) : std::string()),
                                                kui::SizedBox(13.f, 13.f, kui::Icon("widgets", kui::IconStyle::eFilled, kui::Color::Hex(0x4FB3BF).WithAlpha(own ? 1.f : 0.7f)))));
                }
                if (flat)
                    if (const kecs::Entity parent = Doc().World().ParentOf(entity); !parent.IsNull()) line.push_back(kgui::Muted("in " + Doc().NameOf(parent), 11.f));
                if (const auto passed = _passedBy.find(entity); passed != _passedBy.end()) {
                    std::string why;
                    for (const auto& each : passed->second) why += (why.empty() ? "Not drawn by " : "\n") + each;
                    line.push_back(kui::Tooltip(why, kui::SizedBox(14.f, 14.f, kui::Icon("warning", kui::IconStyle::eFilled, kui::Color::Hex(0xE5484D)))));
                }
                if (can)
                    line.push_back(kui::Tooltip(shown ? "Drawn. Pressed: it and what is under it are not." : "Not drawn. Pressed: it and what is under it are.",
                        kui::Button(kui::SizedBox(14.f, 14.f, kui::Icon(shown ? "visibility" : "visibility_off", kui::IconStyle::eFilled, shown ? t.textMuted : t.textMuted.WithAlpha(0.6f))),
                                    [this, entity, shown] { _session->SetShown(entity, !shown); }, kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain))));
                kui::Widget title = kui::Row(std::move(line), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));

                kui::Widget node = kui::TreeNode({}, open, [this, entity](const bool nowOpen) { _session->Fold(entity, !nowOpen); }, std::move(under),
                                                 kui::TreeNodeOptions {}.SetLeaf(flat || children.empty()).SetSelected(picked).SetTitle(std::move(title))
                                                     // Pressed, it is picked; pressed again at once, its name is there to be typed over.
                                                     .OnTap([this, entity, key] { _assets.Pressed(key, [&] { _session->Select(entity); }, [&] { SetState([&] { _renaming = entity; }); }); }));
                // Where what is held over it would go: a line over it or under it, or all of it lit.
                if (_dropOn == entity) {
                    const kui::Widget mark = _dropWhere == Session::Beside::eUnder
                        ? kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = 0.f, .right = 0.f, .height = 26.f }, kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary.WithAlpha(0.22f)).SetRadius(4.f)))))
                        : kui::Positioned(kui::PositionedOptions { .left = 0.f, .top = _dropWhere == Session::Beside::eBefore ? -1.f : 25.f, .right = 0.f, .height = 2.f },
                                          kui::IgnorePointer(kui::Container(kui::ContainerOptions {}.SetDecoration(kui::Decoration {}.SetColor(t.primary)))));
                    node = kui::Stack({ std::move(node), mark });
                }
                // Picked up and dropped on another, it goes before it, after it or under it: where it is in the world
                // stays as it was. And what is dropped on it from the Assets panel — a mesh, a texture, a script — is its.
                node = kui::Draggable({ "entity", entity }, std::move(node));
                const bool openWithChildren = open && !children.empty();
                node = kui::DropTarget(kui::DropTargetOptions {}
                    .Accepts([entity](const kui::DragData& dragged) {
                        if (const auto* moved = dragged.As<kecs::Entity>()) return dragged.type == "entity" && *moved != entity;
                        return dragged.type == "asset";
                    })
                    .OnMove([this, entity, openWithChildren, flat](const kui::DragData& dragged, const kor::Vec2 local) {
                        const Session::Beside where = dragged.type == "entity" && !flat ? BesideAt(local, openWithChildren) : Session::Beside::eUnder;
                        if (_dropOn != entity || _dropWhere != where) SetState([&] { _dropOn = entity; _dropWhere = where; });
                    })
                    .OnLeave([this, entity] { if (_dropOn == entity) SetState([&] { _dropOn = kecs::NullEntity; }); })
                    .OnDrop([this, entity, openWithChildren, flat](const kui::DragData& dragged, const kor::Vec2 local) {
                        _dropOn = kecs::NullEntity;
                        if (const auto* moved = dragged.As<kecs::Entity>()) {
                            _session->MoveEntity(*moved, entity, flat ? Session::Beside::eUnder : BesideAt(local, openWithChildren));
                            if (!flat) _session->Fold(entity, false);
                        } else if (const auto* asset = dragged.As<std::string>()) {
                            if (_session->GiveAsset(*asset, entity) && !asset->starts_with("prefab:")) _session->Select(entity);
                        }
                        SetState();
                    }), std::move(node));
                // Its menu, as a scene tree's is: what is done to it first, what is made under it, how it is seen, and its prefab.
                std::vector<kui::MenuItem> menu;
                menu.push_back({ "Rename", [this, entity] { _session->Select(entity); SetState([&] { _renaming = entity; }); } });
                menu.push_back({ "Duplicate", [this, entity] { _session->Duplicate(entity); } });
                menu.push_back({ "Delete, with what is under it", [this, entity] { _session->Delete(entity); } });
                menu.push_back({ .separator = true });
                menu.push_back({ .label = "Create under it", .items = ObjectMenu(entity, [this, entity] { _session->Fold(entity, false); }) });
                menu.push_back({ .separator = true });
                menu.push_back({ "Frame in the scene view", [this, entity] { _session->Select(entity); _session->Frame(entity); } });
                if (can) menu.push_back({ shown ? "Hide, with what is under it" : "Show, with what is under it", [this, entity, shown] { _session->SetShown(entity, !shown); } });
                if (!Doc().World().ParentOf(entity).IsNull()) menu.push_back({ "Move to the top", [this, entity] { Doc().SetParent(entity, kecs::NullEntity); } });
                menu.push_back({ .separator = true });
                menu.push_back({ .label = "Prefab", .items = PrefabMenu(entity) });
                menu.push_back({ "Save as object", [this, entity] { _session->SaveAsObject(entity); } });
                return kui::ContextMenu(std::move(menu), std::move(node)).Key(key);
            }

            std::map<kecs::Entity, std::vector<std::string>> _passedBy;     // which entities a pass could not run for, and why: as of this build

            /**
             * What there is to make an entity of, as a menu: an empty one; the meshes, the lights and the cameras each in a
             * submenu of their own — by what each holds — and the rest as they are. Made under @p parent, when it is one.
             */
            std::vector<kui::MenuItem> ObjectMenu(const kecs::Entity parent, std::function<void()> before = {})
            {
                std::vector<kui::MenuItem> meshes, lights, cameras, others;
                std::vector<kui::MenuItem> menu;
                for (const auto& object : _session->Objects()) {
                    // By its name, found again when it is picked: every entity's row has this menu, and a copy of each
                    // object in each was most of what building the interface again cost.
                    kui::MenuItem item { object.name, [this, name = object.name, parent, before] {
                        const auto& objects = _session->Objects();
                        const auto found = std::ranges::find(objects, name, &ObjectTemplate::name);
                        if (found == objects.end()) return;
                        if (before) before();
                        _session->Create(*found, parent);
                    } };
                    if (object.name == "Empty") {
                        item.label = "Empty entity";
                        if (parent.IsNull()) item.onSelected = [this] { _session->Select(Doc().CreateEntity()); };     // called what any new entity is
                        else item.onSelected = [this, parent, before] { if (before) before(); _session->Select(Doc().CreateEntity("Entity", parent)); };
                        menu.insert(menu.begin(), std::move(item));
                        continue;
                    }
                    const Value* components = object.data.Find("components");
                    const auto has = [&](const auto& test) {
                        if (components && components->Named())
                            for (const auto& name : *components->Named() | std::views::keys) if (test(name)) return true;
                        return false;
                    };
                    if (has([&](const std::string& name) { return IsCamera(Doc().World(), name); })) cameras.push_back(std::move(item));
                    else if (has([](const std::string& name) { return name.ends_with("Light"); })) lights.push_back(std::move(item));
                    else if (has([](const std::string& name) { return name == "Mesh"; })) meshes.push_back(std::move(item));
                    else others.push_back(std::move(item));
                }
                if (menu.size() == 1 && (!meshes.empty() || !lights.empty() || !cameras.empty() || !others.empty())) menu.push_back({ .separator = true });
                if (!meshes.empty()) menu.push_back({ .label = "Mesh", .items = std::move(meshes) });
                if (!lights.empty()) menu.push_back({ .label = "Light", .items = std::move(lights) });
                if (!cameras.empty()) menu.push_back({ .label = "Camera", .items = std::move(cameras) });
                for (auto& item : others) menu.push_back(std::move(item));
                return menu;
            }

            /** What a prefab is to an entity, in its menu: made one — or, of an instance, its prefab opened, applied, reverted, let go. */
            std::vector<kui::MenuItem> PrefabMenu(const kecs::Entity entity)
            {
                Document& doc = Doc();
                const kecs::Entity root = doc.PrefabRootOf(entity);
                if (root.IsNull())
                    return { { "Save as prefab", [this, entity] { _session->SaveAsPrefab(entity); }, _session->HasProject() } };
                const auto act = [this, root](bool (Document::*what)(kecs::Entity, std::string*), const char* done) {
                    return [this, root, what, done] {
                        std::string error;
                        if ((Doc().*what)(root, &error)) _session->Say(done); else _session->Say(error, true);
                    };
                };
                const std::string id = "prefab:" + std::filesystem::path(doc.PrefabOf(root)).filename().string();
                std::vector<kui::MenuItem> items {
                    { "Edit the prefab", [this, id] { _session->OpenPrefab(id); }, _session->PrefabOpen().empty() },
                    { "Apply to the prefab", act(&Document::ApplyPrefab, "Applied to the prefab: every instance of it follows.") },
                    { "Revert to the prefab", act(&Document::RevertPrefab, "Reverted: it is as its prefab is.") },
                    { .separator = true },
                    { "Unlink: no prefab's", [this, root] { Doc().Unlink(root); _session->Say("Unlinked: no prefab's now."); } },
                };
                if (root != entity) items.push_back({ "Pick the instance", [this, root] { _session->Select(root); } });
                return items;
            }

            /**
             * The world's entities as a tree: each by what it is and its name; picked by pressing, renamed by pressing
             * twice, folded by their arrows; moved before, after or under one another by dragging; shown and hidden by
             * their eyes. Typed into, the line over it finds them by name, or by a component they have.
             */
            /**
             * Over the outliner while a prefab is open: which, and the way back — as Unity's breadcrumb is. Its entities are
             * all the outliner has, and the scene view draws them alone.
             */
            kui::Widget PrefabBar(const std::string& prefab)
            {
                const kui::Theme& t = kui::Theme::Current();
                const kui::Color accent = t.primary;     // the theme's accent on its icon; the bar itself, the panel's grey
                const bool changed = _session->PrefabChanged();
                return kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::All(8.f))
                                          .SetDecoration(kui::Decoration {}.SetColor(t.text.WithAlpha(0.05f))),
                    Rows({
                        kui::Row({ kui::SizedBox(16.f, 16.f, kui::Icon("widgets", kui::IconStyle::eFilled, accent)),
                                   kui::Expanded(kui::Text(std::format("{}{}", std::filesystem::path(prefab).stem().string(), changed ? "  ·  changed" : ""),
                                                           kui::TextStyle {}.SetSize(13.f).SetColor(t.text), kui::TextAlign::eStart, false, 1, true)) },
                                 kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)),
                        kgui::Muted("The prefab, by itself: what is done here is the prefab's, and every instance follows it once it is saved.", 11.f),
                        kui::Row({ kui::Tooltip("Writes the prefab as it is now (Ctrl+S)", Chip("Save", [this] { _session->SavePrefab(); })),
                                   kui::Tooltip("The scene as it was, its instances of the prefab as it is: saved first, when it has changed",
                                                Chip("Back to the scene", [this] { _session->ClosePrefab(true); })) },
                                 kui::FlexOptions {}.SetGap(6.f)),
                    }, 6.f));
            }

            kui::Widget Outliner()
            {
                _passedBy.clear();
                for (auto& [entity, why] : Doc().Runtime().PassedBy()) _passedBy[entity].push_back(std::move(why));
                kui::Widget bar = kui::Row({
                    kui::Tooltip("Makes an entity: an empty one, or one of the objects there are — meshes, lights and cameras each in a list of their own.",
                        kui::MenuBar({ { "+", ObjectMenu(kecs::NullEntity) } })),
                    kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = _outlinerFilter, .placeholder = "Find: a name, or a component",
                        .onChanged = [this](const std::string& typed) { SetState([&] { _outlinerFilter = typed; }); } }).Key("outliner filter")),
                }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                std::vector<kui::Widget> rows;
                if (_outlinerFilter.empty()) {
                    for (const kecs::Entity entity : Doc().World().Roots()) rows.push_back(EntityRow(entity));
                    if (rows.empty()) rows.push_back(kgui::Muted("No entities yet."));
                } else {
                    const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                    const std::string wanted = lower(_outlinerFilter);
                    for (const kecs::Entity entity : Doc().Entities()) {
                        bool found = lower(Doc().NameOf(entity)).find(wanted) != std::string::npos;
                        for (const auto& component : Doc().ComponentsOf(entity)) found = found || lower(component).find(wanted) != std::string::npos;
                        if (found) rows.push_back(EntityRow(entity, true));
                    }
                    if (rows.empty()) rows.push_back(kgui::Muted(std::format("No entity is called '{}', or has such a component.", _outlinerFilter)));
                }
                // Dropped on the panel itself, past the tree, an entity goes to the top — and a model is imported.
                // Right-clicked where no entity is: what can be made, at the top. (An entity's line has its own menu.)
                kui::Widget tree = kui::DropTarget(kui::DropTargetOptions {}
                    .Accepts([](const kui::DragData& dragged) { return dragged.type == "entity" || dragged.type == "asset"; })
                    .OnDrop([this](const kui::DragData& dragged, kor::Vec2) {
                        if (const auto* moved = dragged.As<kecs::Entity>()) { Doc().SetParent(*moved, kecs::NullEntity); return; }
                        const auto* id = dragged.As<std::string>();
                        const auto asset = id ? AssetOf(*id) : std::nullopt;
                        if (asset && asset->kind == Session::Asset::Kind::eModel) _assets.AskToImport(asset->file);
                        else if (asset && asset->kind == Session::Asset::Kind::ePrefab) _session->PlacePrefab(asset->id);
                        else _session->Say("Dropped on an entity, a mesh, a texture, a script or a prefab is its; dropped here, a model is imported and a prefab placed.", true);
                    }), kui::ScrollView(kui::Padding(kui::EdgeInsets { 8.f, 4.f, 8.f, 8.f }, Rows(std::move(rows), 2.f))));
                std::vector<kui::Widget> column;
                if (const std::string prefab = _session->PrefabOpen(); !prefab.empty()) column.push_back(PrefabBar(prefab));
                column.push_back(kui::Padding(kui::EdgeInsets { 8.f, 6.f, 8.f, 4.f }, std::move(bar)));
                column.push_back(kui::Separator());
                column.push_back(kui::Expanded(kui::ContextMenu(ObjectMenu(kecs::NullEntity), std::move(tree))));
                return kui::Column(std::move(column),
                                   kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch));
            }

            // ---- the inspector ----------------------------------------------------------------------------------


            kui::Widget Inspector()
            {
                if (!_session->selectedAsset.empty()) {
                    const auto assets = _session->Assets();
                    if (const auto it = std::ranges::find(assets, _session->selectedAsset, &Session::Asset::id); it != assets.end()) return _assets.AssetInspector(*it);
                }
                if (const kecs::Entity entity = _session->selectedEntity; Doc().World().Alive(entity))
                    return kui::Make<Following>(_session, entity, [this, entity] { return EntityInspector(entity); });
                if (const kgraph::Node* node = Doc().Graph().Find(_session->selectedNode)) return NodeInspector(*node);
                return Panel({ kgui::Muted("Pick a node or an entity to see what it is made of.") });
            }

            /**
             * An input's control on its node: small, a row high — a number to drag, a box to tick, a colour, a choice, a
             * few numbers side by side. What it is set to is the node's at once.
             */
            kui::Widget NodeControl(const kgraph::Node& node, const kgraph::Port& port, const Value& value)
            {
                const std::string id = node.id, name = port.name;
                const auto set = [this, id, name](Value changed) { Doc().SetInput(id, name, std::move(changed)); };
                const kui::NumberType number = NumberTypeOf(port);
                const auto drag = [&](const float value, std::function<void(double)> changed, const float width) {
                    return kui::DragValue(value, std::move(changed), Dragging(LimitsOfPort(port), number).SetWidth(width));
                };
                if (!port.choices.empty()) {
                    const std::string* chosen = value.String();
                    const auto at = chosen ? std::ranges::find(port.choices, *chosen) : port.choices.end();
                    // In a box of its own size: what a node's width is worked out from.
                    return kui::SizedBox(118.f, 22.f, kui::Dropdown(port.choices, at == port.choices.end() ? -1 : static_cast<int>(at - port.choices.begin()),
                                                                    [set, choices = port.choices](const int index) { set(Value(choices[static_cast<std::size_t>(index)])); },
                                                                    kui::DropdownOptions {}.SetWidth(118.f)));
                }
                if (port.editor == "color" || (port.type == "vector" && NamesAColor(name))) {
                    const auto* items = value.Items();
                    const bool alpha = !items || items->size() != 3;
                    return kui::SizedBox(40.f, 18.f, ColorOf(value, set, alpha));
                }
                // Exactly what it takes, whatever it holds now: a whole number's dragged a unit at a time, a float4's four drags.
                if (port.shape.Known() && !port.builder && !(port.shape.scalar == kgraph::Scalar::eBool && port.shape.components == 1)) {
                    const kgraph::Shape shape = port.shape;
                    const auto numbers = NumbersOfShape(value, shape);
                    if (numbers.size() == 1)
                        return drag(numbers.front(), [set, shape](const double changed) { set(kgraph::Conform(Value(changed), shape)); }, 64.f);
                    std::vector<kui::Widget> drags;
                    for (std::size_t i = 0; i < numbers.size(); ++i)
                        drags.push_back(drag(numbers[i], [set, shape, numbers, i](const double changed) {
                            auto now = numbers;
                            now[i] = static_cast<float>(changed);
                            set(ValueOfShape(now, shape));
                        }, 44.f));
                    return kui::Row(std::move(drags), kui::FlexOptions {}.SetGap(2.f));
                }
                if (const bool* flag = value.Bool(); flag || port.type == "bool")
                    return kui::Checkbox(flag && *flag, [set](const bool on) { set(Value(on)); });
                if (const std::string* text = value.String(); text || port.type == "string")
                    return kui::TextField(kui::TextFieldOptions { .text = text ? *text : std::string(), .onSubmitted = [set](const std::string& typed) { set(Value(typed)); },
                                                                  .width = 112.f }).Key(std::format("node {} {}", id, name));
                if (const auto* items = value.Items(); items && !items->empty() && items->size() <= 4) {
                    std::vector<kui::Widget> numbers;
                    for (std::size_t i = 0; i < items->size(); ++i)
                        numbers.push_back(drag(NumberOf((*items)[i]), [set, list = *items, i](const double changed) {
                            auto now = list;
                            now[i] = Value(changed);
                            set(Value(std::move(now)));
                        }, 44.f));
                    return kui::Row(std::move(numbers), kui::FlexOptions {}.SetGap(2.f));
                }
                // A number, or nothing yet: a number.
                return drag(value.Number() ? static_cast<float>(*value.Number()) : 0.f, [set](const double changed) { set(Value(changed)); }, 64.f);
            }

            /** The control an input is set with: by what kind of thing it is, then by its type. */
            kui::Widget EditorFor(const kgraph::Node& node, const kgraph::Port& port, const Value& value, const std::function<void(Value)>& set, const std::string& key)
            {
                if (!port.choices.empty()) return Choice(port.choices, value, set, port.choices.front());
                if (port.editor == "color") return ColorOf(value, set);
                // Exactly what it takes, whatever it holds now: a box to tick, or a drag for each of its numbers, of its kind.
                if (port.shape.Known() && !port.builder) {
                    const Value shown = value.IsNull() ? port.initial : value;
                    if (port.shape.scalar == kgraph::Scalar::eBool && port.shape.components == 1)
                        return kui::Checkbox(shown.Bool() && *shown.Bool(), [set](const bool on) { set(Value(on)); });
                    return DragNumbers(NumbersOfShape(shown, port.shape), [set, shape = port.shape](const std::vector<float>& changed) { set(ValueOfShape(changed, shape)); },
                                       LimitsOfPort(port), NumberTypeOf(port));
                }
                if (port.editor == "function") {
                    const auto functions = Doc().Functions();
                    return functions.empty() ? TextEdit(value, set, key, "a function's name") : Choice(functions, value, set, "a function");
                }
                if (port.editor == "module") {
                    const auto modules = _session->ShaderModules();
                    return modules.empty() ? TextEdit(value, set, key, "a module's name") : Choice(modules, value, set, "a module");
                }
                if (port.editor == "entry") {
                    // The entry points of the module named beside it.
                    const std::string beside = port.name.substr(0, port.name.size() - 5) + "module";
                    const auto module = node.inputs.find(beside);
                    const std::string* named = module == node.inputs.end() ? nullptr : module->second.value.String();
                    const auto entries = named ? _session->EntryPoints(*named) : std::vector<std::string> {};
                    return entries.empty() ? TextEdit(value, set, key, "the function's name") : Choice(entries, value, set, "an entry point");
                }
                if (port.editor == "node") {
                    // The nodes it can be: for what stands for another pass's node, what the passes before this one
                    // made; for a loop's `of`, the nodes of a loop; for its `first`, what is not of one.
                    std::vector<std::string> ids;
                    if (node.type == "Reference") ids = Doc().Importable(node.pass);
                    else
                        for (const auto& other : Doc().Graph().nodes) {
                            if (other.id == node.id || other.type == "Reference" || !Doc().Runtime().ResourceOf(other.id)) continue;
                            const bool looped = !Doc().Runtime().TurnsOf(other.id).empty();
                            if ((port.name == "of" && !looped) || (port.name == "first" && looped)) continue;
                            ids.push_back(other.id);
                        }
                    return Choice(std::move(ids), value, set, ids.empty() ? "no node it can be" : "a node");
                }
                if (port.name == "format" && node.type == "Image" && port.builder && port.builder->enumeration) {
                    // The formats an image of its use can have: one drawn into as depth, the depth formats; one drawn
                    // into or written by a shader, none that is depth or packed.
                    std::set<std::string> uses;
                    for (auto& made : Doc().Runtime().UsageOf(node.id)) uses.insert(std::move(made));
                    if (const auto usage = node.inputs.find("usage"); usage != node.inputs.end()) {
                        if (const auto* items = usage->second.value.Items()) { for (const auto& item : *items) if (item.String()) uses.insert(*item.String()); }
                        else if (usage->second.value.String()) uses.insert(*usage->second.value.String());
                    }
                    const bool depth = uses.contains("eDepthStencilAttachment"), written = uses.contains("eColorAttachment") || uses.contains("eStorage");
                    std::vector<std::string> names;
                    for (const auto& entry : port.builder->enumeration->values) {
                        const std::string format(entry.name);
                        const bool ofDepth = (format.starts_with("eD") && format.size() > 2 && std::isdigit(static_cast<unsigned char>(format[2]))) || format.starts_with("eS8");
                        const bool packed = format.find("BC") != std::string::npos || format.find("ASTC") != std::string::npos || format.find("ETC") != std::string::npos;
                        if (depth ? !ofDepth : written && (ofDepth || packed)) continue;
                        names.push_back(format);
                    }
                    return Choice(std::move(names), value, set, depth ? "a depth format" : "a format");
                }
                if (port.editor == "components") return ComponentPicker(Doc().ComponentNames(), value, set, key, true);
                if (port.editor == "component") return ComponentPicker(Doc().ComponentNames(), value, set, key, false);
                if (port.editor == "componentFields") {
                    // The fields of the component named beside it that a buffer holds, and where the entity is: ticked,
                    // when nothing is said, as many as it holds then.
                    const auto beside = node.inputs.find("component");
                    const std::string* component = beside == node.inputs.end() ? nullptr : beside->second.value.String();
                    const kecs::ComponentId id = component ? Doc().World().Find(*component) : kecs::NoComponent;
                    if (id == kecs::NoComponent) return kgui::Muted("name a component first");
                    std::vector<std::string> names, unticked;
                    for (const auto& field : Doc().World().Describe(id).fields)
                        if (!kgraph::LayoutTypeOf(field.type).empty()) names.push_back(field.name);
                    for (const auto& [place, type] : kgraph::PlaceFields())
                        if (!std::ranges::contains(names, place)) (kgraph::PlacedByDefault(place) ? names : unticked).emplace_back(place);
                    Value ticked = value;
                    if (!value.Items()) ticked = Value(Value::List(names.begin(), names.end()));
                    names.insert(names.end(), unticked.begin(), unticked.end());
                    return Ticks(std::move(names), ticked, set);
                }
                if (port.editor == "script") {
                    // The project's scripts that fill a resource: what this node can run.
                    std::vector<std::string> names;
                    for (const auto& script : Doc().ResourceScripts()) names.push_back(script.name);
                    std::ranges::sort(names);
                    return Choice(std::move(names), value, set, "a ResourceScript");
                }
                if (port.editor == "entity") {
                    // The entities that have the component named beside it: any other could not be the one.
                    const auto beside = node.inputs.find("component");
                    const std::string* component = beside == node.inputs.end() ? nullptr : beside->second.value.String();
                    std::vector<std::string> names;
                    for (const kecs::Entity entity : Doc().Entities())
                        if (const std::string name = Doc().NameOf(entity); std::ranges::find(names, name) == names.end()
                            && (!component || component->empty() || std::ranges::contains(Doc().ComponentsOf(entity), *component))) names.push_back(name);
                    return Choice(std::move(names), value, set, "the first that has it");
                }
                if (port.editor == "parameter") {
                    // The image parameters of the pipeline this pass runs.
                    std::vector<std::string> names;
                    // A ComputePass is its pipeline as well: its own parameters.
                    std::string pipelineId = node.type == "ComputePass" ? node.id : std::string();
                    if (const auto pipeline = node.inputs.find("pipeline"); pipeline != node.inputs.end() && pipeline->second.link) pipelineId = pipeline->second.link->node;
                    if (!pipelineId.empty())
                        for (const auto& each : Doc().Runtime().PortsOf(pipelineId))
                            if (each.name.starts_with("param.") && each.type == "Image" && !each.name.ends_with(".sampler") && !std::ranges::contains(names, each.name.substr(6))) names.push_back(each.name.substr(6));
                    return Choice(std::move(names), value, set, "an image parameter");
                }
                if (port.editor == "attributes") return Attributes(value, set, key);
                if (port.editor == "fields") return Fields(value, set, key);
                if (port.editor == "names") return Names(value, set, key);
                if (port.editor == "numbers") {
                    // A few numbers that are one thing: a box (x, y, width, height), a depth bias's three.
                    std::vector<float> numbers(port.name == "depthBias" ? 3 : 4, 0.f);
                    if (const auto* items = value.Items())
                        for (std::size_t i = 0; i < numbers.size() && i < items->size(); ++i) numbers[i] = NumberOf((*items)[i]);
                    return DragNumbers(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); },
                                       LimitsOfPort(port), port.name == "depthBias" ? kui::NumberType::eFloat : kui::NumberType::eInt);
                }
                return port.builder ? TypeEditor(*port.builder, value, set, key, LimitsOfPort(port)) : NamedEditor(port.type, value, set, key, LimitsOfPort(port), NumberTypeOf(port));
            }

            /** Whether an input of a node is one of its shaders: shown as a slot, and not among its other inputs. */
            static bool IsShaderInput(const kgraph::Node& node, const std::string_view name)
            {
                if (node.type == "ComputePass") return name == "module" || name == "entry";
                if (node.type != "GraphicsPipeline" && node.type != "ComputePipeline" && node.type != "RayTracingPipeline") return false;
                return name.ends_with(".module") || name.ends_with(".entry");
            }

            /**
             * A slot of a pipeline for a shader of @p stage: shown as a shader's file is. Pressed, the entry points of that
             * stage to pick from, and only those; what is dropped on it from the Assets panel goes in when it is of that stage.
             */
            kui::Widget ShaderSlot(const kgraph::Node& node, const std::string& slot, const std::string& stage, const std::string& empty, std::vector<kui::MenuItem> menu = {})
            {
                const auto input = [&](const char* part) -> std::string {
                    const auto it = node.inputs.find(slot.empty() ? std::string(part) : std::format("{}.{}", slot, part));
                    return it != node.inputs.end() && it->second.value.String() ? *it->second.value.String() : std::string();
                };
                const std::string module = input("module"), entry = input("entry"), id = node.id, key = std::format("{}/{}", id, slot);
                const bool given = !module.empty();
                const bool open = _shaderPicking == key;
                std::vector<kui::Widget> lines;
                const auto choices = _session->ShadersOf(stage);
                for (const auto& [from, name] : choices)
                    lines.push_back(kui::Button(kui::Row({ AssetsView::StageIcon(stage, 16.f), kui::Text(name, kui::TextStyle {}.SetSize(13.f)), kgui::Muted(from, 11.f) },
                                                         kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)),
                                                [this, id, slot, from, name] { _session->SetShader(id, slot, from, name); SetState([&] { _shaderPicking.clear(); }); },
                                                kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)));
                if (choices.empty()) lines.push_back(kgui::Muted(std::format("No {} shader in the project's shaders.", AssetsView::StageOf(stage).name), 12.f));
                if (given) {
                    if (const std::filesystem::path file = _session->CurrentProject().ShaderDirectory() / (module + ".slang"); std::filesystem::exists(file))
                        menu.insert(menu.begin(), { "Open in Visual Studio Code", [this, file] { _session->OpenInCode(file); } });
                }
                kui::Widget tile = _assets.ShaderTile(key, stage, given ? (entry.empty() ? std::format("{}Main", stage) : entry) : empty, module, given,
                                                      [this, key] { SetState([&] { _shaderPicking = _shaderPicking == key ? std::string() : key; }); }, std::move(menu));
                tile = kui::Tooltip(given ? AssetsView::StageOf(stage).name : std::format("{}: pick one, or drop one here from the Assets panel.", AssetsView::StageOf(stage).name), std::move(tile));
                tile = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, id, slot, stage](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* asset = dragged.As<std::string>()) _session->DropShader(id, slot, stage, *asset);
                }), std::move(tile));
                return kui::Column({ std::move(tile),
                                     kui::PopupAnchor(open, PopupPanel(kui::Column(std::move(lines), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f)), 260.f),
                                                      [this] { SetState([&] { _shaderPicking.clear(); }); }) },
                                   kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart));
            }

            /** What a pipeline is made of, as slots: VTG's optional stages hidden until asked for, a ray tracer's in lists. */
            kui::Widget ShaderSlots(const kgraph::Node& node)
            {
                const std::string id = node.id;
                const auto clear = [this, id](std::vector<std::string> slots, std::string asked) {
                    return [this, id, slots = std::move(slots), asked = std::move(asked)] {
                        for (const auto& slot : slots) _session->ClearShader(id, slot);
                        if (!asked.empty()) _session->Hide(id, asked);
                    };
                };
                const auto given = [&](const char* slot) { return node.inputs.contains(std::format("{}.module", slot)); };
                const auto wrap = [](std::vector<kui::Widget> tiles) { return kui::Wrap(std::move(tiles), 6.f, 6.f); };
                std::vector<kui::Widget> rows { Heading("Shaders") };
                if (node.type == "ComputePipeline") rows.push_back(wrap({ ShaderSlot(node, "compute", "compute", "No compute shader") }));
                else if (node.type == "ComputePass") rows.push_back(wrap({ ShaderSlot(node, "", "compute", "No compute shader") }));
                else if (node.type == "GraphicsPipeline") {
                    std::vector<kui::Widget> tiles, offers;
                    const auto fragment = [&] {
                        return ShaderSlot(node, "fragment", "fragment", "Depth only", given("fragment") ? std::vector<kui::MenuItem> { { "Depth only: no fragment shader", clear({ "fragment" }, {}) } } : std::vector<kui::MenuItem> {});
                    };
                    if (kgraph::StagesOf(node) == "eMesh") {
                        if (given("task") || _session->Asked(id, "task")) tiles.push_back(ShaderSlot(node, "task", "task", "No task shader", { { "Remove the task shader", clear({ "task" }, "task") } }));
                        else offers.push_back(Chip("Add a task shader", [this, id] { _session->Show(id, "task"); }));
                        tiles.push_back(ShaderSlot(node, "mesh", "mesh", "No mesh shader"));
                    } else {
                        tiles.push_back(ShaderSlot(node, "vertex", "vertex", "No vertex shader"));
                        if (given("tessellationControl") || given("tessellationEvaluation") || _session->Asked(id, "tessellation")) {
                            std::vector<kui::MenuItem> remove { { "Remove tessellation", clear({ "tessellationControl", "tessellationEvaluation" }, "tessellation") } };
                            tiles.push_back(ShaderSlot(node, "tessellationControl", "hull", "No hull shader", remove));
                            tiles.push_back(ShaderSlot(node, "tessellationEvaluation", "domain", "No domain shader", remove));
                        } else offers.push_back(Chip("Add tessellation", [this, id] { _session->Show(id, "tessellation"); }));
                        if (given("geometry") || _session->Asked(id, "geometry")) tiles.push_back(ShaderSlot(node, "geometry", "geometry", "No geometry shader", { { "Remove the geometry shader", clear({ "geometry" }, "geometry") } }));
                        else offers.push_back(Chip("Add a geometry shader", [this, id] { _session->Show(id, "geometry"); }));
                    }
                    tiles.push_back(fragment());
                    rows.push_back(wrap(std::move(tiles)));
                    if (!offers.empty()) rows.push_back(kui::Wrap(std::move(offers), 6.f, 4.f));
                } else {
                    rows.push_back(wrap({ ShaderSlot(node, "raygen", "raygeneration", "No ray generation shader") }));
                    const auto list = [&](const char* title, const std::string& name, const char* stage, const char* empty) {
                        rows.push_back(kgui::Muted(title, 12.f));
                        std::vector<kui::Widget> tiles;
                        const int size = _session->ShaderListSize(id, name);
                        for (int i = 0; i < size; ++i)
                            tiles.push_back(ShaderSlot(node, std::format("{}[{}]", name, i), stage, empty,
                                                       { { "Remove from the list", [this, id, name, i] { _session->RemoveFromShaderList(id, name, i); } } }));
                        if (!tiles.empty()) rows.push_back(wrap(std::move(tiles)));
                        rows.push_back(Chip(std::format("Add a {} shader", name), [this, id, name] { _session->AddToShaderList(id, name); }));
                    };
                    list("Miss", "miss", "miss", "No miss shader");
                    rows.push_back(kgui::Muted("Hit groups", 12.f));
                    for (int group = 0; group < _session->ShaderListSize(id, "hitGroup"); ++group) {
                        std::vector<kui::MenuItem> remove { { "Remove this hit group", [this, id, group] { _session->RemoveFromShaderList(id, "hitGroup", group); } } };
                        rows.push_back(kui::Row({ kui::Expanded(kui::Text(std::format("Group {}", group), kui::TextStyle {}.SetSize(12.f))),
                                                  Chip("Remove", [this, id, group] { _session->RemoveFromShaderList(id, "hitGroup", group); }) },
                                                kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                        rows.push_back(wrap({ ShaderSlot(node, std::format("hitGroup[{}].closestHit", group), "closesthit", "No closest hit", remove),
                                              ShaderSlot(node, std::format("hitGroup[{}].anyHit", group), "anyhit", "No any hit", remove),
                                              ShaderSlot(node, std::format("hitGroup[{}].intersection", group), "intersection", "Triangles", remove) }));
                    }
                    rows.push_back(Chip("Add a hit group", [this, id] { _session->AddToShaderList(id, "hitGroup"); }));
                    list("Callable", "callable", "callable", "No callable shader");
                }
                return Rows(std::move(rows), 6.f);
            }

            kui::Widget NodeInspector(const kgraph::Node& node)
            {
                // A Reference is what it stands for, brought in: what it is here, and under that the thing itself, to set
                // up where it stands — every page that brings it in has what is set.
                if (node.type == "Reference")
                    if (const kgraph::Node* target = StandsFor(node); target && target != &node) {
                        const kui::Theme& t = kui::Theme::Current();
                        const UseHere use = UsesAt(target->id, node.pass);
                        const bool setup = Doc().IsSetup(target->pass);
                        const std::string from = target->pass.empty() ? std::string("in no pass") : setup ? std::format("made once, in setup's {}", target->pass) : std::format("from {}", target->pass);
                        std::vector<kui::Widget> said {
                            kui::Text(std::format("Brought in: {}, {}", target->id, from), kui::TextStyle {}.SetSize(12.5f).SetColor(t.text)),
                            kgui::Muted(std::format("On {}, it is {}. {}", node.pass.empty() ? std::string("this page") : node.pass, UsedHere(use), WrittenBefore(use)), 11.f),
                            kgui::Muted("What is set below is its own: changed here, it changes where it is made, and on every page that brings it in.", 11.f),
                            kui::Row({ Small("Go to where it is made", [this, id = target->id] { _session->GoTo(id); }) }),
                        };
                        if (const std::string holds = Holds(target->id); !holds.empty()) said.insert(said.begin() + 1, kgui::Muted(holds, 11.f));
                        return Rows({ kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::All(10.f))
                                                         .SetDecoration(kui::Decoration {}.SetColor((setup ? kui::Color::Hex(0x3E9FA8) : t.primary).WithAlpha(0.10f)).SetRadius(6.f)),
                                                     Rows(std::move(said), 4.f)),
                                      NodeInspector(*target) }, 10.f);
                    }
                const kgraph::Runtime& runtime = Doc().Runtime();
                const std::string id = node.id;
                std::vector<kui::Widget> rows;
                rows.push_back(Heading(id));
                rows.push_back(kgui::Muted(node.type));
                rows.push_back(kui::Tooltip("Its name: what wires, files and components know it by. Changed, they follow; a script that reaches it by name does not.",
                    kgui::Labeled("Name", kui::TextField(kui::TextFieldOptions { .text = id, .placeholder = id,
                        .onSubmitted = [this, id](const std::string& typed) {
                            std::string why;
                            if (typed == id) return;
                            if (Doc().RenameNode(id, typed, &why)) _session->Select(typed);
                            else _session->Say(std::format("'{}' is not renamed: {}", id, why), true);
                        } }).Key("name " + id), LabelWidth)));
                if (node.type != "Reference")
                    rows.push_back(kui::Tooltip("What it is called on the graph. Its name stays what wires, files and components know it by.",
                        kgui::Labeled("Title", kui::TextField(kui::TextFieldOptions { .text = node.title, .placeholder = id,
                            .onSubmitted = [this, id](const std::string& typed) { Doc().SetTitle(id, typed); } }).Key("title " + id), LabelWidth)));
                if (const auto& passes = Doc().Passes(); !passes.empty()) {
                    std::vector<std::string> choices { "(in no pass)" };
                    choices.insert(choices.end(), passes.begin(), passes.end());
                    const auto at = std::ranges::find(passes, node.pass);
                    rows.push_back(kui::Tooltip("The pass whose page it is on.", kgui::Labeled("Pass", kui::Dropdown(choices, at == passes.end() ? 0 : static_cast<int>(at - passes.begin()) + 1,
                        [this, id, choices](const int index) {
                            Doc().SetNodePass(id, index <= 0 ? std::string() : choices[static_cast<std::size_t>(index)]);
                            _session->GoTo(id);
                        }), LabelWidth)));
                }
                if (const auto* status = runtime.Status(id); status && !status->ok) rows.push_back(Problem(status->error));
                else if (status && !status->note.empty()) rows.push_back(kgui::Muted(status->note, 11.f));
                // A Draw or a Dispatch of many elements: one of them all, or what it would take to be.
                if (const auto batching = runtime.BatchingOf(id)) {
                    const char* one = node.type == "Draw" ? "draw" : "dispatch";
                    if (batching->batched) {
                        rows.push_back(kgui::Muted(std::format("Batched: one {} of {} {}.", one, batching->elements, batching->elements == 1 ? "element" : "elements"), 11.f));
                    } else {
                        const kui::Theme& t = kui::Theme::Current();
                        std::vector<kui::Widget> said { kui::Text(std::format("{} {} a frame, one an element.", batching->elements, node.type == "Draw" ? "draws" : "dispatches"),
                                                                  kui::TextStyle {}.SetSize(12.5f).SetColor(t.text)) };
                        const auto* status = runtime.Status(id);
                        if (batching->offered)
                            said.push_back(kui::Row({ Small(std::format("Batch: one {} of them all", one), [this, id] { _session->Batch(id); }) }));
                        else if (!batching->why.empty() && !(status && status->note == batching->why))
                            said.push_back(kgui::Muted(batching->why, 11.f));
                        rows.push_back(kui::Container(kui::ContainerOptions {}.SetPadding(kui::EdgeInsets::All(10.f))
                                                          .SetDecoration(kui::Decoration {}.SetColor(t.primary.WithAlpha(0.10f)).SetRadius(6.f)),
                                                      Rows(std::move(said), 4.f)));
                    }
                }
                // What a pass costs, followed as it runs: the GPU's time, and the CPU's recording it.
                if (const kgraph::NodeType* type = kgraph::FindType(node.type); type && type->category == "Passes")
                    rows.push_back(kui::Tooltip("Averaged over recent frames. The GPU's time arrives a few frames late; a fixed page's steps are added together.",
                        kgui::Labeled("Takes", kui::Make<LiveValue>(_session, [this, id] {
                            const auto timings = Doc().Runtime().Timings();
                            const auto it = timings.find(id);
                            if (it == timings.end()) return Value("not run");
                            const auto& t = it->second;
                            return Value(std::format("GPU {} · record {:.3f} ms{}", t.gpuMeasured ? std::format("{:.3f} ms", t.gpuMs) : std::string("not measured"), t.recordMs,
                                                     t.runs > 1 ? std::format(" · {} steps at most", t.runs) : std::string()));
                        }, [](const Value& said) { return kgui::Line(said.String() ? *said.String() : std::string()); }), LabelWidth)));
                // What the script it runs makes, and whether anything runs it.
                if (node.type == "Script")
                    if (const auto named = node.inputs.find("script"); named != node.inputs.end() && named->second.value.String())
                        for (const auto& script : Doc().ResourceScripts())
                            if (script.name == *named->second.value.String()) {
                                std::string makes;
                                for (const auto& resource : script.resources) makes += std::format("{}{} ({})", makes.empty() ? "" : ", ", resource.name, resource.kind);
                                rows.push_back(kgui::Muted(std::format("{} makes {}{}", script.name, makes.empty() ? std::string("nothing") : makes,
                                                                       script.fill ? "." : ". Nothing runs it now: it fills them when the project runs with its scripts."), 11.f));
                            }
                if (const auto* report = runtime.ReportOf(id)) {
                    rows.push_back(kgui::Muted(std::format("Runs for {} {}.", report->ran + report->culled, report->ran + report->culled == 1 ? "entity" : "entities")));
                    if (report->skipped > 0) rows.push_back(Problem(std::format("{} passed by. {}", report->skipped, report->why)));
                    else if (!report->why.empty()) rows.push_back(Problem(report->why));
                }
                if (const kecs::ComponentId component = runtime.ComponentOf(id); component != kecs::NoComponent)
                    rows.push_back(kgui::Muted(std::format("What its draws take from each entity is the component '{}': a ForEach of it gives its fields.",
                                                           Doc().World().Describe(component).name)));
                rows.push_back(kui::Separator());

                // Its shaders, each in a slot of its stage.
                if (node.type == "GraphicsPipeline" || node.type == "ComputePipeline" || node.type == "RayTracingPipeline" || node.type == "ComputePass") {
                    rows.push_back(ShaderSlots(node));
                    rows.push_back(kui::Separator());
                }

                // Every input it has, given or not — one not given as it starts, its name dimmed — and every output.
                const kgraph::NodeType* nodeType = kgraph::FindType(node.type);
                auto inputs = InputsOf(runtime, node, [](const std::string&) { return true; });
                std::erase_if(inputs, [&](const Shown& shown) { return IsShaderInput(node, shown.port.name); });
                // An input given in parts is shown in its parts: the whole is there only once it is given — wired.
                std::erase_if(inputs, [&](const Shown& shown) {
                    return !shown.given && nodeType && !kgraph::PartsOf(*nodeType, shown.port.name).empty();
                });
                // What it gives: an output that is one of its inputs as it is set — `extent`, `format` — is that input's to put on
                // the node, from its row; the rest are listed under What it gives.
                const auto outputs = runtime.OutputsOf(id);
                std::set<std::string, std::less<>> inputNames;
                for (const auto& port : runtime.PortsOf(id)) inputNames.insert(port.name);
                if (nodeType) for (const auto& port : nodeType->inputs) inputNames.insert(port.name);     // another way of drawing's too
                const auto wiresFrom = [&](const std::string& output) {
                    std::size_t wires = 0;
                    for (const auto& other : Doc().Graph().nodes)
                        for (const auto& input : other.inputs | std::views::values)
                            if (input.link && input.link->node == id && input.link->output == output) ++wires;
                    return wires;
                };
                // An input's own output: on the node to wire from, or not; off, its wires come off with it.
                const auto givesToo = [&](const std::string& name) -> std::optional<kui::Widget> {
                    const auto output = std::ranges::find(outputs, name, &kgraph::Port::name);
                    if (output == outputs.end() || !output->optional) return std::nullopt;
                    const std::size_t wires = wiresFrom(name);
                    const bool onNode = wires > 0 || _session->Asked(id, "out:" + name);
                    const std::string tip = wires > 0 ? std::format("Given as an output, wired to {} {}: press to take it off, and its wires", wires, wires == 1 ? "input" : "inputs")
                                          : onNode ? "Given as an output on the node: press to take it off" : "Give it as an output on the node, to wire what it is set to elsewhere";
                    return kui::Tooltip(tip, kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon("output", kui::IconStyle::eFilled, onNode ? kui::Theme::Current().primary : kui::Theme::Current().textMuted)),
                                        [this, id, name, onNode] { _session->Hide(id, "out:" + name); if (onNode) Doc().DisconnectOutput(id, name); else _session->Show(id, "out:" + name); },
                                        kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f))));
                };
                const auto label = [](const std::string& text, const bool given, kui::Widget editor) {
                    const auto& theme = kui::Theme::Current();
                    return kui::Row({ kui::SizedBox(LabelWidth, -1.f, kgui::Line(text, kui::TextStyle {}.SetColor(given ? theme.text : theme.textMuted))),
                                      kui::Expanded(std::move(editor)) }, kui::FlexOptions {}.SetGap(8.f));
                };
                const auto set = [this, id](const std::string& name) {
                    return [this, id, name](Value changed) {
                        if (changed.IsNull()) Doc().ClearInput(id, name);
                        else Doc().SetInput(id, name, std::move(changed));
                    };
                };
                // Back to nothing given; one that can be done without goes from the node with it.
                const auto remove = [this, id](const std::vector<std::string>& names) {
                    return [this, id, names] {
                        for (const auto& name : names) { Doc().ClearInput(id, name); _session->Hide(id, name); }
                    };
                };
                const auto plain = [&](const Shown& shown) {    // set here, with a control: not wired, and not left open for a wire
                    if (shown.port.kind != kgraph::PortKind::eValue) return false;
                    const auto input = node.inputs.find(shown.port.name);
                    return input == node.inputs.end() || (!input->second.link && !input->second.value.IsNull());
                };

                // Each in its part of the inspector: those of no part first, then each part under a heading of its own, in the
                // order its first input comes — closed until something in it is given, or it is opened.
                std::vector<std::string> groups;
                std::map<std::string, std::vector<kui::Widget>, std::less<>> grouped;
                std::map<std::string, std::pair<int, int>, std::less<>> counted;    // of each part: how many are given, of how many
                std::set<std::string, std::less<>> needed;                         // the parts with what the node cannot do without
                std::map<std::string, std::string, std::less<>> idleSays;           // of a part with nothing shown: what it is for
                for (std::size_t at = 0; at < inputs.size(); ++at) {
                    const Shown& shown = inputs[at];
                    const kgraph::Port& port = shown.port;
                    if (!port.group.empty() && !grouped.contains(port.group)) groups.push_back(port.group);
                    std::vector<kui::Widget>& out = port.group.empty() ? rows : grouped[port.group];
                    if (!port.group.empty()) {
                        counted[port.group].second += 1;
                        if (shown.given) counted[port.group].first += 1;
                        if (!port.optional) needed.insert(port.group);
                    }
                    const std::string name = port.name;
                    const auto input = node.inputs.find(name);

                    // Numbers that are one thing's parts — extent.x, extent.y, extent.z — on one line, under its name.
                    if (name.ends_with(".x") && plain(shown) && (port.type == "number")) {
                        const std::string prefix = name.substr(0, name.size() - 1);
                        std::vector<std::string> parts { name };
                        for (const char* axis : { "y", "z", "w" }) {
                            const std::size_t next = at + parts.size();
                            if (next < inputs.size() && inputs[next].port.name == prefix + axis && plain(inputs[next])) parts.push_back(prefix + axis);
                            else break;
                        }
                        if (parts.size() > 1) {
                            std::vector<float> numbers;
                            bool any = false;
                            for (const auto& part : parts) {
                                const auto each = node.inputs.find(part);
                                any = any || each != node.inputs.end();
                                const auto listed = std::ranges::find_if(inputs, [&](const Shown& other) { return other.port.name == part; });
                                numbers.push_back(each != node.inputs.end() ? NumberOf(each->second.value) : listed != inputs.end() ? NumberOf(listed->port.initial) : 0.f);
                            }
                            auto editor = DragNumbers(numbers, [this, id, parts, numbers](const std::vector<float>& changed) {
                                for (std::size_t i = 0; i < parts.size(); ++i)
                                    if (changed[i] != numbers[i]) Doc().SetInput(id, parts[i], Value(changed[i]));
                            }, LimitsOfPort(port), NumberTypeOf(port));
                            std::vector<kui::Widget> line { kui::Expanded(std::move(editor)) };
                            // Given whole instead: a port for a vector worked out elsewhere, on one wire.
                            const std::string whole = prefix.substr(0, prefix.size() - 1);
                            if (nodeType && std::ranges::contains(kgraph::PartsOf(*nodeType, whole), name))
                                line.push_back(kui::Tooltip("Wire it whole: one vector for all its parts", SmallIcon("link", [this, id, whole, parts] {
                                    for (const auto& part : parts) Doc().ClearInput(id, part);
                                    Doc().SetInput(id, whole, Value {});
                                })));
                            if (auto gives = givesToo(whole)) line.push_back(std::move(*gives));
                            if (any) line.push_back(kui::Tooltip("Back to nothing given", SmallIcon("close", remove(parts))));
                            out.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc, label(whole, any, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)))));
                            at += parts.size() - 1;
                            continue;
                        }
                    }

                    kui::Widget editor;
                    const kgraph::Placement placement = kgraph::PlacementOf(port, node);
                    if (placement == kgraph::Placement::eWire && !(shown.given && input->second.link) && port.kind == kgraph::PortKind::eValue) {
                        // Wired only: from the node that sets it up, made here when it has one, or from what works one out.
                        std::vector<kui::Widget> wire { kui::Expanded(kgui::Muted("wire one in")) };
                        if (!port.companion.empty())
                            wire.push_back(kui::Tooltip(std::format("Make a {} beside this node, wired in: where it is set up", port.companion),
                                Small(std::format("Make a {}", port.companion), [this, id, name] {
                                    std::string error;
                                    const std::string made = Doc().MakeCompanion(id, name, &error);
                                    if (made.empty()) _session->Say(error, true);
                                    else _session->Select(made);
                                })));
                        editor = kui::Row(std::move(wire), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                    } else if (shown.given && input->second.link) {
                        editor = kgui::Line(std::format("from {}{}", input->second.link->node, input->second.link->output == "out" ? "" : "." + input->second.link->output));
                    } else if (port.kind == kgraph::PortKind::eResource) {
                        const std::string wanted = port.type == "Resource" || port.type == "value" ? std::string("wire one in") : std::format("wire one in: {}", port.type);
                        // What it can be wired from, chosen here as well as drawn on the graph: the nodes that give
                        // what it takes, of this page or made before it — one of another pass is brought in.
                        if (const auto from = _session->WireChoices(id, name); !from.empty()) {
                            std::vector<std::string> labels;
                            for (const auto& other : from) {
                                const kgraph::Node* source = Doc().Graph().Find(other);
                                labels.push_back(source && !source->pass.empty() && source->pass != node.pass ? std::format("{}   ·   from {}", other, source->pass) : other);
                            }
                            editor = kui::Dropdown(std::move(labels), -1, [this, id, name, from](const int index) { _session->WireFrom(id, name, from[static_cast<std::size_t>(index)]); },
                                                   kui::DropdownOptions {}.SetPlaceholder(wanted));
                        } else editor = kgui::Muted(wanted + " (nothing gives one yet)");
                        // A buffer a shader declares can be made as the shader lays it out: a node with the struct's
                        // fields as inputs, wired here, for the data to be wired to.
                        if (port.type == "Buffer" && name.starts_with("param."))
                            editor = kui::Row({ kui::Expanded(std::move(editor)),
                                                kui::Tooltip("Make a node laid out as the shader declares this buffer: an input for each field of its struct, wired here",
                                                             Small("Make its buffer", [this, id, name] {
                                                                 std::string error;
                                                                 const std::string made = Doc().MakeBufferFor(id, name, &error);
                                                                 if (made.empty()) _session->Say(error, true);
                                                                 else { _session->Select(made); _session->Say(std::format("Made '{}': wire what each of its fields is.", made)); }
                                                             })) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter));
                    } else if (shown.given && input->second.value.IsNull()) {
                        editor = kgui::Muted("a port: wire a value in");
                    } else {
                        // Not given, it shows what it is all the same: what the node type says it starts as.
                        editor = EditorFor(node, port, shown.given ? input->second.value : port.initial, set(name), id + "/" + name);
                    }
                    std::vector<kui::Widget> line { kui::Expanded(std::move(editor)) };
                    // Where it is set: on the node, with a control in its row, or here. One wired only is neither.
                    if (port.kind == kgraph::PortKind::eValue && placement != kgraph::Placement::eWire && port.editor != "attributes") {
                        const bool onNode = placement == kgraph::Placement::eNode;
                        line.push_back(kui::Tooltip(onNode ? "Set on the node: a control in its row. Press to set it only here" : "Set it on the node as well: a control in its row, and a port to wire",
                            kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(onNode ? "push_pin" : "input", kui::IconStyle::eFilled, onNode ? kui::Theme::Current().primary : kui::Theme::Current().textMuted)),
                                        [this, id, name, onNode] { Doc().SetPlacement(id, name, onNode ? kgraph::Placement::eInspector : kgraph::Placement::eNode); },
                                        kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)))));
                    }
                    // A value a node can work out — a number, a vector, a colour — can be wired instead: a name chosen from a list cannot.
                    // (One set on the node is a port already.)
                    if (placement != kgraph::Placement::eNode && plain(shown) && port.choices.empty() && (port.editor.empty() || port.editor == "color" || port.editor == "numbers"))
                        line.push_back(kui::Tooltip("Make it a port, to wire a value to", SmallIcon("link", [this, id, name] { Doc().SetInput(id, name, Value {}); })));
                    // One it can do without that takes a resource: a port on the node when it is asked to be, to wire from the graph.
                    if (port.kind == kgraph::PortKind::eResource && port.optional && !shown.given) {
                        const bool shownOnNode = _session->Asked(id, name);
                        line.push_back(kui::Tooltip(shownOnNode ? "A port on the node: press to take it off" : "Put a port for it on the node, to wire from the graph",
                            kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(shownOnNode ? "push_pin" : "input", kui::IconStyle::eFilled, shownOnNode ? kui::Theme::Current().primary : kui::Theme::Current().textMuted)),
                                        [this, id, name, shownOnNode] { if (shownOnNode) _session->Hide(id, name); else _session->Show(id, name); },
                                        kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)))));
                    }
                    if (auto gives = givesToo(name)) line.push_back(std::move(*gives));
                    if (shown.given)
                        line.push_back(kui::Tooltip("Back to nothing given: as it starts", SmallIcon("close", remove({ name }))));
                    // What takes the whole width to set — a vertex's attributes, a view's swizzle — is under its name, not beside it.
                    const bool swizzle = port.builder && port.builder->kind == kb::Kind::eArray && port.builder->element && port.builder->element->enumeration
                        && std::string_view(port.builder->element->enumeration->name).find("Swizzle") != std::string_view::npos;
                    if (port.editor == "attributes" || swizzle) {
                        const auto& theme = kui::Theme::Current();
                        out.push_back(kui::Row({ kui::Expanded(kgui::Line(PortLabel(name), kui::TextStyle {}.SetColor(shown.given ? theme.text : theme.textMuted))),
                                                  kui::Row(std::vector<kui::Widget>(line.begin() + 1, line.end()), kui::FlexOptions {}.SetGap(4.f)) },
                                                kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                        out.push_back(std::move(line.front()));
                        continue;
                    }
                    out.push_back(kui::Tooltip(port.doc.empty() ? port.type : port.doc + " (" + port.type + ")",
                                                label(PortLabel(name), shown.given, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)))));
                }

                // What is not shown because it does nothing as the node is set: said, by why, in the part it would be in.
                {
                    std::vector<kgraph::Port> idle;
                    (void)InputsOf(runtime, node, [](const std::string&) { return true; }, &idle);
                    std::erase_if(idle, [&](const kgraph::Port& port) { return IsShaderInput(node, port.name); });
                    std::vector<std::tuple<std::string, std::string, int>> left;     // a part, why, and how many: in the order they come
                    for (const auto& port : idle) {
                        if (port.when.input.empty() || node.inputs.contains(port.name)) continue;
                        // Of an input in parts, the whole once.
                        if (port.name.size() > 2 && port.name[port.name.size() - 2] == '.' && std::ranges::any_of(idle, [&](const kgraph::Port& other) { return other.name + "." + port.name.back() == port.name; })) continue;
                        const auto known = std::ranges::find_if(left, [&](const auto& each) { return std::get<0>(each) == port.group && std::get<1>(each) == port.when.what; });
                        if (known == left.end()) left.emplace_back(port.group, port.when.what, 1);
                        else ++std::get<2>(*known);
                    }
                    for (const auto& [group, what, count] : left) {
                        const std::string said = std::format("{} more {}{}", count, count == 1 ? "input " : "", what.empty() ? "as it is set" : what);
                        if (!group.empty() && !grouped.contains(group)) groups.push_back(group);
                        if (!group.empty() && counted[group].second == 0) { idleSays[group] += (idleSays[group].empty() ? "" : ", ") + said; grouped[group]; continue; }
                        (group.empty() ? rows : grouped[group]).push_back(kgui::Muted(said, 11.f));
                    }
                }
                for (const auto& group : groups) {
                    auto& within = grouped[group];
                    const auto [given, of] = counted[group];
                    const auto& theme = kui::Theme::Current();
                    // Nothing in it does anything as the node is set: its heading says what it is for, and there is nothing to open.
                    if (of == 0) {
                        rows.push_back(kui::Row({ kui::SizedBox(16.f, 16.f), kui::Expanded(kgui::Line(PortLabel(group), kui::TextStyle {}.SetColor(theme.textMuted))),
                                                  kgui::Muted(idleSays[group], 11.f) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                        continue;
                    }
                    const bool open = _session->SectionOpen(id, group, given > 0 || needed.contains(group));
                    const std::string says = given == 0 ? std::format("{}, as they start", of) : std::format("{} of {} given", given, of);
                    auto heading = kui::TextStyle {}.SetColor(given > 0 ? theme.text : theme.textMuted);
                    heading.weight = 600.f;
                    rows.push_back(kui::Button(kui::Row({ kui::SizedBox(16.f, 16.f, kui::Icon(open ? "expand_more" : "chevron_right", kui::IconStyle::eFilled, theme.textMuted)),
                                                          kui::Expanded(kgui::Line(PortLabel(group), heading)),
                                                          kgui::Muted(says, 11.f) },
                                                        kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)),
                                               [this, id, group, open] { _session->OpenSection(id, group, !open); },
                                               kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)));
                    if (open) rows.push_back(kui::Container(kui::ContainerOptions { .padding = kui::EdgeInsets::Only(12.f, 0.f, 0.f, 4.f) }, Rows(std::move(within), 6.f)));
                }

                // What it gives: what it makes, and its size, its settings. Each wired from on the graph; one it gives besides
                // what it makes is a port on the node once it is asked to be, or wired from.
                {
                    std::vector<kgraph::Port> made;
                    // What it gives that is none of its inputs, nor a part of one: what it makes, what it works out, what it is made as.
                    // A member of a Bundle it gives — a ForEach's field of a component, where it is — is listed once it is wired
                    // or put on the node: the rest are what taking its Bundle apart gives.
                    for (const auto& output : outputs) {
                        if (!output.group.empty() && output.optional && wiresFrom(output.name) == 0 && !_session->Asked(id, "out:" + output.name)) continue;
                        std::string whole = output.name;
                        if (whole.size() > 2 && whole[whole.size() - 2] == '.' && std::string_view("xyzw").find(whole.back()) != std::string_view::npos) whole.resize(whole.size() - 2);
                        if (!inputNames.contains(output.name) && !inputNames.contains(whole)) made.push_back(output);
                    }
                    if (!made.empty()) {
                        rows.push_back(kui::Separator());
                        rows.push_back(kgui::Muted("What it gives", 11.f));
                    }
                    for (const auto& output : made) {
                        const std::size_t wires = wiresFrom(output.name);
                        const bool onNode = !output.optional || wires > 0 || _session->Asked(id, "out:" + output.name);
                        // What it is now, for a value; what it is wired to.
                        std::string said = wires == 0 ? std::string() : std::format("wired to {} {}", wires, wires == 1 ? "input" : "inputs");
                        if (output.kind == kgraph::PortKind::eValue)
                            if (const Value now = runtime.ValueOf(id, output.name); !now.IsNull()) said = said.empty() ? Brief(now) : std::format("{}   ·   {}", Brief(now), said);
                        if (said.empty()) said = output.kind == kgraph::PortKind::eResource ? output.type : std::string("not worked out yet");
                        std::vector<kui::Widget> line { kui::Expanded(kgui::Muted(said)) };
                        if (output.optional && wires == 0)
                            line.push_back(kui::Tooltip(onNode ? "A port on the node: press to take it off" : "Put it on the node as a port, to wire from",
                                kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(onNode ? "push_pin" : "output", kui::IconStyle::eFilled, onNode ? kui::Theme::Current().primary : kui::Theme::Current().textMuted)),
                                            [this, id, name = output.name, onNode] { if (onNode) _session->Hide(id, "out:" + name); else _session->Show(id, "out:" + name); },
                                            kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::eSecondary).SetPadding(kui::EdgeInsets::Symmetric(4.f, 2.f)))));
                        if (wires > 0)
                            line.push_back(kui::Tooltip("Take its wires off", SmallIcon("close", [this, id, name = output.name] { _session->Hide(id, "out:" + name); Doc().DisconnectOutput(id, name); })));
                        // A Bundle — a component's fields, where it is — taken apart beside it: an output a member, to wire from.
                        if (output.type == "bundle")
                            line.push_back(kui::Tooltip("Take it apart: an Unbundle beside the node, with an output for each of its members",
                                Small("Take it apart", [this, id, name = output.name] {
                                    std::string error;
                                    const std::string made = Doc().TakeApart(id, name, &error);
                                    if (made.empty()) _session->Say(error, true);
                                    else { _session->Select(made); _session->Say(std::format("Took '{}' apart: '{}' gives each of its members.", name, made)); }
                                })));
                        rows.push_back(kui::Tooltip(output.doc.empty() ? output.type : output.doc + " (" + output.type + ")",
                                                    label(PortLabel(output.name), onNode, kui::Row(std::move(line), kui::FlexOptions {}.SetGap(4.f)))));
                    }
                }
                // What it holds, to look at: an image as a picture, a buffer as numbers. Copied out while it is looked at.
                if (const auto held = runtime.ResourceOf(id); held && held->kind == "Mesh") {
                    rows.push_back(kui::Separator());
                    rows.push_back(Small("Open in the viewer", [this, id] { _session->View(id); }));
                }
                if (const auto held = runtime.ResourceOf(id); held && (runtime.ImageOf(id).Get() || held->kind == "Buffer" || held->kind == "BufferView")) {
                    rows.push_back(kui::Separator());
                    rows.push_back(Small("Open in the viewer: every level, its histogram, a table", [this, id] { _session->View(id); }));
                }
                rows.push_back(kui::Separator());
                rows.push_back(Small("Remove the node", [this, id] { Doc().RemoveNodes({ id }); _session->Select(std::string()); }));
                return Panel(std::move(rows));
            }

            /** What a choice of a field that holds something is called: a file's or a mesh's own name, a plain picture as what it is, a node as the graph's. */
            std::string ChoiceLabel(const std::string& id)
            {
                if (id == "$white") return "white (plain)";
                if (id == "$black") return "black (plain)";
                if (id == "$normal") return "flat normal (plain)";
                if (id.starts_with('@') || id.starts_with("$script/gen/")) return _session->AssetLabel(id);
                if (id.starts_with('$')) return id.substr(1);
                if (Doc().Graph().Find(id)) return std::format("{}   ·   graph", id);
                return _session->AssetLabel(id);
            }

            kui::Widget EntityInspector(const kecs::Entity entity)
            {
                Document& doc = Doc();
                const kui::Theme& t = kui::Theme::Current();
                const kecs::World& world = doc.World();
                std::vector<kui::Widget> rows;
                const std::string name = doc.NameOf(entity);
                const std::string kind = _session->EntityKind(entity);
                const auto [icon, tint] = EntityIcon(kind);
                const bool can = _session->CanShow(entity), shown = !can || _session->IsShown(entity);

                // What it is, what it is called, and whether it is drawn.
                {
                    std::vector<kui::Widget> head;
                    head.push_back(kui::Tooltip(kind == "empty" ? std::string("An entity with nothing to it yet") : "A " + kind, kui::SizedBox(20.f, 20.f, kui::Icon(icon, kui::IconStyle::eFilled, tint))));
                    head.push_back(kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = name,
                        .onSubmitted = [this, entity](const std::string& typed) { Doc().Rename(entity, typed); } }).Key("name=" + name)));
                    if (can)
                        head.push_back(kui::Tooltip(shown ? "Drawn. Pressed: it and what is under it are not." : "Not drawn. Pressed: it and what is under it are.",
                                                    SmallIcon(shown ? "visibility" : "visibility_off", [this, entity, shown] { _session->SetShown(entity, !shown); })));
                    head.push_back(kui::Tooltip("Frame it in the scene view", SmallIcon("center_focus_strong", [this, entity] { _session->Frame(entity); })));
                    rows.push_back(kui::Row(std::move(head), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                }
                // A pass that could not run for it says why here, where it is mended: a field with nothing in it, a mesh that does not fit.
                for (const auto& [passed, why] : doc.Runtime().PassedBy())
                    if (passed == entity) rows.push_back(Problem(std::format("Not drawn by {}", why)));

                // What it is under: chosen among the entities it can go under — not itself, nor what is under it — or
                // dropped here from the outliner.
                {
                    std::set<kecs::Entity> below { entity };
                    std::vector<kecs::Entity> others;
                    std::vector<std::string> labels { "(nothing: it is at the top)" };
                    std::map<std::string, int> seen;
                    for (const kecs::Entity other : doc.Entities()) {       // parents before their children
                        if (below.contains(other)) continue;
                        if (below.contains(world.ParentOf(other))) { below.insert(other); continue; }
                        std::string label = doc.NameOf(other);
                        if (const int count = ++seen[label]; count > 1) label += std::format("  ({})", count);
                        others.push_back(other);
                        labels.push_back(std::move(label));
                    }
                    const kecs::Entity parent = world.ParentOf(entity);
                    const auto at = std::ranges::find(others, parent);
                    kui::Widget under = kui::Dropdown(labels, parent.IsNull() || at == others.end() ? 0 : static_cast<int>(at - others.begin()) + 1, [this, entity, others](const int index) {
                        Doc().SetParent(entity, index <= 0 ? kecs::NullEntity : others[static_cast<std::size_t>(index - 1)]);
                    });
                    under = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("entity").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                        if (const auto* other = dragged.As<kecs::Entity>(); other && *other != entity) _session->MoveEntity(entity, *other, Session::Beside::eUnder);
                    }), std::move(under));
                    rows.push_back(kui::Tooltip("What it is under: it moves, turns and grows with that. Chosen here, or an entity dropped here from the scene tree. Where it is in the world stays as it was.",
                                                kgui::Labeled("Under", std::move(under), LabelWidth)));
                    if (const auto count = world.ChildrenOf(entity).size(); count > 0)
                        rows.push_back(kgui::Labeled("Over", kgui::Muted(std::format("{} {}", count, count == 1 ? "entity" : "entities")), LabelWidth));
                }
                // Which prefab it is an instance of — or part of one — and what of it is the instance's own.
                if (const kecs::Entity root = doc.PrefabRootOf(entity); !root.IsNull()) {
                    const auto overrides = doc.OverridesOf(root);
                    const std::string prefab = std::filesystem::path(doc.PrefabOf(root)).stem().string();
                    std::vector<kui::Widget> line;
                    line.push_back(kui::SizedBox(18.f, 18.f, kui::Icon("widgets", kui::IconStyle::eFilled, kui::Color::Hex(0x4FB3BF))));
                    line.push_back(kui::Expanded(kgui::Line(root == entity ? std::format("An instance of {}", prefab)
                                                                           : std::format("Part of {}, an instance of {}", doc.NameOf(root), prefab))));
                    rows.push_back(kui::Row(std::move(line), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                    rows.push_back(kgui::Muted(overrides.empty() ? std::string("As its prefab is: nothing of it is the instance's own.")
                                                                 : std::format("{} {} its own: changes made to it here, kept when the prefab changes.", overrides.size(),
                                                                               overrides.size() == 1 ? "thing is" : "things are")));
                    const auto act = [this, root](bool (Document::*what)(kecs::Entity, std::string*), const char* done) {
                        return [this, root, what, done] {
                            std::string error;
                            if ((Doc().*what)(root, &error)) _session->Say(done); else _session->Say(error, true);
                        };
                    };
                    std::vector<kui::Widget> buttons;
                    buttons.push_back(kui::Tooltip("Writes the instance as it is now to its prefab: every instance of it follows, here and in every scene as it is opened.",
                                                   Small("Apply to the prefab", act(&Document::ApplyPrefab, "Applied to the prefab: every instance of it follows."))));
                    buttons.push_back(kui::Tooltip("Drops what is the instance's own, and what was put under it: it is as its prefab is.",
                                                   Small("Revert", act(&Document::RevertPrefab, "Reverted: it is as its prefab is."))));
                    buttons.push_back(kui::Tooltip("Makes it entities like any other: no prefab's.", Small("Unlink", [this, root] { Doc().Unlink(root); _session->Say("Unlinked: no prefab's now."); })));
                    if (root != entity) buttons.push_back(Small("Pick the instance", [this, root] { _session->Select(root); }));
                    rows.push_back(kui::Row(std::move(buttons), kui::FlexOptions {}.SetGap(6.f)));
                }
                // What draws it: each pass's node, to go to.
                {
                    const auto passes = _session->RunsFor(entity);
                    if (!passes.empty()) {
                        std::vector<kui::Widget> each;
                        for (const auto& pass : passes) each.push_back(kui::Tooltip("Go to the node, on the graph", Chip(pass, [this, pass] { _session->GoTo(pass); })));
                        rows.push_back(kgui::Labeled("Run by", kui::Wrap(std::move(each), 4.f, 4.f), LabelWidth));
                    } else if (!can) {
                        rows.push_back(kgui::Labeled("Run by", kgui::Muted("nothing: it has no component of a pipeline's"), LabelWidth));
                    }
                }

                // Where it is, relative to what it is under: every entity has that, and it is no component.
                rows.push_back(kui::Separator());
                rows.push_back(kui::Row({ kui::SizedBox(16.f, 16.f, kui::Icon("open_with", kui::IconStyle::eFilled, t.textMuted)), kui::Expanded(Heading("Transform")),
                                          kui::Tooltip("Back to where what it is under is: no move, no turn, its own size", Chip("Reset", [this, entity] {
                                              Doc().SetPosition(entity, kecs::Vec3 { 0.f, 0.f, 0.f });
                                              Doc().SetRotation(entity, kecs::Vec3 { 0.f, 0.f, 0.f });
                                              Doc().SetScale(entity, kecs::Vec3 { 1.f, 1.f, 1.f });
                                          })) }, kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                // Where it is, anywhere; how it is turned, round from -180 to 180 degrees; how big, by so much of its size.
                // Each a LiveValue: followed as it plays, the row alone built again.
                const auto three = [&](const char* label, kecs::Vec3 (kecs::World::*get)(kecs::Entity) const, const kui::DragValueOptions& options, const char* what, auto set) {
                    auto read = [this, entity, get] {
                        const kecs::Vec3 v = (Doc().World().*get)(entity);
                        return Value(Value::List { v.x, v.y, v.z });
                    };
                    auto build = [this, entity, set, options](const Value& value) {
                        static constexpr const char* axes[] { "X", "Y", "Z" };
                        std::array<float, 3> now {};
                        for (std::size_t i = 0; i < 3; ++i) now[i] = NumberOf(value.Items()->at(i));
                        std::vector<kui::Widget> parts;
                        for (std::size_t i = 0; i < 3; ++i)
                            parts.push_back(kui::DragValue(now[i], [this, entity, set, now, i](const double v) {
                                auto changed = now;
                                changed[i] = static_cast<float>(v);
                                (Doc().*set)(entity, kecs::Vec3 { changed[0], changed[1], changed[2] });
                            }, kui::DragValueOptions(options).SetLabel(axes[i]).SetWidth(48.f)));
                        return Fitted(std::move(parts));
                    };
                    rows.push_back(kui::Tooltip(what, kgui::Labeled(label, kui::Make<LiveValue>(_session, std::move(read), std::move(build)), LabelWidth)));
                };
                three("Position", &kecs::World::PositionOf, kui::DragValueOptions {}.SetSpeed(0.01f).SetDecimals(2), "Where it is, relative to what it is under.", &Document::SetPosition);
                three("Rotation", &kecs::World::RotationOf, kui::DragValueOptions {}.SetSpeed(0.5f).SetDecimals(2).SetRange(-180.f, 180.f).SetWrap(true),
                      "How it is turned, in degrees: about X, about Y, about Z. Turned past half a turn, it comes round the other way.", &Document::SetRotation);
                three("Scale", &kecs::World::ScaleOf, Dragging({ -kui::Infinity, kui::Infinity, true }, kui::NumberType::eFloat),
                      "How big it is, each way: dragged by so much of its size. Less than nothing is mirrored.", &Document::SetScale);

                const auto has = doc.ComponentsOf(entity);
                const auto pipelines = _session->PipelineComponents();
                // What kind of thing a component is: said beside its name, and by its picture.
                const auto kindOf = [&](const std::string& component) -> std::tuple<const char*, std::string, kui::Color> {
                    const auto& type = doc.World().Describe(doc.World().Find(component));
                    if (const auto pipeline = pipelines.find(component); pipeline != pipelines.end()) return { "brush", "run by a pipeline", kui::Color::Hex(0xD9703F) };
                    if (component == "Mesh") return { "view_in_ar", "what is drawn", kui::Color::Hex(0x7DBB4B) };
                    if (IsCamera(doc.World(), component)) return { "videocam", "what the scene is seen through", kui::Color::Hex(0x62C3E8) };
                    if (component.ends_with("Light")) return { "lightbulb", "a light", kui::Color::Hex(0xE8C562) };
                    if (type.fields.empty()) return { "label", "a tag", kui::Color::Hex(0x9AA4B0) };
                    return { "data_object", "data", kui::Color::Hex(0x5B9BD5) };
                };
                // A field's control, read from @p component's and set by @p set: of one component, or of each that shares it.
                const auto fieldEditor = [&](const std::string& component, const auto& field, const std::function<void(const Value&)>& set) -> kui::Widget {
                    const std::string fieldName = field.name;
                    const Value value = doc.Field(entity, component, fieldName);
                    // As its script says — its [Range], [Min], [Logarithmic] — and an unsigned one none or more.
                    const kgraph::NumberLimits limits { field.type.starts_with("u") ? std::max(field.min, 0.f) : field.min, field.max, field.logarithmic };
                    kui::Widget editor;
                    if (HoldsResource(field.type)) {
                        editor = HeldField(component, fieldName, field.type, value, set);
                    } else if (field.type == "entity") {
                        editor = EntityPicker(doc.World(), value, set);
                    } else if (!Editable(field.type)) {
                        editor = kgui::Muted(field.type);
                    } else if (value.String()) {
                        const std::string text = *value.String();
                        editor = kui::TextField(kui::TextFieldOptions { .text = text, .onSubmitted = [set](const std::string& typed) { set(Value(typed)); } })
                            .Key(component + "." + fieldName + "=" + text);
                    } else if ((field.type == "vec4" || field.type == "vec3") && NamesAColor(fieldName)) {
                        editor = ColorOf(value, set, field.type == "vec4");
                    } else {
                        // A box to tick, numbers to drag: followed as it plays (LiveValue), the control alone built again.
                        const kui::NumberType number = NumberTypeOfField(field.type);
                        auto build = [set, limits, number, choices = field.choices](const Value& now) -> kui::Widget {
                            if (now.Bool()) return kui::Checkbox(*now.Bool(), [set](const bool on) { set(Value(on)); });
                            if (!choices.empty() && now.Number()) {
                                // One of so many things: by its name. A number that is none of them is shown as it is.
                                std::vector<std::string> names = choices;
                                const auto at = static_cast<long long>(std::llround(*now.Number()));
                                int selected = at >= 0 && at < static_cast<long long>(names.size()) ? static_cast<int>(at) : -1;
                                if (selected < 0) { selected = static_cast<int>(names.size()); names.push_back(std::format("{} (none of these)", at)); }
                                const auto count = choices.size();
                                return kui::Dropdown(std::move(names), selected, [set, count](const int index) {
                                    if (index >= 0 && static_cast<std::size_t>(index) < count) set(Value(index));
                                });
                            }
                            if (const auto* items = now.Items()) {
                                std::vector<float> numbers;
                                for (const auto& item : *items) numbers.push_back(NumberOf(item));
                                return DragNumbers(numbers, [set](const std::vector<float>& changed) { set(Value(Value::List(changed.begin(), changed.end()))); }, limits, number);
                            }
                            return kui::DragValue(NumberOf(now), [set](const double v) { set(Value(v)); }, Dragging(limits, number));
                        };
                        editor = kui::Make<LiveValue>(_session, [this, entity, component, fieldName] { return Doc().Field(entity, component, fieldName); }, std::move(build));
                    }
                    return editor;
                };
                // The pipelines' parameters it has — what its draws take of it that no material does. One that no pass the
                // choices leave running uses (another renderer's) is shut, and what is its own is not set while so.
                std::vector<std::string> materials;
                for (const auto& component : has)
                    if (pipelines.contains(component) && !doc.World().Describe(doc.World().Find(component)).fields.empty()) materials.push_back(component);
                std::map<std::string, bool> inUse;
                for (const auto& material : materials) inUse[material] = _session->MaterialInUse(material);
                for (const auto& component : has) {
                    const bool material = std::ranges::contains(materials, component);
                    rows.push_back(kui::Separator());
                    const auto& type = doc.World().Describe(doc.World().Find(component));
                    auto [componentIcon, what, componentTint] = kindOf(component);
                    // Another renderer's: shut unless opened, and what is its own not set while no pass uses it.
                    const bool unused = material && !inUse[component];
                    const bool shut = unused ? !_unusedOpen.contains(component) : _componentsShut.contains(component);
                    if (material) what = unused ? std::string("pipeline parameters · no pass uses them now") : std::string("pipeline parameters");
                    // A material: of which kind, which asset it follows, and how much of it is the entity's own.
                    const bool kind = doc.Runtime().MaterialKindOf(component) != nullptr;
                    const std::string followed = kind ? doc.MaterialAssetOfEntity(entity, component) : std::string();
                    const std::vector<std::string> ownFields = kind ? doc.MaterialOverridesOf(entity, component) : std::vector<std::string>();
                    if (kind) {
                        const MaterialAsset* asset = followed.empty() ? nullptr : Doc().MaterialAssetOf(followed);
                        what = followed.empty() ? std::string("material · its own")
                             : std::format("material · {}{}", asset ? asset->name : followed,
                                           ownFields.empty() ? std::string() : std::format(" · {} {} its own", ownFields.size(), ownFields.size() == 1 ? "field" : "fields"));
                    }
                    const auto pipeline = pipelines.find(component);
                    std::vector<kui::Widget> head;
                    if (!type.fields.empty())
                        head.push_back(kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(shut ? "chevron_right" : "expand_more", kui::IconStyle::eFilled, t.textMuted)),
                                                   [this, component, shut, unused] {
                                                       SetState([&] {
                                                           std::set<std::string>& folded = unused ? _unusedOpen : _componentsShut;
                                                           if (shut == !unused) folded.erase(component); else folded.insert(component);
                                                       });
                                                   },
                                                   kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain)));
                    head.push_back(kui::Tooltip("Switched off, it has the component still and nothing runs for it",
                                                kui::Checkbox(doc.IsEnabled(entity, component), [this, entity, component](const bool on) { Doc().SetEnabled(entity, component, on); })));
                    head.push_back(kui::SizedBox(16.f, 16.f, kui::Icon(componentIcon, kui::IconStyle::eFilled, componentTint)));
                    head.push_back(Heading(component));
                    head.push_back(kui::Expanded(kgui::Muted(what, 11.f)));
                    if (pipeline != pipelines.end())
                        head.push_back(kui::Tooltip("Go to the pipeline it is of, on the graph", SmallIcon("hub", [this, node = pipeline->second] { _session->GoTo(node); })));
                    head.push_back(kui::Tooltip("Take the component off the entity", SmallIcon("close", [this, entity, component] { Doc().RemoveComponent(entity, component); })));
                    rows.push_back(kui::ContextMenu({
                        { "Take it off the entity", [this, entity, component] { Doc().RemoveComponent(entity, component); } },
                        { doc.IsEnabled(entity, component) ? "Switch it off" : "Switch it on", [this, entity, component, on = doc.IsEnabled(entity, component)] { Doc().SetEnabled(entity, component, !on); } },
                        { "Go to its pipeline", [this, node = pipeline != pipelines.end() ? pipeline->second : std::string()] { _session->GoTo(node); }, pipeline != pipelines.end() },
                    }, kui::Opacity(unused ? 0.55f : 1.f, kui::Row(std::move(head), kui::FlexOptions {}.SetGap(6.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)))));
                    if (shut) continue;
                    // A material that follows an asset: what of it is the entity's own, kept or not.
                    if (!followed.empty()) {
                        const auto act = [this, entity, component](bool (Document::*what)(kecs::Entity, std::string_view), const char* done) {
                            return [this, entity, component, what, done] { if ((Doc().*what)(entity, component)) _session->Say(done); };
                        };
                        std::vector<kui::Widget> buttons;
                        if (!ownFields.empty()) {
                            buttons.push_back(kui::Tooltip("Writes what is its own to the material asset: every entity that follows it has it.",
                                                           Small("Apply to the asset", act(&Document::ApplyMaterial, "Applied to the material: every entity that follows it has it."))));
                            buttons.push_back(kui::Tooltip("Drops what is its own: it is as its material asset is.",
                                                           Small("Revert", act(&Document::RevertMaterial, "Reverted: it is as its material is."))));
                        }
                        buttons.push_back(kui::Tooltip("Makes the material its own: it no longer follows the asset.",
                                                       Small("Unlink", act(&Document::UnlinkMaterial, "Unlinked: its material is its own now."))));
                        rows.push_back(kui::Row(std::move(buttons), kui::FlexOptions {}.SetGap(6.f)));
                    } else if (kind) {
                        rows.push_back(kui::Tooltip("Saves it as a material of the project's, in assets/materials: the entity follows it, and so can others dropped it on.",
                                                    Small("Make an asset of it", [this, entity, component] { _session->MaterialAssetFrom(entity, component); })));
                    }
                    for (const auto& field : type.fields) {
                        const std::string fieldName = field.name;
                        const auto set = [this, entity, component, fieldName](const Value& changed) {
                            std::string error;
                            if (!Doc().SetField(entity, component, fieldName, changed, &error)) _session->Say(error, true);
                        };
                        kui::Widget editor = fieldEditor(component, field, set);
                        if (unused) editor = kui::Disabled(std::move(editor));
                        // The entity's own, not its asset's: marked so.
                        const bool own = std::ranges::contains(ownFields, fieldName);
                        rows.push_back(kui::Tooltip(own ? std::format("{} · the entity's own: its material asset no longer sets it", field.type) : field.type,
                                                    kgui::Labeled(own ? std::format("● {}", fieldName) : fieldName, std::move(editor), LabelWidth)));
                    }
                }

                // What it can be given, each said for what it is: a pipeline's, to be drawn by it; data; a tag.
                rows.push_back(kui::Separator());
                std::vector<std::string> more, moreLabels;
                for (const int order : { 0, 1, 2 })
                    for (const auto& component : doc.ComponentNames()) {
                        if (std::ranges::find(has, component) != has.end()) continue;
                        if (doc.World().Describe(doc.World().Find(component)).abstract) continue;     // a kind: given as one of it
                        const bool pipeline = pipelines.contains(component), tag = doc.World().Describe(doc.World().Find(component)).fields.empty();
                        if ((pipeline ? 0 : tag ? 2 : 1) != order) continue;
                        more.push_back(component);
                        moreLabels.push_back(std::format("{}   ·   {}", component, std::get<1>(kindOf(component))));
                    }
                kui::Widget add = kui::Dropdown(moreLabels, -1, [this, entity, more](const int index) {
                    Doc().AddComponent(entity, more[static_cast<std::size_t>(index)]);
                }, kui::DropdownOptions {}.SetPlaceholder(more.empty() ? "it has every component there is" : "a component, or a tag"));
                // A script dropped here from the Assets panel is the component it makes.
                add = kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* id = dragged.As<std::string>()) _session->GiveAsset(*id, entity);
                }), std::move(add));
                rows.push_back(kui::Tooltip("Gives it a component. A mesh, a texture or a script dropped here from the Assets panel is given to it too.", kgui::Labeled("Add", std::move(add), LabelWidth)));
                rows.push_back(kgui::Labeled("New tag", kui::TextField(kui::TextFieldOptions { .placeholder = "its name, then Enter",
                    .onSubmitted = [this, entity](const std::string& typed) {
                        if (typed.empty()) return;
                        Doc().AddTag(typed);
                        Doc().AddComponent(entity, typed);
                    } }).Key(std::format("tag{}", has.size())), LabelWidth));
                std::vector<kui::Widget> actions { Small("Duplicate", [this, entity] { _session->Duplicate(entity); }) };
                // One of no prefab's is made one: it, and what is under it, in the project's prefabs; it the first instance.
                if (Doc().PrefabRootOf(entity).IsNull())
                    actions.push_back(kui::Tooltip("Saves it, and what is under it, as a prefab in the project's prefabs folder: it is the prefab's first instance, and more are dragged from the Assets panel.",
                                                   Small("Save as prefab", [this, entity] { _session->SaveAsPrefab(entity); })));
                // One whose mesh or picture a generator makes: the generator, as it is set, kept to be given to others.
                if (const auto components = Doc().ComponentsOf(entity); std::ranges::any_of(components, [&](const std::string& name) {
                        return std::ranges::contains(Doc().ScriptsTaking("MeshGenerator"), name) || std::ranges::contains(Doc().ScriptsTaking("ImageGenerator"), name); }))
                    actions.push_back(kui::Tooltip("Saves its generator, with its fields as they are, in the project's assets: dropped on another entity, it makes the same there.",
                                                   Small("Save the generator", [this, entity] { _session->SaveGenerator(entity); })));
                actions.push_back(Small("Destroy the entity", [this, entity] { Doc().DestroyEntity(entity); _session->Select(kecs::NullEntity); }));
                rows.push_back(kui::Row(std::move(actions), kui::FlexOptions {}.SetGap(6.f)));
                // A script dropped anywhere on the entity's inspector is the component it makes; a mesh or a texture is
                // given to it as Add does. (A field that holds one takes what is dropped on it for itself.)
                return kui::DropTarget(kui::DropTargetOptions {}.AcceptsType("asset").OnDrop([this, entity](const kui::DragData& dragged, kor::Vec2) {
                    if (const auto* id = dragged.As<std::string>()) _session->GiveAsset(*id, entity);
                }), Panel(std::move(rows)));
            }

            // ---- what is wrong ----------------------------------------------------------------------------------

            /**
             * The frame as it runs: the graph's passes in the order the frame graph schedules them, each with its level,
             * its queue, a bar as long as its GPU time — coloured by its share of the frame — and what it uses; then what
             * was left out, and why. Read only: a pass pressed is gone to on its page. Followed as it runs.
             */
            /**
             * The pages as a flow, left to right: each a card wired from the pages it uses what they made of, and each
             * Switch between branches a card that says which it takes. Nothing is wired or moved for good here: the
             * flow is the graph's, and only which branch is chosen is set — and a page pressed twice is shown in the
             * Graph panel. A page that did not run in the last frame (a branch not chosen) is dimmed.
             */
            kui::Widget FlowPanel()
            {
                return kui::Make<LiveValue>(_session, [this] {
                    if (!_layout->IsShown("graph") || !_session->ShowsFrame()) return Value();
                    const auto flow = _session->Flow();
                    std::string seen;
                    for (const auto& step : flow.steps) seen += std::format("{}|{}|{}|{:.1f}|{};", step.id, step.ran, step.chosen, step.gpuMs, step.nodes);
                    for (const auto& link : flow.links) seen += std::format("{}>{}#{};", link.from, link.to, link.option);
                    // And built again when a card was dragged, or a page moved to another lane.
                    for (const auto& [id, by] : _flowMoved) seen += std::format("{}@{},{};", id, by.x, by.y);
                    for (const auto& step : flow.steps) seen += step.rate;
                    // And when a link of the order, or a tie, is made or goes.
                    for (const auto& [page, before] : _session->Links()) seen += std::format("{}<{};", page, before);
                    for (const auto& tie : _session->Ties()) seen += std::format("{}~{};", tie.first, tie.second);
                    return Value(seen);
                }, [this](const Value& seen) { return seen.IsNull() ? kui::SizedBox(0.f, 0.f) : FlowGraph(); });
            }

            /** The flow laid out again: every card where the flow puts it, none where it was dragged, and all of it in sight. */
            void ArrangeFlow()
            {
                _flowMoved.clear();
                _flowFramed.clear();
                _session->Say("The flow is laid out again, by what each page uses.");
                SetState();
            }

            /** The lane a step of the flow is in: its page's rate; a Switch, every frame's. */
            static kgraph::PassRate LaneOf(const Session::PassFlow::Step& step)
            {
                return step.rate == "setup" ? kgraph::PassRate::eSetup : step.rate == "fixed" ? kgraph::PassRate::eFixed : kgraph::PassRate::eFrame;
            }
            static const char* LaneName(const kgraph::PassRate rate)
            {
                return rate == kgraph::PassRate::eSetup ? "SETUP  ·  made once" : rate == kgraph::PassRate::eFixed ? "FIXED STEP  ·  while it plays" : "EVERY FRAME";
            }

            /** The colour of the order's ports and wires: quiet, beside what is used. */
            static kui::Color OrderColor() { return kui::Theme::Current().textMuted; }

            kui::Widget FlowGraph()
            {
                using Rate = kgraph::PassRate;
                const auto flow = _session->Flow();
                const kui::Theme& t = kui::Theme::Current();
                const auto ties = _session->Ties();
                // Laid out by how big each card was drawn last.
                std::map<std::string, std::pair<float, float>> sizes;
                for (const auto& [id, size] : _flowView->sizes) sizes[id] = { size.x, size.y };
                // A lane for each rate, in the order they run — setup, the fixed step, every frame — each laid out by itself,
                // left to right after the one before. Every lane is there, empty or not: a page is dragged into it.
                constexpr float LaneGap = 120.f, LanePad = 48.f, EmptyLane = 260.f;
                Session::FlowLayout layout;
                _lanes.clear();
                float left = 0.f, top = 0.f, bottom = 320.f;
                for (const Rate rate : { Rate::eSetup, Rate::eFixed, Rate::eFrame }) {
                    Session::PassFlow lane;
                    for (const auto& step : flow.steps) if (LaneOf(step) == rate) lane.steps.push_back(step);
                    for (const auto& link : flow.links)
                        if (std::ranges::contains(lane.steps, link.from, &Session::PassFlow::Step::id) && std::ranges::contains(lane.steps, link.to, &Session::PassFlow::Step::id))
                            lane.links.push_back(link);
                    const auto laid = Session::LayOut(lane, sizes);
                    float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
                    for (const auto& [id, at] : laid.at) {
                        const auto size = sizes.contains(id) ? sizes.at(id) : std::pair { 240.f, 90.f };
                        low = std::min(low, at.first);
                        high = std::max(high, at.first + size.first);
                        top = std::min(top, at.second);
                        bottom = std::max(bottom, at.second + size.second);
                    }
                    const float shift = laid.at.empty() ? 0.f : left + LanePad - low;
                    for (const auto& [id, at] : laid.at) layout.at[id] = { at.first + shift, at.second };
                    if (laid.at.empty()) layout.at[std::format("@lane {}", static_cast<int>(rate))] = { left + LanePad, 0.f };
                    for (auto branch : laid.branches) layout.branches.push_back(std::move(branch));
                    const float width = laid.at.empty() ? EmptyLane : high - low + 2.f * LanePad;
                    _lanes.push_back({ rate, left, left + width });
                    left += width + LaneGap;
                }
                // Framed whole again when the pages are others, or their cards were first measured: laid out anew then.
                std::string pages = sizes.empty() ? std::string("?") : std::string();
                for (const auto& step : flow.steps) pages += step.id + step.rate + "|";
                if (pages != _flowFramed) { _flowFramed = pages; _flowView->Frame(); }
                kui::NodeGraph graph;
                // An empty lane holds a card that says what it is for: pressed twice, a page of it is made.
                for (const auto& lane : _lanes) {
                    const std::string id = std::format("@lane {}", static_cast<int>(lane.rate));
                    if (!layout.at.contains(id)) continue;
                    kui::GraphNode empty;
                    empty.id = id;
                    empty.title = lane.rate == Rate::eSetup ? "No setup page" : lane.rate == Rate::eFixed ? "No fixed-step page" : "No page";
                    empty.icon = "add";
                    empty.accent = t.textMuted;
                    empty.width = EmptyLane - 2.f * LanePad;
                    empty.position = { layout.at.at(id).first, layout.at.at(id).second };
                    empty.body = kui::Padding(kui::EdgeInsets::Symmetric(8.f, 6.f), kgui::Muted("Drag a page here, or press twice to make one.", 11.f));
                    graph.nodes.push_back(std::move(empty));
                }
                // The lanes, behind everything: as tall as the tallest, each named for when its pages run.
                for (const auto& lane : _lanes)
                    graph.comments.push_back({ .id = std::format("lane {}", static_cast<int>(lane.rate)), .text = LaneName(lane.rate),
                                               .rect = kui::Rect::LTRB(lane.left, top - 2.f * LanePad, lane.right, bottom + LanePad),
                                               .color = lane.rate == Rate::eSetup ? kui::Color::Hex(0xB0892F) : lane.rate == Rate::eFixed ? kui::Color::Hex(0x3FA37C) : t.primary });
                // Each branch a band of its own, framed and named: a colour each, the same one it is in every time.
                static const std::uint32_t hues[] { 0x4C8DD6, 0xC58A2E, 0x9B6ADA, 0x3FA37C, 0xCF5C7A, 0x6C8A9E };
                for (std::size_t b = 0; b < layout.branches.size(); ++b) {
                    const auto& branch = layout.branches[b];
                    graph.comments.push_back({ .id = std::format("branch {}", b), .text = branch.title,
                                               .color = kui::Color::Hex(hues[static_cast<std::size_t>(branch.option) % std::size(hues)]), .around = branch.steps });
                }
                for (const auto& step : flow.steps) {
                    kui::GraphNode node;
                    node.id = step.id;
                    node.title = step.id;
                    const auto at = layout.at.at(step.id);
                    node.position = { at.first + _flowMoved[step.id].x, at.second + _flowMoved[step.id].y };
                    node.outputs.push_back({ .id = "out", .label = "made", .type = "flow" });
                    if (step.isSwitch) {
                        node.icon = "call_split";
                        node.accent = t.primary;
                        node.width = 220.f;
                        for (std::size_t i = 0; i < step.options.size(); ++i)
                            node.inputs.push_back({ .id = std::format("option[{}]", i), .label = std::format("{}  {}", i, step.options[i]), .type = "flow" });
                        node.body = kui::Padding(kui::EdgeInsets::All(6.f), kui::Dropdown(step.options, std::clamp(step.chosen, 0, std::max(0, static_cast<int>(step.options.size()) - 1)),
                            [this, driver = step.driver](const int option) { _session->Choose(driver, option); }, kui::DropdownOptions {}.SetWidth(200.f)));
                    } else {
                        node.icon = step.rate == "setup" ? "build" : step.rate == "fixed" ? "timer" : "layers";
                        // A page is made of nodes: as abstract as the most abstract of them.
                        node.composite = true;
                        for (const auto& each : Doc().Graph().nodes)
                            if (each.pass == step.id)
                                if (const kgraph::NodeType* type = kgraph::FindType(each.type); type && each.type != "Reference")
                                    node.level = std::max(node.level, static_cast<int>(type->level));
                        node.inputs.push_back({ .id = "in", .label = "uses", .type = "flow", .multiple = true });
                        // Where what it uses does not say: what it runs after, wired from that one's `then`.
                        node.inputs.push_back({ .id = "after", .label = "after", .type = "order", .color = OrderColor(), .shape = kui::PortShape::eDiamond, .multiple = true });
                        node.outputs.push_back({ .id = "then", .label = "then", .type = "order", .color = OrderColor(), .shape = kui::PortShape::eDiamond });
                        if (!step.ran) node.accent = t.textMuted;
                        std::string said = !step.ran ? std::format("{} nodes  ·  not run", step.nodes)
                                                     : std::format("{} nodes  ·  {:.2f} ms", step.nodes, step.gpuMs);
                        // Tied with another: both write something, and nothing says which runs first.
                        std::vector<std::string> tied;
                        for (const auto& tie : ties)
                            if (tie.first == step.id || tie.second == step.id) tied.push_back(tie.first == step.id ? tie.second : tie.first);
                        std::vector<kui::Widget> lines { kgui::Muted(said, 11.f) };
                        if (!tied.empty()) {
                            std::string with;
                            for (const auto& other : tied) with += (with.empty() ? "" : ", ") + other;
                            lines.push_back(kui::Tooltip("Both write something, and nothing says which runs first: wire one's then to the other's after, or right-click: Run after.",
                                kui::Text(std::format("Tied with {}", with), kui::TextStyle {}.SetSize(11.f).SetColor(kui::Color::Hex(0xE0A040)), kui::TextAlign::eStart, false, 2, true)));
                        }
                        node.body = kui::Padding(kui::EdgeInsets::Symmetric(8.f, 4.f), Rows(std::move(lines), 2.f));
                    }
                    graph.nodes.push_back(std::move(node));
                }
                for (const auto& link : flow.links) {
                    const auto to = std::ranges::find(flow.steps, link.to, &Session::PassFlow::Step::id);
                    graph.wires.push_back({ { link.from, "out" }, { link.to, to != flow.steps.end() && to->isSwitch ? std::format("option[{}]", link.option) : std::string("in") } });
                }
                // The links of the order: each from the page it runs after.
                for (const auto& [page, before] : _session->Links()) {
                    const auto isPage = [&](const std::string& id) { return std::ranges::any_of(flow.steps, [&](const auto& step) { return step.id == id && !step.isSwitch; }); };
                    if (isPage(page) && isPage(before)) graph.wires.push_back({ { before, "then" }, { page, "after" } });
                }
                // Where each card is, as laid out: what a card let go in another lane is moved by.
                auto places = std::make_shared<std::map<std::string, std::pair<kor::Vec2, Rate>>>();
                for (const auto& card : graph.nodes)
                    if (const auto step = std::ranges::find(flow.steps, card.id, &Session::PassFlow::Step::id); step != flow.steps.end() && !step->isSwitch)
                        (*places)[card.id] = { card.position + (_flowView->sizes.contains(card.id) ? _flowView->sizes.at(card.id) * 0.5f : kor::Vec2 { 120.f, 45.f }), LaneOf(*step) };
                // A function dropped on the frame is a page of its own, in the lane it is dropped in, with a call of it.
                return kui::DropTarget(kui::DropTargetOptions {}
                    .Accepts([](const kui::DragData& dragged) { const auto* id = dragged.As<std::string>(); return dragged.type == "asset" && id && id->starts_with("function:"); })
                    .OnDrop([this](const kui::DragData& dragged, const kor::Vec2 local) {
                        const auto* id = dragged.As<std::string>();
                        if (!id) return;
                        const kor::Vec2 at = _flowView->centre + (local - _flowView->size * 0.5f) / std::max(_flowView->zoom, 0.01f);
                        const auto lane = std::ranges::find_if(_lanes, [&](const Lane& each) { return at.x >= each.left && at.x < each.right; });
                        const std::string name = id->substr(9);
                        std::string page = name;
                        for (int n = 2; std::ranges::contains(Doc().Passes(), page); ++n) page = std::format("{} {}", name, n);
                        if (!Doc().AddPass(page, -1, lane != _lanes.end() ? lane->rate : kgraph::PassRate::eFrame)) return;
                        const std::string call = Doc().AddNode("Function", 0.0, 0.0, name, page);
                        if (!call.empty()) Doc().SetInput(call, "function", kgraph::Value(name));
                        _session->Say(std::format("A page of its own, {}, with a call of {}: pressed twice, it is gone into, and the call wired there.", page, name));
                    }), kui::NodeEditor(std::move(graph), kui::NodeEditorOptions {}
                    // Only the order is wired here: a page's then to another's after, that one running after it.
                    .CanConnect([](const kui::NodePort& from, const kui::NodePort& to) { return from.type == "order" && to.type == "order"; })
                    .OnConnect([this](const kui::GraphWire& wire) {
                        if (!Doc().RunAfter(wire.to.node, wire.from.node))
                            _session->Say(std::format("{} cannot run after {}: another lane's, or it runs before it already.", wire.to.node, wire.from.node), true);
                        else _session->Say(std::format("{} runs after {} now.", wire.to.node, wire.from.node));
                    })
                    .OnDisconnect([this](const kui::GraphWire& wire) { if (wire.to.port == "after") Doc().StopRunningAfter(wire.to.node, wire.from.node); })
                    // A page let go in another lane runs at that lane's rate; let go in its own, it stays where it was put.
                    .OnMove([this, places](const std::vector<std::string>& nodes, const kor::Vec2 by) {
                        for (const auto& id : nodes) {
                            const auto place = places->find(id);
                            if (place != places->end()) {
                                const float x = place->second.first.x + by.x;
                                const auto lane = std::ranges::find_if(_lanes, [&](const Lane& each) { return x >= each.left && x < each.right; });
                                if (lane != _lanes.end() && lane->rate != place->second.second) {
                                    if (Doc().SetPassRate(id, lane->rate)) {
                                        _flowMoved.erase(id);
                                        _session->Say(std::format("{} runs {} now.", id, lane->rate == Rate::eSetup ? "once, as setup" : lane->rate == Rate::eFixed ? "on the fixed step" : "every frame"));
                                    }
                                    continue;
                                }
                            }
                            _flowMoved[id] = _flowMoved[id] + by;
                        }
                        SetState();
                    })
                    .Menu([this](const std::vector<std::string>& picked, const kor::Vec2 at) {
                        std::vector<kui::MenuItem> items;
                        const auto& passes = Doc().Passes();
                        if (!picked.empty() && std::ranges::contains(passes, picked.front())) {
                            const std::string page = picked.front();
                            const Rate rate = Doc().RateOf(page);
                            std::size_t nodes = 0;
                            for (const auto& node : Doc().Graph().nodes) nodes += node.pass == page;
                            items.push_back({ "Open the page", [this, page] { _session->ShowPass(page); } });
                            for (auto& item : OrderItems(page)) items.push_back(std::move(item));
                            items.push_back(MenuLine());
                            items.push_back({ "Run it every frame", [this, page] { Doc().SetPassRate(page, Rate::eFrame); }, rate != Rate::eFrame });
                            items.push_back({ "Run it on the fixed step", [this, page] { Doc().SetPassRate(page, Rate::eFixed); }, rate != Rate::eFixed });
                            items.push_back({ "Make it once, as setup", [this, page] { Doc().SetPassRate(page, Rate::eSetup); }, rate != Rate::eSetup });
                            items.push_back(MenuLine());
                            items.push_back({ "Put back where the flow has it", [this, picked] { SetState([&] { for (const auto& id : picked) _flowMoved.erase(id); }); } });
                            items.push_back({ nodes ? std::format("Delete, with its {} nodes", nodes) : std::string("Delete"), [this, page] { Doc().RemovePass(page); } });
                            items.push_back({ "Delete the page, keep its nodes", [this, page] { Doc().RemovePass(page, true); }, nodes > 0 });
                            return items;
                        }
                        if (!picked.empty()) {
                            items.push_back({ "Put back", [this, picked] { SetState([&] { for (const auto& id : picked) _flowMoved.erase(id); }); } });
                            items.push_back(MenuLine());
                        }
                        // A new page, of the lane it is asked for in.
                        const auto lane = std::ranges::find_if(_lanes, [&](const Lane& each) { return at.x >= each.left && at.x < each.right; });
                        const Rate rate = lane != _lanes.end() ? lane->rate : Rate::eFrame;
                        items.push_back({ rate == Rate::eSetup ? "New setup page" : rate == Rate::eFixed ? "New page of the fixed step" : "New page", [this, rate] { NewPage(rate); } });
                        items.push_back({ "Every node at once", [this] { _session->ShowEveryPass(); } });
                        items.push_back(MenuLine());
                        items.push_back({ "Arrange the flow", [this] { ArrangeFlow(); } });
                        items.push_back({ "Frame everything", [this] { _flowView->Frame(); SetState(); } });
                        // Every Reference wired through: a page shows what it uses of another beside what uses it.
                        if (std::ranges::any_of(Doc().Graph().nodes, [](const kgraph::Node& node) { return node.type == "Reference"; })) {
                            items.push_back(MenuLine());
                            items.push_back({ "Wire straight from other pages", [this] {
                                const std::size_t gone = Doc().WireStraight();
                                kor::log::Info("{} Reference{} wired through: what used each is wired from what it stood for", gone, gone == 1 ? "" : "s");
                                SetState();
                            } });
                        }
                        return items;
                    })
                    // Pressed twice, a page is gone into; an empty lane's card makes the lane's first.
                    .OnActivate([this](const std::string& id) {
                        if (std::ranges::contains(Doc().Passes(), id)) _session->ShowPass(id);
                        else if (id.starts_with("@lane ")) NewPage(static_cast<Rate>(std::stoi(id.substr(6))));
                    })
                    .SetView(_flowView)));
            }
            /** A lane of the frame, as last laid out: the rate of its pages, and where it starts and ends across. */
            struct Lane { kgraph::PassRate rate; float left, right; };
            std::vector<Lane> _lanes;
            int _arrangeWait = 0;                                           // frames the page to be put in order has been drawn
            std::map<std::string, kor::Vec2> _flowMoved;                    // where the flow's cards were dragged to, from where they are laid out
            std::string _flowFramed;                                        // the pages the flow was last framed with
            // Where a node of another page, shown on a page it is wired to straight, was dragged to there: by page, by node.
            std::map<std::string, std::map<std::string, kor::Vec2>> _outsideMoved;
            // And where it was put when the page was last put in order, by page: its place there, from then on. Which of
            // the nodes laid out were such, and of which page.
            std::map<std::string, std::map<std::string, kor::Vec2>> _outsideArranged;
            std::set<std::string> _placedOutside;
            std::string _placedPage;
            std::shared_ptr<kui::NodeEditorView> _flowView = std::make_shared<kui::NodeEditorView>();

            kui::Widget FramePanel()
            {
                // Read four times a second, and built again only when what it shows has changed: numbers steady enough to read.
                return kui::Make<LiveValue>(_session, [this] {
                    // Nothing is read, or shown, while it is behind another tab.
                    if (!_layout->IsShown("frame")) return Value();
                    const auto now = std::chrono::steady_clock::now();
                    // Paused, it is the frame it was paused on.
                    if (!_frameSeen.IsNull() && (_framePaused || now - _frameReadAt < std::chrono::milliseconds(250))) return _frameSeen;
                    _frameReadAt = now;
                    const kgraph::Runtime& runtime = Doc().Runtime();
                    _frameShot = { runtime.Frame(), runtime.Times(), _session->FrameMs(), 0.0, _session->Fps() };
                    for (const auto& timing : runtime.Timings() | std::views::values) _frameShot.prepareMs += timing.prepareMs;
                    const auto& frame = _frameShot.frame;
                    std::string seen = std::format("{:.1f}|{}|", _frameShot.frameMs, _frameShot.fps);
                    for (const auto& pass : frame.passes) seen += std::format("{}|{}|{:.2f}|{:.2f}|{:.2f};", pass.name, pass.async, pass.gpuMs, pass.recordMs, pass.gpuStartMs);
                    for (const auto& [node, why] : frame.leftOut) seen += node + why;
                    _frameSeen = Value(seen);
                    return _frameSeen;
                }, [this](const Value& seen) { return seen.IsNull() ? kui::SizedBox(0.f, 0.f) : FrameRows(); });
            }

            /**
             * The frame: how long it takes against the time a frame has, then a line for each pass node — a fixed page's
             * steps one line, opened to see each — with its GPU time, its CPU's and its share of the GPU's, in the order
             * they run or the costliest first. What did not run is shut at the foot, by why. A line pressed goes to its node.
             */
            kui::Widget FrameRows()
            {
                using Pass = kgraph::Runtime::FrameReport::Pass;
                const kui::Theme& t = kui::Theme::Current();
                const auto& frame = _frameShot.frame;
                // Where a pass of the frame is on the graph: a call's node is gone to by its call, a loop's turn by its node.
                const auto onGraph = [](std::string id) {
                    if (const auto slash = id.find('/'); slash != std::string::npos) id.resize(slash);
                    if (const auto turn = id.find(" ["); turn != std::string::npos) id.resize(turn);
                    return id;
                };
                // A call's node is said as its call's, then its own.
                const auto said = [](std::string id) {
                    for (std::size_t slash; (slash = id.find('/')) != std::string::npos;) id.replace(slash, 1, " › ");
                    return id;
                };
                const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                const std::string wanted = lower(_frameFilter);

                // A node's passes as one line: each fixed step a frame runs is a pass of the frame graph, and all of them the node's.
                struct Line { std::string node; std::vector<const Pass*> steps; double gpuMs = 0.0, recordMs = 0.0; bool measured = true, async = false; };
                std::vector<Line> lines;
                std::map<std::string, std::size_t> lineOf;
                double gpu = 0.0, record = 0.0;
                for (const auto& pass : frame.passes) {
                    // The editor's own passes — the pictures on the graph's cards — are one line: they cost the frame too.
                    const std::string node = pass.node.starts_with('$') ? std::string("$") : pass.node;
                    const auto [at, made] = lineOf.try_emplace(node, lines.size());
                    if (made) lines.push_back({ node });
                    Line& line = lines[at->second];
                    line.steps.push_back(&pass);
                    line.gpuMs += pass.gpuMs;
                    line.recordMs += pass.recordMs;
                    line.measured = line.measured && pass.gpuMeasured;
                    line.async = line.async || pass.async;
                    gpu += pass.gpuMs;
                    record += pass.recordMs;
                }
                const double prepare = _frameShot.prepareMs;
                if (_frameView == 1) std::ranges::stable_sort(lines, std::greater {}, [](const Line& line) { return std::max(line.gpuMs, line.recordMs); });

                std::vector<kui::Widget> rows;
                // How long a frame takes, against the time one has at the rate picked: over it, the bar says so.
                const double frameMs = _frameShot.frameMs, budget = 1000.0 / _frameHz;
                const auto fact = [&](std::string text, std::string tip) { return kui::Tooltip(std::move(tip), kgui::Muted(std::move(text), 12.f)); };
                const int rates[] { 30, 60, 120, 144 };
                std::vector<kui::Widget> head {
                    Heading(std::format("{:.1f} ms", frameMs)),
                    kgui::Muted(std::format("{} fps", _frameShot.fps), 12.f),
                    fact(std::format("GPU {:.2f} ms", gpu), "What the GPU spends on the graph's passes, added together"),
                    fact(std::format("CPU {:.2f} ms", prepare + record), std::format("Preparing the passes ({:.2f} ms) and recording them ({:.2f} ms)", prepare, record)),
                };
                if (frame.bytes > 0)
                    head.push_back(fact(std::format("{:.0f} MiB", frame.bytes / 1048576.0), frame.unsharedBytes > frame.bytes
                        ? std::format("Made by the frame graph; {:.1f} MiB less than without sharing memory", (frame.unsharedBytes - frame.bytes) / 1048576.0)
                        : std::string("Made by the frame graph")));
                head.push_back(kui::Expanded(kui::SizedBox(0.f, 0.f)));
                head.push_back(kui::Tooltip(_framePaused ? "Paused on this frame: pressed, it follows the frames again" : "Pauses on this frame, to look at it",
                    SmallIcon(_framePaused ? "play_arrow" : "pause", [this] { SetState([&] { _framePaused = !_framePaused; }); })));
                head.push_back(kui::Tooltip("The rate a frame is measured against", kui::SizedBox(110.f, -1.f,
                    kui::Dropdown({ "30 Hz", "60 Hz", "120 Hz", "144 Hz" }, static_cast<int>(std::ranges::find(rates, _frameHz) - std::begin(rates)),
                                  [this, rates](const int index) { SetState([&] { _frameHz = rates[std::clamp(index, 0, 3)]; }); }))));
                rows.push_back(kui::Row(std::move(head), kui::FlexOptions {}.SetGap(12.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                rows.push_back(kui::Tooltip(std::format("The last frames, each a bar as high as it took; the line across is the {:.1f} ms a frame has at {} Hz, "
                                                        "and the frames over it are red. The GPU's time for the passes is the line along them.", budget, _frameHz),
                                            FrameHistory(_frameShot.times, budget)));
                rows.push_back(kui::Row({
                    kui::SizedBox(290.f, -1.f, kui::TabBar({ "Order run", "Cost", "Timeline" }, _frameView, [this](const int tab) { SetState([&] { _frameView = tab; }); })),
                    kui::Expanded(kui::TextField(kui::TextFieldOptions { .text = _frameFilter, .placeholder = "Find a pass",
                        .onChanged = [this](const std::string& typed) { SetState([&] { _frameFilter = typed; }); } }).Key("frame filter")),
                }, kui::FlexOptions {}.SetGap(12.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                if (frame.passes.empty()) { rows.push_back(kgui::Muted("Nothing has run yet.")); return Panel(std::move(rows)); }
                if (_frameView == 2) {
                    rows.push_back(Timeline(frame, wanted, onGraph, said));
                    return Panel(std::move(rows));
                }

                // The columns: what each number is.
                const auto number = [](std::string text, const float width, const kui::Color& ink) {
                    return kui::SizedBox(width, -1.f, kui::Text(std::move(text), kui::TextStyle {}.SetSize(11.5f).SetColor(ink), kui::TextAlign::eEnd, false));
                };
                const auto heading = [&](std::string text, const float width) { return number(std::move(text), width, t.textMuted); };
                rows.push_back(kui::Padding(kui::EdgeInsets::Only(10.f, 0.f, 6.f, 0.f), kui::Row({
                    kui::SizedBox(16.f, 0.f), kui::Expanded(kgui::Muted("PASS", 10.5f)), kui::SizedBox(72.f, 0.f),
                    heading("GPU", 64.f), heading("CPU", 64.f), heading("SHARE", 44.f),
                }, kui::FlexOptions {}.SetGap(8.f))));
                // A pass's numbers: its GPU time ("—" where it cannot be timed), its recording's, and its share of the GPU's as a bar and a figure.
                const auto numbers = [&](std::vector<kui::Widget>& line, const double gpuMs, const double recordMs, const bool measured) {
                    const double share = gpu > 0.0 ? gpuMs / gpu : 0.0;
                    const kui::Color heat = HeatColor(share);
                    const float bar = static_cast<float>(std::clamp(share, 0.0, 1.0));
                    line.push_back(kui::SizedBox(72.f, 6.f, kui::Row({
                        kui::Expanded(kui::Container(kui::ContainerOptions {}.SetHeight(6.f).SetDecoration(kui::Decoration {}.SetColor(t.primary.WithAlpha(0.35f + 0.65f * bar)).SetRadius(3.f))), std::max(bar, 0.01f)),
                        kui::Expanded(kui::Container(kui::ContainerOptions {}.SetHeight(6.f).SetDecoration(kui::Decoration {}.SetColor(t.text.WithAlpha(0.06f)).SetRadius(3.f))), std::max(1.f - bar, 0.f) + 0.0001f) })));
                    line.push_back(number(measured ? std::format("{:.2f} ms", gpuMs) : std::string("—"), 64.f, measured ? heat : t.textMuted));
                    line.push_back(number(std::format("{:.2f} ms", recordMs), 64.f, t.textMuted));
                    line.push_back(number(measured ? std::format("{:.0f}%", 100.0 * share) : std::string(), 44.f, heat));
                };
                std::size_t listed = 0;
                for (const Line& line : lines) {
                    const bool editors = line.node == "$";
                    const std::string shown = editors ? std::string("The editor's previews") : said(line.node);
                    if (!wanted.empty() && lower(shown).find(wanted) == std::string::npos) continue;
                    ++listed;
                    const std::string id = editors ? std::string() : onGraph(line.node);
                    const bool stepped = line.steps.size() > 1, open = _frameOpen.contains(line.node);
                    std::vector<kui::Widget> cells;
                    cells.push_back(stepped
                        ? kui::Button(kui::SizedBox(16.f, 16.f, kui::Icon(open ? "expand_more" : "chevron_right", kui::IconStyle::eFilled, t.textMuted)),
                                      [this, node = line.node] { SetState([&] { if (!_frameOpen.erase(node)) _frameOpen.insert(node); }); },
                                      kui::ButtonOptions {}.SetStyle(kui::ButtonStyle::ePlain))
                        : kui::SizedBox(16.f, 16.f));
                    cells.push_back(kui::Expanded(kui::Text(stepped ? std::format("{}  ×{}", shown, line.steps.size()) : shown,
                                                            kui::TextStyle {}.SetSize(12.5f).SetColor(editors ? t.textMuted : t.text), kui::TextAlign::eStart, false, 1, true)));
                    if (line.async) cells.push_back(ListPill("async", t.textMuted));
                    numbers(cells, line.gpuMs, line.recordMs, line.measured);
                    // Said when the pointer rests on it: where it runs among the rest, and what it reads and writes.
                    const Pass& first = *line.steps.front();
                    std::string reads, writes;
                    for (const auto& [resource, how] : first.uses) {
                        if (how != 'w') reads += std::format("{}{}", reads.empty() ? "" : ", ", resource);
                        if (how != 'r') writes += std::format("{}{}", writes.empty() ? "" : ", ", resource);
                    }
                    const bool fixed = std::ranges::any_of(line.steps, [](const Pass* pass) { return pass->name.contains("\nstep"); });
                    std::string tip = std::format("{}{}{}", line.async ? "On the async compute queue. " : "",
                                                  first.level == 0 ? std::string("Level 0: it waits for no other pass.") : std::format("Level {}: what it waits for runs at lower levels.", first.level),
                                                  !stepped ? std::string() : fixed ? std::format(" Run once for each of the {} fixed steps this frame.", line.steps.size())
                                                                                   : std::format(" It is {} passes of the frame graph.", line.steps.size()));
                    if (!reads.empty()) tip += "\nReads " + reads;
                    if (!writes.empty()) tip += "\nWrites " + writes;
                    if (first.uses.empty()) tip += "\nUses nothing it declares";
                    tip += "\nPressed, it is gone to in the graph.";
                    if (editors) tip = "What the editor draws for itself: the pictures on the graph's cards, and of what is looked at. Not the scene's: a player does without them.";
                    const auto go = [this, id] { if (!id.empty()) _session->GoTo(id); };
                    rows.push_back(kui::Tooltip(std::move(tip), ListLine("frame:" + line.node, !id.empty() && _session->selectedNode == id, std::move(cells), go)));
                    if (stepped && open)
                        for (std::size_t step = 0; step < line.steps.size(); ++step) {
                            const Pass& pass = *line.steps[step];
                            // A fixed step by its number; another pass of the node's by its own name, past the node's.
                            std::string name = pass.name.contains('\n') ? pass.name.substr(pass.name.find('\n') + 1) : fixed ? std::string("step 1")
                                             : editors ? pass.name.substr(pass.name.find(' ') + 1) : pass.name;
                            std::vector<kui::Widget> sub { kui::SizedBox(16.f, 16.f), kui::Expanded(kgui::Muted(std::move(name), 12.f)) };
                            numbers(sub, pass.gpuMs, pass.recordMs, pass.gpuMeasured);
                            rows.push_back(kui::Padding(kui::EdgeInsets::Only(18.f, 0.f, 0.f, 0.f), ListLine(std::format("frame:{}#{}", line.node, step), false, std::move(sub), go)));
                        }
                }
                if (listed == 0) rows.push_back(kgui::Muted("No pass has what is typed."));

                // What did not run, shut at the foot: by why, each why said once.
                struct Why { const char* kind; const char* means; std::vector<std::pair<std::string, std::string>> nodes; };
                Why whys[] {
                    { "skipped", "what they need is made by a pass that is left out", {} },
                    { "culled", "nothing needs what they make", {} },
                    { "resting", "nothing that runs this frame reads what they make: they stay in the frame, and record nothing", {} },
                };
                std::size_t notRun = 0;
                for (const auto& [node, why] : frame.leftOut) {
                    if (!wanted.empty() && lower(node).find(wanted) == std::string::npos) continue;
                    auto* kind = std::ranges::find_if(whys, [&](const Why& w) { return why.starts_with(w.kind); });
                    if (kind == std::end(whys)) kind = &whys[0];
                    const auto colon = why.find(": ");
                    kind->nodes.emplace_back(node, kind == &whys[0] && colon != std::string::npos ? why.substr(colon + 2) : std::string());
                    ++notRun;
                }
                if (notRun > 0) {
                    std::string counts;
                    for (const Why& why : whys)
                        if (!why.nodes.empty()) counts += std::format("{}{} {}", counts.empty() ? "" : " · ", why.nodes.size(), why.kind);
                    rows.push_back(ListLine("frame:not run", false, {
                        kui::SizedBox(16.f, 16.f, kui::Icon(_frameNotRunOpen ? "expand_more" : "chevron_right", kui::IconStyle::eFilled, t.textMuted)),
                        kui::Expanded(kgui::Muted(std::format("Not run ({}): {}", notRun, counts), 12.f)),
                    }, [this] { SetState([&] { _frameNotRunOpen = !_frameNotRunOpen; }); }));
                    if (_frameNotRunOpen)
                        for (const Why& why : whys) {
                            if (why.nodes.empty()) continue;
                            rows.push_back(kui::Padding(kui::EdgeInsets::Only(34.f, 4.f, 6.f, 0.f), kgui::Muted(std::format("{} — {}", Upper(why.kind), why.means), 10.5f)));
                            for (const auto& [node, detail] : why.nodes) {
                                const std::string id = onGraph(node);
                                std::vector<kui::Widget> cells { kui::SizedBox(16.f, 16.f), kui::Text(said(node), kui::TextStyle {}.SetSize(12.5f), kui::TextAlign::eStart, false, 1, true) };
                                cells.push_back(kui::Expanded(detail.empty() ? kui::SizedBox(0.f, 0.f) : kgui::Muted(detail, 11.f)));
                                rows.push_back(kui::Padding(kui::EdgeInsets::Only(18.f, 0.f, 0.f, 0.f),
                                    ListLine("frame:not run:" + node, _session->selectedNode == id, std::move(cells), [this, id] { _session->GoTo(id); })));
                            }
                        }
                }
                return Panel(std::move(rows));
            }
            /** The last frames' times as bars, with the time a frame has across them and the GPU's along them. */
            static kui::Widget FrameHistory(const kgraph::Runtime::FrameTimes& times, const double budget)
            {
                return kui::CustomPaint([times, budget](kui::Canvas& c, const kor::Vec2 size) {
                    const kui::Theme& t = kui::Theme::Current();
                    c.DrawRRect({ kui::Rect::FromSize(size), kui::Radii(4.f) }, kui::Paint::Fill(t.text.WithAlpha(0.04f)));
                    // The frames there have been, of the last 120: before the graph ran, there are none.
                    std::size_t first = times.frameMs.size() > 120 ? times.frameMs.size() - 120 : 0;
                    while (first < times.frameMs.size() && times.frameMs[first] <= 0.f) ++first;
                    const std::size_t count = times.frameMs.size() - first;
                    if (count == 0) return;
                    float most = static_cast<float>(budget) * 1.25f;
                    for (std::size_t i = first; i < times.frameMs.size(); ++i) most = std::max(most, times.frameMs[i]);
                    const float step = size.x / 120.f, height = size.y - 2.f;
                    const auto y = [&](const float ms) { return size.y - 1.f - height * std::min(ms / most, 1.f); };
                    std::vector<kor::Vec2> gpu;
                    for (std::size_t i = 0; i < count; ++i) {
                        const float ms = times.frameMs[first + i], x = size.x - static_cast<float>(count - i) * step;
                        c.DrawRect(kui::Rect::LTRB(x + 0.5f, y(ms), x + std::max(step - 1.f, 1.f), size.y - 1.f),
                                   kui::Paint::Fill(ms > budget ? kui::Color::Hex(0xFF6B6B) : t.primary.WithAlpha(0.55f)));
                        if (first + i < times.gpuMs.size()) gpu.push_back({ x + step * 0.5f, y(times.gpuMs[first + i]) });
                    }
                    if (gpu.size() > 1) c.DrawPolyline(gpu, kui::Paint::Stroked(t.text.WithAlpha(0.6f), 1.f));
                    const float line = y(static_cast<float>(budget));
                    c.DrawLine({ 0.f, line }, { size.x, line }, kui::Paint::Stroked(t.text.WithAlpha(0.35f), 1.f));
                }, { -1.f, 40.f });
            }

            /**
             * One frame as a profiler draws it: the CPU's threads above — the main thread preparing each pass, the threads
             * that record them — and the GPU's queues below, on one time from when the frame started on the CPU (where the
             * device can line its clock up with the CPU's), each piece of work coloured by what it is: drawing, compute,
             * rays, copies. The newest frame the GPU has measured, not an average; paused, the one paused on. The wheel
             * zooms about the pointer and a drag moves along; under the pointer a block is said, and pressed, its node is
             * gone to.
             */
            kui::Widget Timeline(const kgraph::Runtime::FrameReport& frame, const std::string& wanted,
                                 const std::function<std::string(std::string)>& onGraph, const std::function<std::string(std::string)>& said)
            {
                const kui::Theme& t = kui::Theme::Current();
                // What a block is, and its colour: the GPU's by what its commands did, the CPU's by what it was doing.
                enum Kind { eDraw, eCompute, eRays, eTransfer, eNothing, ePrepare, eRecord, eEditor, Kinds };
                static const kui::Color colours[Kinds] { kui::Color::Hex(0x4C8DF6), kui::Color::Hex(0x3FB27F), kui::Color::Hex(0xA970FF), kui::Color::Hex(0xE0A33A),
                                                         kui::Color::Hex(0x5A5A5A), kui::Color::Hex(0x8A94A6), kui::Color::Hex(0x2BB3C0), kui::Color::Hex(0x6E6E6E) };
                static const char* kinds[Kinds] { "draws", "compute", "rays", "copies and clears", "records nothing", "prepare", "record", "the editor's" };
                const auto kindOf = [](const std::uint8_t work) {
                    if (work & kor::TimerWork::Rays) return eRays;
                    if (work & kor::TimerWork::Dispatch) return eCompute;
                    if (work & kor::TimerWork::Draw) return eDraw;
                    if (work & kor::TimerWork::Transfer) return eTransfer;
                    return eNothing;
                };
                const auto workSaid = [](const std::uint8_t work) {
                    std::string text;
                    const std::pair<std::uint8_t, const char*> named[] { { kor::TimerWork::Draw, "draws" }, { kor::TimerWork::Dispatch, "dispatches" },
                                                                         { kor::TimerWork::Rays, "traces rays" }, { kor::TimerWork::Transfer, "copies or clears" } };
                    for (const auto& [bit, name] : named)
                        if (work & bit) text += std::format("{}{}", text.empty() ? "" : ", ", name);
                    return text.empty() ? std::string("records nothing") : text;
                };

                // The lanes: the CPU's threads that did something, then the GPU's queues.
                std::set<int> threads { 0 };
                bool async = false, gpu = false, oneClock = true;
                for (const auto& pass : frame.passes) {
                    if (pass.recordThread >= 0 && pass.cpuRecordStartMs >= 0.0) threads.insert(pass.recordThread);
                    if (pass.gpuStartMs >= 0.0) { gpu = true; async = async || pass.async; oneClock = oneClock && pass.gpuOnCpuClock; }
                }
                std::vector<std::string> lanes;
                std::map<int, int> laneOfThread;
                for (const int thread : threads) {
                    laneOfThread[thread] = static_cast<int>(lanes.size());
                    lanes.push_back(thread == 0 ? std::string("Main thread") : std::format("Worker {}", lanes.size()));     // numbered as they are shown
                }
                const int cpuLanes = static_cast<int>(lanes.size());
                const int graphicsLane = static_cast<int>(lanes.size());
                if (gpu) lanes.push_back("Graphics");
                const int asyncLane = async ? static_cast<int>(lanes.size()) : graphicsLane;
                if (gpu && async) lanes.push_back("Async compute");

                struct Block { std::string node, name, said; int lane; double start, ms; Kind kind; bool dimmed; };
                auto blocks = std::make_shared<std::vector<Block>>();
                const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                double from = 0.0, to = 0.0;
                for (const auto& pass : frame.passes) {
                    const bool editors = pass.node.starts_with('$');
                    std::string name = editors ? "editor: " + pass.node.substr(pass.node.find(' ') + 1) : said(pass.node);
                    if (pass.name.contains('\n')) name += " (" + pass.name.substr(pass.name.find('\n') + 1) + ")";
                    const bool dimmed = !wanted.empty() && lower(name).find(wanted) == std::string::npos;
                    const auto add = [&](const int lane, const double start, const double ms, const Kind kind, std::string what) {
                        blocks->push_back({ pass.node, name, std::move(what), lane, start, ms, editors && kind < ePrepare ? eEditor : kind, dimmed });
                        from = std::min(from, start);
                        to = std::max(to, start + ms);
                    };
                    if (pass.cpuPrepareStartMs >= 0.0) add(laneOfThread[0], pass.cpuPrepareStartMs, pass.cpuPrepareMs, ePrepare, "prepared, on the main thread");
                    if (pass.cpuRecordStartMs >= 0.0 && laneOfThread.contains(pass.recordThread))
                        add(laneOfThread[pass.recordThread], pass.cpuRecordStartMs, pass.cpuRecordMs, eRecord, std::format("recorded, on {}", lanes[laneOfThread[pass.recordThread]]));
                    if (pass.gpuStartMs >= 0.0)
                        add(pass.async ? asyncLane : graphicsLane, pass.gpuStartMs, pass.gpuLastMs, kindOf(pass.work),
                            std::format("on the {} queue: {}", pass.async ? "async compute" : "graphics", workSaid(pass.work)));
                }
                if (blocks->empty() || to <= from) return kgui::Muted("Nothing of a frame has been timed yet.");

                constexpr float Label = 112.f, Lane = 22.f, Axis = 18.f, Section = 18.f;
                const float height = Axis + Section + Lane * static_cast<float>(cpuLanes) + (gpu ? Section + Lane * static_cast<float>(lanes.size() - cpuLanes) : 0.f);
                const auto laneTop = [cpuLanes](const int lane) {
                    return Axis + Section + Lane * static_cast<float>(lane) + (lane >= cpuLanes ? Section : 0.f);
                };
                // What of the frame is in view, as shares of it: zoomed and moved along by the wheel and a drag.
                const double v0 = _timelineFrom, v1 = std::min(1.0, _timelineFrom + 1.0 / _timelineZoom);
                auto seen = std::make_shared<kor::Vec2>();      // the canvas's size, as last painted: what the pointer is measured against
                const std::string picked = _session->selectedNode;
                const int hovered = _timelineHover;
                kui::Widget canvas = kui::CustomPaint([=](kui::Canvas& c, const kor::Vec2 size) {
                    *seen = size;
                    const float span = std::max(size.x - Label - 4.f, 1.f);
                    const auto x = [&](const double ms) { return Label + static_cast<float>(((ms - from) / (to - from) - v0) / (v1 - v0)) * span; };
                    // The sections and their lanes, named down the left.
                    c.DrawText("CPU", { 0.f, Axis + 2.f }, kui::TextStyle {}.SetSize(10.5f).SetColor(t.textMuted));
                    if (gpu) c.DrawText(oneClock ? "GPU" : "GPU  (its own clock)", { 0.f, laneTop(cpuLanes) - Section + 2.f }, kui::TextStyle {}.SetSize(10.5f).SetColor(t.textMuted));
                    for (int lane = 0; lane < static_cast<int>(lanes.size()); ++lane) {
                        const float top = laneTop(lane);
                        c.DrawRect(kui::Rect::LTRB(Label, top + 1.f, size.x, top + Lane - 1.f), kui::Paint::Fill(t.text.WithAlpha(lane % 2 ? 0.025f : 0.045f)));
                        c.DrawText(lanes[lane], { 8.f, top + 4.f }, kui::TextStyle {}.SetSize(11.5f).SetColor(t.text));
                    }
                    // The time along the top, as fine as the part in view is short.
                    const double shownMs = (to - from) * (v1 - v0);
                    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(shownMs / 5.0))) + 1, 1, 6);     // enough that no two ticks read alike
                    c.Save();
                    c.ClipRect(kui::Rect::LTRB(Label, 0.f, size.x, size.y));
                    for (int tick = 0; tick <= 5; ++tick) {
                        const double ms = from + (to - from) * (v0 + (v1 - v0) * tick / 5.0);
                        const float at = x(ms);
                        c.DrawLine({ at, Axis - 3.f }, { at, size.y }, kui::Paint::Stroked(t.text.WithAlpha(0.07f), 1.f));
                        const std::string text = std::format("{:.{}f} ms", ms, decimals);
                        c.DrawText(text, { tick == 5 ? at - 6.5f * static_cast<float>(text.size()) : tick == 0 ? at + 2.f : at - 3.2f * static_cast<float>(text.size()), 0.f },
                                   kui::TextStyle {}.SetSize(10.5f).SetColor(t.textMuted));
                    }
                    // When the frame started on the CPU: what every time is from.
                    if (oneClock && from < 0.0) c.DrawLine({ x(0.0), Axis }, { x(0.0), size.y }, kui::Paint::Stroked(t.text.WithAlpha(0.3f), 1.f));
                    for (std::size_t i = 0; i < blocks->size(); ++i) {
                        const Block& block = (*blocks)[i];
                        const float top = laneTop(block.lane);
                        const float left = x(block.start), right = std::max(x(block.start + block.ms), left + 1.5f);
                        if (right < Label || left > size.x) continue;
                        const kui::Rect rect = kui::Rect::LTRB(left, top + 3.f, right, top + Lane - 3.f);
                        const bool lit = block.kind != eEditor && onGraph(block.node) == picked;
                        kui::Color fill = colours[block.kind];
                        if (block.dimmed) fill = fill.WithAlpha(0.2f);
                        c.DrawRRect({ rect, kui::Radii(2.f) }, kui::Paint::Fill(fill));
                        if (static_cast<int>(i) == hovered || lit) c.DrawRRect({ rect, kui::Radii(2.f) }, kui::Paint::Stroked(t.text, lit ? 1.5f : 1.f));
                        if (rect.Width() > 30.f) {
                            c.Save();
                            c.ClipRect(kui::Rect::LTRB(std::max(rect.left, Label) + 3.f, rect.top, rect.right - 2.f, rect.bottom));
                            c.DrawText(block.name, { std::max(rect.left, Label) + 4.f, rect.top + 2.f }, kui::TextStyle {}.SetSize(10.5f).SetColor(kui::colors::White.WithAlpha(block.dimmed ? 0.4f : 0.95f)));
                            c.Restore();
                        }
                    }
                    c.Restore();
                }, { -1.f, height });
                // The block under the pointer: the last drawn of those there, as it is the one on top.
                const auto under = [=](const kor::Vec2 at) -> int {
                    const float span = std::max(seen->x - Label - 4.f, 1.f);
                    for (int i = static_cast<int>(blocks->size()) - 1; i >= 0; --i) {
                        const Block& block = (*blocks)[i];
                        const float top = laneTop(block.lane);
                        const float left = Label + static_cast<float>(((block.start - from) / (to - from) - v0) / (v1 - v0)) * span;
                        const float right = std::max(Label + static_cast<float>(((block.start + block.ms - from) / (to - from) - v0) / (v1 - v0)) * span, left + 3.f);
                        if (at.x >= std::max(left, Label) && at.x <= right && at.y >= top && at.y < top + Lane) return i;
                    }
                    return -1;
                };
                canvas = kui::GestureDetector(kui::GestureOptions {}
                    .OnHover([this, under](const kor::Vec2 at) {
                        _timelineAt = at.x;     // where the pointer is, for the wheel to zoom about
                        if (const int i = under(at); i != _timelineHover) SetState([&] { _timelineHover = i; });
                    })
                    .OnExit([this] { if (_timelineHover >= 0) SetState([&] { _timelineHover = -1; }); })
                    .OnTap([this, blocks, onGraph] {
                        if (_timelineHover < 0 || _timelineHover >= static_cast<int>(blocks->size())) return;
                        if (const Block& block = (*blocks)[_timelineHover]; block.kind != eEditor && !block.node.starts_with('$')) _session->GoTo(onGraph(block.node));
                    })
                    // The wheel zooms about the pointer, up to a thousand times; a drag moves along what is zoomed in on.
                    .OnScroll([this, seen](const kor::Vec2 delta) {
                        const double span = std::max(seen->x - Label - 4.f, 1.f);
                        const double at = std::clamp((static_cast<double>(_timelineAt) - Label) / span, 0.0, 1.0);
                        const double width = 1.0 / _timelineZoom, pointed = _timelineFrom + at * width;
                        const double zoom = std::clamp(_timelineZoom * std::pow(1.2, delta.y), 1.0, 1000.0);
                        SetState([&] { _timelineZoom = zoom; _timelineFrom = std::clamp(pointed - at / zoom, 0.0, 1.0 - 1.0 / zoom); });
                        return true;
                    })
                    .OnPanUpdate([this, seen](const kor::Vec2 delta, const kor::Vec2 at) {
                        _timelineAt = at.x;
                        if (_timelineZoom <= 1.0) return;
                        const double span = std::max(seen->x - Label - 4.f, 1.f);
                        SetState([&] { _timelineFrom = std::clamp(_timelineFrom - delta.x / span / _timelineZoom, 0.0, 1.0 - 1.0 / _timelineZoom); });
                    }), canvas);

                // Under it: the block under the pointer, said; and what each colour is.
                std::string told;
                if (hovered >= 0 && hovered < static_cast<int>(blocks->size())) {
                    const Block& block = (*blocks)[hovered];
                    told = std::format("{}  ·  {}  ·  starts at {:.3f} ms, takes {:.3f} ms", block.name, block.said, block.start, block.ms);
                }
                std::set<int> present;
                for (const auto& block : *blocks) present.insert(block.kind);
                std::vector<kui::Widget> legend;
                for (const int kind : present)
                    legend.push_back(kui::Row({
                        kui::Container(kui::ContainerOptions {}.SetWidth(10.f).SetHeight(10.f).SetDecoration(kui::Decoration {}.SetColor(colours[kind]).SetRadius(2.f))),
                        kgui::Muted(kinds[kind], 11.f) }, kui::FlexOptions {}.SetGap(5.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)));
                legend.push_back(kui::Expanded(kui::SizedBox(0.f, 0.f)));
                if (_timelineZoom > 1.0)
                    legend.push_back(Chip(std::format("Fit  ·  {:.0f}×", _timelineZoom), [this] { SetState([&] { _timelineZoom = 1.0; _timelineFrom = 0.0; }); }));
                return Rows({
                    std::move(canvas),
                    kui::Row(std::move(legend), kui::FlexOptions {}.SetGap(14.f).SetCrossAxisAlignment(kui::CrossAxisAlignment::eCenter)),
                    told.empty()
                        ? kgui::Muted(std::format("The newest frame the GPU has measured, {:.2f} ms from its first pass being prepared to its last ending{}. "
                                                  "The wheel zooms, a drag moves along; point at a block to see it, press it to go to its node.",
                                                  to - std::min(from, 0.0), oneClock ? "" : ". This device cannot line its clock up with the CPU's: the GPU's lanes start at its first pass"), 11.f)
                        : kui::Text(told, kui::TextStyle {}.SetSize(12.f)),
                }, 6.f);
            }

            // The Frame panel: the rate a frame is measured against, how its passes are shown, what is typed to find,
            // the lines with steps opened, whether what did not run is opened, and what was last read of the frame, and when.
            int _frameHz = 60;
            int _frameView = 0;     // its passes in the order they run, by cost, or on a timeline
            bool _framePaused = false, _frameNotRunOpen = false;
            // The frame as it was last read — paused, as it was when it was — and what is under the pointer on its timeline.
            struct FrameShot { kgraph::Runtime::FrameReport frame; kgraph::Runtime::FrameTimes times; double frameMs = 0.0, prepareMs = 0.0; int fps = 0; };
            FrameShot _frameShot;
            int _timelineHover = -1;                    // the block of the timeline under the pointer
            double _timelineZoom = 1.0, _timelineFrom = 0.0;   // how far in it is zoomed, and where what is in view starts, as a share of the frame
            float _timelineAt = 0.f;                    // where the pointer is across it, for the wheel to zoom about
            std::string _frameFilter;
            std::set<std::string> _frameOpen;
            Value _frameSeen;
            std::chrono::steady_clock::time_point _frameReadAt {};

            /**
             * What is wrong, told to the log as it comes — with what to do about it, where that is known — and as it goes.
             * A few times a second, whatever panel is open: what went wrong is in the log whether it was in sight or not.
             */
            void ReportProblems()
            {
                std::set<std::string> now;
                for (auto& problem : _session->Problems()) {
                    if (!_reported.contains(problem.text)) {
                        const std::string hint = HintFor(problem.text);
                        const std::string said = hint.empty() ? problem.text : std::format("{}\n→ {}", problem.text, hint);
                        if (problem.warning) kor::log::Warn("{}", said);
                        else kor::log::Error("{}", said);
                    }
                    now.insert(std::move(problem.text));
                }
                for (const auto& gone : _reported)
                    if (!now.contains(gone)) kor::log::Info("Resolved: {}", gone);
                _reported = std::move(now);
            }

            std::shared_ptr<Session> _session;
            std::shared_ptr<kui::DockLayout> _layout;
            std::uint64_t _shown = 0;
            std::set<std::string> _reported;     // what is wrong, as the log was last told
            std::chrono::steady_clock::time_point _problemsAt {};
            Dialog _dialog = Dialog::eNone;
            bool _viewModesOpen = false, _gridOpen = false;     // the Scene panel's popups: how it shows the scene, the grid's settings
            bool _snapOpen = false, _viewsOpen = false;         // how the gizmo snaps; which way the view looks, and its bookmarks
            kor::Vec2 _addAt {};
            std::string _filter, _newId;
            std::string _menuCategory;      // the node menu's: the category shown, empty for all of them
            std::string _menuPreview;       // and the node type previewed: the one last pointed at
            int _newKind = 0;
            // The graph's view: where it looks, shared with the node editor; what a wire let go over nothing waits for;
            // and the page as it was last shown, for putting it in order.
            std::shared_ptr<kui::NodeEditorView> _view = _session->graphView;
            std::optional<LooseWire> _wire;
            std::string _wireOnto;   // the node a wire was let go on, for the port it should go to
            // The Assets panel: the folder that is picked in it, the folders shut down its side, what was last pressed
            // and when (twice in a row opens), and what is asked about before it goes to the trash.
            // The outliner: what is looked for in it, the entity whose name is being typed over, and where what is held
            // over it would go. The inspector: the components shut in it.
            /**
             * Components to choose from, many (@p many: `with`, `without`) or one (`component`): a search above a box of
             * them, those chosen first, the rest those whose names have what is typed — those that start with it first.
             * Enter ticks the first found. However many components there are, what is looked for is a few letters away.
             */
            kui::Widget ComponentPicker(std::vector<std::string> names, const Value& value, const std::function<void(Value)>& set, const std::string& key, const bool many)
            {
                std::vector<std::string> on;
                if (many) {
                    if (const auto* items = value.Items())
                        for (const auto& item : *items) if (item.String() && !item.String()->empty()) on.push_back(*item.String());
                } else if (value.String() && !value.String()->empty()) {
                    on.push_back(*value.String());
                }
                for (const auto& name : on) if (!std::ranges::contains(names, name)) names.push_back(name);
                const auto lower = [](std::string text) { std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; };
                const std::string typed = _componentSearch.contains(key) ? _componentSearch.at(key) : std::string();
                const std::string wanted = lower(typed);
                // Those found: the chosen first, as they were chosen; then those that start with what is typed; then those that have it.
                std::vector<std::string> found = on, starting, having;
                for (const auto& name : names) {
                    if (std::ranges::contains(on, name)) continue;
                    const std::string folded = lower(name);
                    if (wanted.empty() || folded.starts_with(wanted)) starting.push_back(name);
                    else if (folded.find(wanted) != std::string::npos) having.push_back(name);
                }
                std::ranges::sort(starting);
                std::ranges::sort(having);
                found.insert(found.end(), starting.begin(), starting.end());
                found.insert(found.end(), having.begin(), having.end());
                const auto choose = [set, on, many](const std::string& name, const bool chosen) {
                    if (!many) { set(Value(chosen ? name : std::string())); return; }
                    auto changed = on;
                    if (chosen && !std::ranges::contains(changed, name)) changed.push_back(name);
                    if (!chosen) std::erase(changed, name);
                    set(Value(Value::List(changed.begin(), changed.end())));
                };
                const std::string first = found.size() > on.size() ? found[on.size()] : std::string();
                std::vector<kui::Widget> rows;
                rows.push_back(kui::TextField(kui::TextFieldOptions { .text = typed, .placeholder = names.empty() ? "There are no components yet" : std::format("Find a component: {} of them", names.size()),
                    .onChanged = [this, key](const std::string& text) { SetState([&] { _componentSearch[key] = text; }); },
                    .onSubmitted = [this, key, first, choose](const std::string&) {
                        if (first.empty()) return;
                        choose(first, true);
                        SetState([&] { _componentSearch.erase(key); });
                    } }).Key(key + " search"));
                std::vector<kui::Widget> box;
                constexpr std::size_t Most = 200;
                for (std::size_t i = 0; i < found.size() && i < Most; ++i) {
                    const std::string& name = found[i];
                    box.push_back(kui::Checkbox(std::ranges::contains(on, name), [choose, name](const bool checked) { choose(name, checked); }, name));
                }
                if (found.size() > Most) box.push_back(kgui::Muted(std::format("{} more: type more of a name", found.size() - Most)));
                if (box.empty()) box.push_back(kgui::Muted(names.empty() ? "there are none yet" : "none has that in its name"));
                const float tall = std::min(static_cast<float>(box.size()) * 22.f + 8.f, 180.f);
                rows.push_back(kui::ScrollView(kui::Padding(kui::EdgeInsets::All(4.f),
                    kui::Column(std::move(box), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStart).SetGap(2.f)))).Height(tall));
                return kui::Column(std::move(rows), kui::FlexOptions {}.SetCrossAxisAlignment(kui::CrossAxisAlignment::eStretch).SetGap(4.f));
            }

            std::string _outlinerFilter;
            std::map<std::string, std::string, std::less<>> _componentSearch;     // what is typed in each component picker, by its input
            kecs::Entity _renaming = kecs::NullEntity, _dropOn = kecs::NullEntity;
            Session::Beside _dropWhere = Session::Beside::eUnder;
            std::set<std::string> _componentsShut;
            std::set<std::string> _unusedOpen;     // the materials no pass uses now, opened all the same: shut, else
            std::uint32_t _went = 0, _viewAsked = 0, _played = 0;
            int _passDrop = -1;     // the pass row a pass held over the list would take the place of
            bool _overview = false; // the page shown is every node at once, of passes: its own layout
            std::string _passHover; // the line of the pass list under the pointer
            bool _addingPass = false;  // the pass list asks for a new pass's name
            bool _everyChoice = false;  // the pass list lists the pages of what the choices do not choose too
            Session::ShaderChoice _shaderChoice;    // what the New shader dialog has ticked
            kgraph::PassRate _addingRate = kgraph::PassRate::eFrame;  // the lane of the Outline a new page's name is asked for in
            std::set<std::string> _lanesShut;   // the Outline's lanes folded
            bool _drawerValues = false; // and it lists values too, not only resources
            std::uint32_t _passFocus = 0;
            std::vector<std::string> _functionOf;   // the nodes a function is being made of
            // A model being imported: its file, what its meshes have (once known), and what of it is to be read.
            int _made = 0;
            std::vector<PlacedNode> _placed;
            std::vector<PlacedWire> _wires;     ///< The page's wires, at their ports: what putting it in order goes by.
            // The Assets panel, and the dialogs of its: this window's to draw, sharing which dialog is open.
            AssetsView _assets { _session,
                                 AssetsView::Host { [this](std::function<void()> change) { SetState(std::move(change)); },
                                                    [this](const std::string& component, const std::string& field, const std::string& type, const Value& value,
                                                           const std::function<void(const Value&)>& set) { return HeldField(component, field, type, value, set); } },
                                 _dialog, _newId, _newKind };
        };
    }

    std::optional<kgraph::Runtime::Timing> CostOf(const std::map<std::string, kgraph::Runtime::Timing, std::less<>>& timings, const std::string& id)
    {
        std::optional<kgraph::Runtime::Timing> sum;
        // What it stands for starts with its id, so it is all after where the id would be.
        for (auto it = timings.lower_bound(id); it != timings.end() && it->first.starts_with(id); ++it) {
            const std::string& name = it->first;
            if (name.size() != id.size() && name[id.size()] != '/' && name.compare(id.size(), 2, " [") != 0) continue;
            if (!sum) sum.emplace();
            sum->gpuMs += it->second.gpuMs;
            sum->recordMs += it->second.recordMs;
            sum->prepareMs += it->second.prepareMs;
            sum->gpuMeasured = sum->gpuMeasured || it->second.gpuMeasured;
            sum->runs += it->second.runs;
        }
        return sum;
    }

    std::vector<PortChoice> PortChoices(const Document& doc, const std::string& fromNode, const std::string& fromPort, const bool fromOutput, const std::string& node)
    {
        const kgraph::Runtime& runtime = doc.Runtime();
        const kgraph::Node* onto = doc.Graph().Find(node);
        const kgraph::Node* source = doc.Graph().Find(fromNode);
        if (!onto || !source || fromNode == node) return {};
        // Every input a node has, as it is set: shown or not, and those that do nothing as it is set.
        const auto inputsOf = [&](const kgraph::Node& of) {
            std::vector<kgraph::Port> ports, more;
            for (auto& shown : InputsOf(runtime, of, [](const std::string&) { return true; }, &more)) ports.push_back(std::move(shown.port));
            for (auto& port : more) if (port.name.find("[]") == std::string::npos) ports.push_back(std::move(port));
            return ports;
        };
        // What the wire carries: its own end's kind.
        std::string carried;
        for (const auto& port : fromOutput ? runtime.OutputsOf(fromNode) : inputsOf(*source))
            if (port.name == fromPort) { carried = WireType(port); break; }
        if (carried.empty()) return {};
        const auto said = [](const std::string& kind) { return TypeLabel(kind.substr(2)); };

        std::vector<PortChoice> choices;
        for (const auto& port : fromOutput ? inputsOf(*onto) : runtime.OutputsOf(node)) {
            PortChoice choice { .port = port.name, .label = fromOutput || port.name != "out" ? PortLabel(port.name) : TypeLabel(port.type) };
            const std::string kind = WireType(port);
            choice.fits = fromOutput ? WireFits(carried, kind) : WireFits(kind, carried);
            if (!choice.fits)
                choice.why = kind.starts_with("v:") != carried.starts_with("v:")
                           ? (kind.starts_with("v:") ? "takes a value; the wire is of a resource" : std::format("is of a {}; the wire is of a value", said(kind)))
                           : std::format("is of a {}; the wire is of a {}", said(kind), said(carried));
            if (fromOutput)
                if (const auto input = onto->inputs.find(port.name); input != onto->inputs.end() && input->second.link)
                    choice.wiredFrom = std::format("{} of {}", PortLabel(input->second.link->output), input->second.link->node);
            // A Bundle into what fills its inputs by name says how many of them it fills.
            if (choice.fits && carried == "v:bundle" && (port.name == "fields" || port.name == "push")) {
                const std::string prefix = port.name == "fields" ? "field." : "push.";
                const auto members = runtime.BundleOf(fromNode, fromPort);
                const auto ports = inputsOf(*onto);
                const auto matched = std::ranges::count_if(members, [&](const std::string& member) {
                    return std::ranges::contains(ports, prefix + member, &kgraph::Port::name);
                });
                choice.label = std::format("By name: {} of its {} members", matched, members.size());
                choice.byName = true;
            }
            choices.push_back(std::move(choice));
        }
        // What fits first, each in the node's order — what a Bundle fills by name before all — and what doesn't after.
        std::ranges::stable_partition(choices, &PortChoice::fits);
        std::ranges::stable_partition(choices, &PortChoice::byName);
        return choices;
    }

    namespace {
        /** Nodes laid out by how they are wired, a column after another, with no loop of them kept apart from the rest. */
        std::map<std::string, std::pair<double, double>> ArrangeFlat(const std::vector<PlacedNode>& nodes, const std::vector<PlacedWire>& wires, float aspect);
    }

    std::map<std::string, std::pair<double, double>> ArrangeNodes(const std::vector<PlacedNode>& nodes, const std::vector<std::pair<std::string, std::string>>& wires,
                                                                  const float aspect)
    {
        std::vector<PlacedWire> placed;
        placed.reserve(wires.size());
        for (const auto& [from, to] : wires) placed.push_back({ from, to });
        return ArrangeNodes(nodes, placed, aspect);
    }

    std::map<std::string, std::pair<double, double>> ArrangeNodes(const std::vector<PlacedNode>& nodes, const std::vector<PlacedWire>& wires, const float aspect)
    {
        // Each loop is laid out by itself, as a block of its own; then the page, with each loop's block as one node wired as
        // its nodes are; then each loop's nodes where its block went. So the frame round a loop holds its nodes and nothing
        // else, and no two loops' frames are one over the other.
        std::map<std::string, std::vector<PlacedNode>> groups;
        for (const auto& node : nodes)
            if (!node.group.empty()) groups[node.group].push_back(node);
        std::erase_if(groups, [](const auto& group) { return group.second.size() < 2; });   // one alone is a node like any
        if (groups.empty()) return ArrangeFlat(nodes, wires, aspect);

        // Round a block, room for its frame as the graph draws it — further out by a margin and a title for each frame inside
        // it — and a little more.
        const auto room = [](const int depth) {
            const float pad = 16.f + 12.f * static_cast<float>(depth);
            return std::tuple { pad + 8.f, 26.f + pad - 8.f + 26.f * static_cast<float>(depth) + 8.f, pad + 8.f };     // side, top, bottom
        };
        std::map<std::string, std::string> blockOf;     // a node of a loop: its loop's block
        std::map<std::string, std::map<std::string, std::pair<double, double>>> inside;
        std::vector<PlacedNode> page;
        for (const auto& [group, members] : groups) {
            const std::string block = "\x1f" + group;
            const auto [Side, Top, Bottom] = room(std::ranges::max(members, {}, &PlacedNode::depth).depth);
            std::vector<PlacedNode> plain = members;
            for (auto& member : plain) { member.group.clear(); blockOf[member.id] = block; }
            std::vector<PlacedWire> among;
            for (const auto& wire : wires)
                if (std::ranges::contains(plain, wire.from, &PlacedNode::id) && std::ranges::contains(plain, wire.to, &PlacedNode::id)) among.push_back(wire);
            auto laid = ArrangeFlat(plain, among, 0.f);
            // From its own corner, and how big it came to be.
            double left = std::numeric_limits<double>::max(), top = left, right = -left, bottom = -left;
            for (const auto& member : plain) {
                const auto [x, y] = laid.at(member.id);
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x + member.width);
                bottom = std::max(bottom, y + member.height);
            }
            for (auto& [id, at] : laid) at = { at.first - left + Side, at.second - top + Top };
            inside[block] = std::move(laid);
            PlacedNode stands { block, members.front().x, members.front().y, static_cast<float>(right - left) + 2.f * Side, static_cast<float>(bottom - top) + Top + Bottom, {}, 0 };
            for (const auto& member : members) { stands.x = std::min(stands.x, member.x); stands.y = std::min(stands.y, member.y); }
            page.push_back(std::move(stands));
        }
        for (const auto& node : nodes)
            if (!blockOf.contains(node.id)) { page.push_back(node); page.back().group.clear(); }
        // Wired as its nodes are: a wire into or out of a loop is into or out of its block, where in the block its node's port came to be.
        std::map<std::string, const PlacedNode*> byId;
        for (const auto& node : nodes) byId[node.id] = &node;
        const auto portY = [&](const std::string& id, const float y) { return y >= 0.f ? y : byId.at(id)->height * 0.5f; };
        std::vector<PlacedWire> between;
        for (const auto& wire : wires) {
            if (!byId.contains(wire.from) || !byId.contains(wire.to)) continue;
            const auto a = blockOf.find(wire.from), b = blockOf.find(wire.to);
            PlacedWire outer { a == blockOf.end() ? wire.from : a->second, b == blockOf.end() ? wire.to : b->second, wire.fromY, wire.toY };
            if (outer.from == outer.to) continue;
            if (a != blockOf.end()) outer.fromY = static_cast<float>(inside.at(a->second).at(wire.from).second) + portY(wire.from, wire.fromY);
            if (b != blockOf.end()) outer.toY = static_cast<float>(inside.at(b->second).at(wire.to).second) + portY(wire.to, wire.toY);
            if (!std::ranges::contains(between, outer)) between.push_back(std::move(outer));
        }
        const auto placed = ArrangeFlat(page, between, aspect);
        std::map<std::string, std::pair<double, double>> where;
        for (const auto& [id, at] : placed) {
            const auto block = inside.find(id);
            if (block == inside.end()) { where[id] = at; continue; }
            for (const auto& [member, offset] : block->second) where[member] = { std::round(at.first + offset.first), std::round(at.second + offset.second) };
        }
        return where;
    }

    namespace {
    /**
     * How many of the wires between two columns cross: each from a place in one to a place in the next — a node's place
     * and how far down it its port is — crossing those it is over and under.
     */
    std::size_t Crossings(std::vector<std::pair<double, double>> wires)
    {
        // By where each starts, then where it ends: what is out of order of where they end crosses. Counted as a merge sort counts.
        std::ranges::sort(wires);
        std::vector<double> ends(wires.size()), spare(wires.size());
        for (std::size_t i = 0; i < wires.size(); ++i) ends[i] = wires[i].second;
        const std::function<std::size_t(std::size_t, std::size_t)> count = [&](const std::size_t from, const std::size_t to) -> std::size_t {
            if (to - from < 2) return 0;
            const std::size_t half = (from + to) / 2;
            std::size_t crossed = count(from, half) + count(half, to);
            std::size_t a = from, b = half, k = from;
            while (a < half && b < to) {
                if (ends[b] < ends[a]) { crossed += half - a; spare[k++] = ends[b++]; }    // over every one left of the first half
                else spare[k++] = ends[a++];
            }
            while (a < half) spare[k++] = ends[a++];
            while (b < to) spare[k++] = ends[b++];
            std::copy(spare.begin() + static_cast<std::ptrdiff_t>(from), spare.begin() + static_cast<std::ptrdiff_t>(to), ends.begin() + static_cast<std::ptrdiff_t>(from));
            return crossed;
        };
        return count(0, ends.size());
    }

    /** A wire of a page, between two of its nodes by index: how far down each it is. */
    struct Edge {
        std::size_t a, b;
        float ya, yb;
    };

    /**
     * A page in columns: each column's nodes, top to bottom, and among them a stand-in (numbered from the nodes' count
     * on) for each wire that runs past the column — the way kept for it. And each wire's ways from one column to the
     * next, a link each: from a node or a stand-in to the next, how far down each it is.
     */
    struct Layers {
        struct Link { std::size_t a, b; float ya, yb; };
        std::vector<std::vector<std::size_t>> rows;
        std::vector<Link> links;
        std::size_t count = 0;
    };

    /**
     * Each column's nodes in the order that crosses as few wires as can be found, as layered graphs are drawn: a wire across
     * several columns has a stand-in in each it passes through; each column is ordered by the middle of where what it is
     * wired to is — the ports it is wired at, so that two wires out of one node cross if their ports are the other way
     * up from what they go to — a column at a time from the left and then from the right, again and again, the best
     * kept; then two side by side are swapped while that crosses fewer. A column starts in the order its nodes were in,
     * top to bottom. Gives the columns with their stand-ins, and the links between them.
     */
    Layers OrderColumns(const std::vector<PlacedNode>& nodes, const std::vector<Edge>& edges, std::vector<std::vector<std::size_t>>& columns)
    {
        const std::size_t layers = columns.size();
        Layers out;
        out.rows = columns;
        for (auto& row : out.rows) std::ranges::stable_sort(row, {}, [&](const std::size_t i) { return nodes[i].y; });
        out.count = nodes.size();
        std::vector<int> layerOf(nodes.size(), -1);
        for (std::size_t c = 0; c < layers; ++c)
            for (const std::size_t i : columns[c]) layerOf[i] = static_cast<int>(c);
        // The links, a wire's from one column to the next, through a stand-in in each column it passes.
        for (const Edge& edge : edges) {
            const int from = layerOf[edge.a], into = layerOf[edge.b];
            if (from < 0 || into <= from) continue;     // a wire back, or in a ring: no order says where it goes
            std::size_t previous = edge.a;
            float previousY = edge.ya;
            for (int layer = from + 1; layer < into; ++layer) {
                const std::size_t standIn = out.count++;
                out.rows[static_cast<std::size_t>(layer)].push_back(standIn);
                out.links.push_back({ previous, standIn, previousY, 0.f });
                previous = standIn;
                previousY = 0.f;
            }
            out.links.push_back({ previous, edge.b, previousY, edge.yb });
        }
        if (layers < 2) { columns = out.rows; return out; }
        // How far down a node a link is, as a part of a place: a node's place and its port's share of its height, so that
        // ports one over another are in that order. A stand-in's is its middle.
        const auto share = [&](const std::size_t u, const float y) {
            if (u >= nodes.size() || nodes[u].height <= 0.f) return 0.49;
            return 0.98 * std::clamp(static_cast<double>(y) / nodes[u].height, 0.0, 1.0);
        };
        std::vector<std::vector<std::size_t>> ups(out.count), downs(out.count);     // the links into each, and out of each
        std::vector<std::pair<double, double>> shares(out.links.size());
        for (std::size_t l = 0; l < out.links.size(); ++l) {
            const auto& link = out.links[l];
            downs[link.a].push_back(l);
            ups[link.b].push_back(l);
            shares[l] = { share(link.a, link.ya), share(link.b, link.yb) };
        }
        auto& rows = out.rows;
        std::vector<int> at(out.count, 0);
        const auto number = [&](const std::size_t c) { for (std::size_t k = 0; k < rows[c].size(); ++k) at[rows[c][k]] = static_cast<int>(k); };
        for (std::size_t c = 0; c < layers; ++c) number(c);
        const auto between = [&](const std::size_t c) {
            std::vector<std::pair<double, double>> wires;
            for (const std::size_t u : rows[c])
                for (const std::size_t l : downs[u]) wires.emplace_back(at[u] + shares[l].first, at[out.links[l].b] + shares[l].second);
            return Crossings(std::move(wires));
        };
        const auto all = [&] { std::size_t sum = 0; for (std::size_t c = 0; c + 1 < layers; ++c) sum += between(c); return sum; };
        // By the middle of where its links go, on one side: one with none there stays where it is.
        const auto byMiddle = [&](const std::size_t c, const bool fromLeft) {
            std::vector<double> key(out.count, 0.0);
            for (const std::size_t u : rows[c]) {
                const auto& side = fromLeft ? ups[u] : downs[u];
                if (side.empty()) { key[u] = at[u] + 0.5; continue; }
                double sum = 0.0;
                for (const std::size_t l : side)
                    sum += fromLeft ? at[out.links[l].a] + shares[l].first : at[out.links[l].b] + shares[l].second;
                key[u] = sum / static_cast<double>(side.size());
            }
            std::ranges::stable_sort(rows[c], {}, [&](const std::size_t u) { return key[u]; });
            number(c);
        };
        // From one order of the columns: sweeps, the best of them kept, then swaps. How many cross, in the end.
        const auto around = [&](const std::size_t c) { return (c > 0 ? between(c - 1) : 0) + (c + 1 < layers ? between(c) : 0); };
        const auto settle = [&]() -> std::size_t {
            for (std::size_t c = 0; c < layers; ++c) number(c);
            auto kept = rows;
            std::size_t fewest = all();
            for (int sweep = 0; sweep < 24 && fewest > 0; ++sweep) {
                if (sweep % 2 == 0) for (std::size_t c = 1; c < layers; ++c) byMiddle(c, true);
                else for (std::size_t c = layers - 1; c-- > 0;) byMiddle(c, false);
                if (const std::size_t now = all(); now < fewest) { fewest = now; kept = rows; }
            }
            rows = std::move(kept);
            for (std::size_t c = 0; c < layers; ++c) number(c);
            // Two side by side swapped, while that crosses fewer with the columns either side.
            for (int round = 0; round < 12 && fewest > 0; ++round) {
                bool better = false;
                for (std::size_t c = 0; c < layers; ++c)
                    for (std::size_t k = 0; k + 1 < rows[c].size(); ++k) {
                        const std::size_t was = around(c);
                        std::swap(rows[c][k], rows[c][k + 1]);
                        number(c);
                        if (const std::size_t now = around(c); now < was) { better = true; fewest -= was - now; continue; }
                        std::swap(rows[c][k], rows[c][k + 1]);
                        number(c);
                    }
                if (!better) break;
            }
            return fewest;
        };
        // That can stop where no one step does better, short of the best: so it is begun again from other orders — each
        // column the other way up, and shuffled, the same shuffles each time, so that a page is always put in one order.
        const auto start = rows;
        auto best = rows;
        std::size_t fewest = std::numeric_limits<std::size_t>::max();
        std::mt19937 shuffle(1729);
        for (int attempt = 0; attempt < 8 && fewest > 0; ++attempt) {
            rows = start;
            if (attempt == 1) for (auto& row : rows) std::ranges::reverse(row);
            else if (attempt > 1) for (auto& row : rows) std::shuffle(row.begin(), row.end(), shuffle);
            if (const std::size_t now = settle(); now < fewest) { fewest = now; best = rows; }
        }
        rows = std::move(best);
        // The nodes in that order, without the stand-ins.
        for (std::size_t c = 0; c < layers; ++c) {
            columns[c].clear();
            for (const std::size_t u : rows[c]) if (u < nodes.size()) columns[c].push_back(u);
        }
        return out;
    }

    /**
     * The places in a column nearest to where each is wanted — @p wanted, weighed by @p weight — that keep them in their
     * order with @p spacing between each and the next: the least, of the squares of how far each is from where it is
     * wanted, there can be. (Each is moved up by what comes before it, and then the places are the closest that never go
     * back up: pooled, a run of them, where they would.)
     */
    std::vector<double> InOrder(const std::vector<double>& wanted, const std::vector<double>& weight, const std::vector<double>& spacing)
    {
        const std::size_t n = wanted.size();
        std::vector<double> before(n, 0.0);
        for (std::size_t k = 1; k < n; ++k) before[k] = before[k - 1] + spacing[k - 1];
        struct Pool { double sum, weight; std::size_t count; };
        std::vector<Pool> pools;
        for (std::size_t k = 0; k < n; ++k) {
            pools.push_back({ (wanted[k] - before[k]) * weight[k], weight[k], 1 });
            while (pools.size() > 1) {
                const Pool& last = pools.back();
                const Pool& previous = pools[pools.size() - 2];
                if (previous.sum / previous.weight <= last.sum / last.weight) break;
                const Pool merged { previous.sum + last.sum, previous.weight + last.weight, previous.count + last.count };
                pools.pop_back();
                pools.back() = merged;
            }
        }
        std::vector<double> places(n);
        std::size_t k = 0;
        for (const Pool& pool : pools)
            for (std::size_t i = 0; i < pool.count; ++i, ++k) places[k] = pool.sum / pool.weight + before[k];
        return places;
    }

    std::map<std::string, std::pair<double, double>> ArrangeFlat(const std::vector<PlacedNode>& nodes, const std::vector<PlacedWire>& wires, const float aspect)
    {
        std::map<std::string, std::pair<double, double>> where;
        if (nodes.empty()) return where;
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < nodes.size(); ++i) index[nodes[i].id] = i;
        std::vector<std::vector<std::size_t>> before(nodes.size()), after(nodes.size());
        std::vector<Edge> edges;
        for (const auto& wire : wires) {
            const auto a = index.find(wire.from), b = index.find(wire.to);
            if (a == index.end() || b == index.end() || a->second == b->second) continue;
            const PlacedNode& from = nodes[a->second];
            const PlacedNode& to = nodes[b->second];
            edges.push_back({ a->second, b->second, wire.fromY >= 0.f ? std::min(wire.fromY, from.height) : from.height * 0.5f,
                              wire.toY >= 0.f ? std::min(wire.toY, to.height) : to.height * 0.5f });
            before[b->second].push_back(a->second);
            after[a->second].push_back(b->second);
        }
        // The column: one more than the furthest of what it is wired from.
        std::vector<int> column(nodes.size(), -1);
        std::vector<char> visiting(nodes.size(), 0);
        const std::function<int(std::size_t)> depth = [&](const std::size_t i) {
            if (column[i] >= 0) return column[i];
            if (visiting[i]) return 0;      // wired in a ring: it ends somewhere
            visiting[i] = 1;
            int most = 0;
            for (const std::size_t from : before[i]) most = std::max(most, depth(from) + 1);
            visiting[i] = 0;
            return column[i] = most;
        };
        int last = 0;
        for (std::size_t i = 0; i < nodes.size(); ++i) last = std::max(last, depth(i));
        // Then as near what it is wired to as that leaves it: an image is beside the pass that draws into it.
        std::vector<std::size_t> order(nodes.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::stable_sort(order, [&](const std::size_t a, const std::size_t b) { return column[a] > column[b]; });
        for (const std::size_t i : order) {
            if (after[i].empty()) continue;
            int nearest = last + 1;
            for (const std::size_t to : after[i]) nearest = std::min(nearest, column[to]);
            column[i] = std::max(column[i], nearest - 1);
        }
        std::vector<std::vector<std::size_t>> columns(static_cast<std::size_t>(last) + 1);
        std::vector<std::size_t> loose;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (before[i].empty() && after[i].empty()) loose.push_back(i);
            else columns[static_cast<std::size_t>(column[i])].push_back(i);
        }
        std::erase_if(columns, [](const auto& c) { return c.empty(); });
        const Layers layers = OrderColumns(nodes, edges, columns);
        float left = nodes.front().x, top = nodes.front().y;
        for (const auto& node : nodes) { left = std::min(left, node.x); top = std::min(top, node.y); }

        // Down each column: each node level with what it is wired to, port with port, as near as the others in its column
        // leave room for. A stand-in is a wire's way past the column: a lane kept clear for it, level with where it goes.
        constexpr float Across = 80.f, Down = 28.f, Lane = 12.f;
        const std::size_t count = layers.count;
        const auto tallOf = [&](const std::size_t u) { return u < nodes.size() ? static_cast<double>(nodes[u].height) : 0.0; };
        const auto gap = [&](const std::size_t u, const std::size_t v) { return u >= nodes.size() || v >= nodes.size() ? Lane : Down; };
        std::vector<double> y(count, 0.0);
        {
            // To begin with, each column stacked, its middle level with the tallest's.
            std::vector<double> tall(layers.rows.size(), 0.0);
            for (std::size_t c = 0; c < layers.rows.size(); ++c) {
                const auto& row = layers.rows[c];
                for (std::size_t k = 0; k < row.size(); ++k) {
                    y[row[k]] = tall[c];
                    tall[c] += tallOf(row[k]) + (k + 1 < row.size() ? gap(row[k], row[k + 1]) : 0.0);
                }
            }
            const double tallest = tall.empty() ? 0.0 : *std::ranges::max_element(tall);
            for (std::size_t c = 0; c < layers.rows.size(); ++c)
                for (const std::size_t u : layers.rows[c]) y[u] += (tallest - tall[c]) * 0.5;
        }
        // What pulls each, from either side: where it would be, level with what a link joins it to — a wire's way past
        // columns pulled harder, so that a long wire runs straight and what it passes moves aside for it.
        struct Pull { std::size_t other; double offset, weight; };
        std::vector<std::vector<Pull>> fromLeft(count), fromRight(count);
        for (const auto& link : layers.links) {
            const double weight = link.a >= nodes.size() && link.b >= nodes.size() ? 16.0 : link.a >= nodes.size() || link.b >= nodes.size() ? 8.0 : 1.0;
            fromLeft[link.b].push_back({ link.a, static_cast<double>(link.ya) - link.yb, weight });
            fromRight[link.a].push_back({ link.b, static_cast<double>(link.yb) - link.ya, weight });
        }
        const auto place = [&](const std::vector<std::size_t>& row, const bool leftSide, const bool rightSide) {
            if (row.empty()) return;
            std::vector<double> wanted(row.size()), weight(row.size()), spacing(row.size(), 0.0);
            for (std::size_t k = 0; k < row.size(); ++k) {
                const std::size_t u = row[k];
                double sum = 0.0, weights = 0.0;
                const auto pull = [&](const std::vector<Pull>& pulls) { for (const Pull& p : pulls) { sum += p.weight * (y[p.other] + p.offset); weights += p.weight; } };
                if (leftSide) pull(fromLeft[u]);
                if (rightSide) pull(fromRight[u]);
                // Nothing pulling it: it stays where it is, if the others let it.
                wanted[k] = weights > 0.0 ? sum / weights : y[u];
                weight[k] = weights > 0.0 ? weights : 0.01;
                if (k + 1 < row.size()) spacing[k] = tallOf(u) + gap(u, row[k + 1]);
            }
            const auto places = InOrder(wanted, weight, spacing);
            for (std::size_t k = 0; k < row.size(); ++k) y[row[k]] = places[k];
        };
        // Sweeps from the left, by what each is wired from, and from the right, by what it is wired to; then by both, so
        // that each is between what it takes and what it gives.
        const std::size_t columnCount = layers.rows.size();
        for (int sweep = 0; sweep < 16; ++sweep) {
            const bool both = sweep >= 10;
            if (sweep % 2 == 0) for (std::size_t c = 0; c < columnCount; ++c) place(layers.rows[c], true, both);
            else for (std::size_t c = columnCount; c-- > 0;) place(layers.rows[c], both, true);
        }
        // Across: each column as wide as its widest, and further from the next the steeper the wires between them run.
        std::vector<float> widest(columns.size(), 0.f), x(columns.size(), left);
        for (std::size_t c = 0; c < columns.size(); ++c)
            for (const std::size_t i : columns[c]) widest[c] = std::max(widest[c], nodes[i].width);
        std::vector<int> layerOf(count, -1);
        for (std::size_t c = 0; c < columnCount; ++c)
            for (const std::size_t u : layers.rows[c]) layerOf[u] = static_cast<int>(c);
        std::vector<double> steepest(columns.size(), 0.0);
        for (const auto& link : layers.links)
            if (const int c = layerOf[link.a]; c >= 0)
                steepest[static_cast<std::size_t>(c)] = std::max(steepest[static_cast<std::size_t>(c)], std::abs(y[link.b] + link.yb - y[link.a] - link.ya));
        for (std::size_t c = 1; c < columns.size(); ++c) x[c] = x[c - 1] + widest[c - 1] + Across + static_cast<float>(std::min(160.0, steepest[c - 1] * 0.15));
        double highest = std::numeric_limits<double>::max(), lowest = -highest;
        for (const auto& row : columns)
            for (const std::size_t i : row) { highest = std::min(highest, y[i]); lowest = std::max(lowest, y[i] + nodes[i].height); }
        for (std::size_t c = 0; c < columns.size(); ++c)
            for (const std::size_t i : columns[c]) where[nodes[i].id] = { std::round(x[c]), std::round(top + y[i] - highest) };
        float wide = columns.empty() ? 0.f : x.back() + widest.back() - left, high = columns.empty() ? 0.f : static_cast<float>(lowest - highest);

        // As the view is shaped: when the page so laid out is far from that, a column that runs past a height is wrapped into
        // another beside it, in the same order — the height lowered until the whole is as wide, for how tall it is, as the view.
        if (aspect > 0.f && !columns.empty()) {
            float shortest = 0.f;
            for (const auto& row : columns)
                for (const std::size_t i : row) shortest = std::max(shortest, nodes[i].height);
            std::vector<std::pair<float, float>> at(nodes.size(), { 0.f, 0.f });     // where each is, as laid out at the height tried
            const auto lay = [&](const float limit, const bool place) {
                float cx = left, most = 0.f;
                for (std::size_t c = 0; c < columns.size(); ++c) {
                    float y = top;
                    for (const std::size_t i : columns[c]) {
                        if (y > top && y - top + nodes[i].height > limit) { cx += widest[c] + Across * 0.5f; y = top; }
                        at[i] = { cx, y };
                        if (place) where[nodes[i].id] = { std::round(cx), std::round(y) };
                        most = std::max(most, y - top + nodes[i].height);
                        y += nodes[i].height + Down;
                    }
                    cx += widest[c] + Across;
                }
                return std::pair { cx - left - Across, most };
            };
            // What is wired to nothing goes under it, in rows as wide as it: as tall as that makes them.
            const auto underneath = [&](const float across) {
                float lx = 0.f, rows = 0.f, row = 0.f;
                for (const std::size_t i : loose) {
                    if (lx > 0.f && lx + nodes[i].width > across) { lx = 0.f; rows += row + 40.f; row = 0.f; }
                    lx += nodes[i].width + 40.f;
                    row = std::max(row, nodes[i].height);
                }
                return loose.empty() ? 0.f : rows + row + 60.f;
            };
            // How many wires cross, as laid out: each straight from its port on the one to its port on the other.
            const auto crossed = [&] {
                struct Line { float x1, y1, x2, y2; std::size_t a, b; };
                std::vector<Line> lines;
                for (const Edge& e : edges) lines.push_back({ at[e.a].first + nodes[e.a].width, at[e.a].second + e.ya, at[e.b].first, at[e.b].second + e.yb, e.a, e.b });
                const auto side = [](const Line& l, const float px, const float py) { return (l.x2 - l.x1) * (py - l.y1) - (l.y2 - l.y1) * (px - l.x1); };
                std::size_t crossings = 0;
                for (std::size_t i = 0; i < lines.size(); ++i)
                    for (std::size_t j = i + 1; j < lines.size(); ++j) {
                        const Line& p = lines[i];
                        const Line& q = lines[j];
                        if (p.a == q.a || p.b == q.b || p.a == q.b || p.b == q.a) continue;
                        if (side(p, q.x1, q.y1) * side(p, q.x2, q.y2) < 0.f && side(q, p.x1, p.y1) * side(q, p.x2, p.y2) < 0.f) ++crossings;
                    }
                return crossings;
            };
            const auto offShape = [&](const float w, const float h) { return std::abs(std::log(w / std::max(h + underneath(std::max(w, 600.f)), 1.f) / aspect)); };
            // The page as laid out, each level with what it is wired to.
            for (const auto& row : columns)
                for (const std::size_t i : row) at[i] = { static_cast<float>(where[nodes[i].id].first), static_cast<float>(where[nodes[i].id].second) };
            const float levelOff = offShape(wide, high);
            const std::size_t levelCrossings = crossed();
            // Of the heights there are to wrap at, those whose whole is shaped nearly as the view, and of those the one whose
            // wires cross least: a column wrapped can cross what it did not.
            struct Tried { float limit, off; std::size_t crossings; };
            std::vector<Tried> tried;
            float nearest = levelOff;
            float tallest = 0.f;
            for (const auto& row : columns) {
                float tall = 0.f;
                for (const std::size_t i : row) tall += nodes[i].height + Down;
                tallest = std::max(tallest, tall - Down);
            }
            for (float limit = tallest; limit >= std::max(shortest, 1.f); limit *= 0.94f) {
                const auto [w, h] = lay(limit, false);
                const float off = offShape(w, h);
                tried.push_back({ limit, off, crossed() });
                nearest = std::min(nearest, off);
            }
            constexpr float Near = 0.3f;    // in the log of width over height: within about a third of the view's shape
            float limit = tallest;
            std::size_t fewest = std::numeric_limits<std::size_t>::max();
            float best = std::numeric_limits<float>::max();
            for (const Tried& one : tried)
                if (one.off <= nearest + Near && (one.crossings < fewest || (one.crossings == fewest && one.off < best))) {
                    fewest = one.crossings;
                    best = one.off;
                    limit = one.limit;
                }
            // Level is kept unless it is far from the view's shape — about twice as wide, or as tall, as the best — or crosses more.
            constexpr float Far = 0.7f;
            if (levelOff > nearest + Far || levelCrossings > fewest) std::tie(wide, high) = lay(limit, true);
        }
        // What is wired to nothing: in rows under it all, as wide as the rest (or one row, shaped by nothing).
        float lx = left, ly = top + high + (columns.empty() ? 0.f : 60.f), row = 0.f;
        const float across = aspect > 0.f ? std::max(wide, 600.f) : std::numeric_limits<float>::max();
        for (const std::size_t i : loose) {
            if (lx > left && lx - left + nodes[i].width > across) { lx = left; ly += row + 40.f; row = 0.f; }
            where[nodes[i].id] = { std::round(lx), std::round(ly) };
            lx += nodes[i].width + 40.f;
            row = std::max(row, nodes[i].height);
        }
        return where;
    }
    }

    kui::Widget EditorRoot(std::shared_ptr<Session> session) { return kui::Make<Root>(std::move(session)); }

    // ---- the scene ----------------------------------------------------------------------------------------------

    EditorScene::EditorScene(std::filesystem::path project) : _projectPath(std::move(project)) {}
    EditorScene::EditorScene(const kor::SceneArgs& arguments) : _projectPath(arguments.String("project")), _scriptsHost(arguments.String("scripts")) {}
    EditorScene::~EditorScene() = default;

    void EditorScene::Initialize()
    {
        // The scene being worked on runs as it would on its own, into an image a panel shows.
        _play = kor::App::Current().OpenOffscreen<PlayScene>({ .title = "Scene", .extent = { 640, 360 } });
        // It does not play until it is told to: what is edited is the scene, not a moment of it.
        _play->SetPlaying(false);
        _session = std::make_shared<Session>(*_play);
        // What it looks like played, seen from its own camera: another view of the same graph, in a panel of its own.
        _game = kor::App::Current().OpenOffscreen<GameScene>({ .title = "Game", .extent = { 640, 360 } }, *_play);
        _session->SetGame(_game);
        // The Scene panel's second view, split beside the first: another view of the graph, seen from its own place.
        _second = kor::App::Current().OpenOffscreen<GameScene>({ .title = "Scene 2", .extent = { 640, 360 } }, *_play);
        _session->SetSecondView(_second);
        _session->LoadObjects();
        // The editor's view, its gizmo and its picking, as the scene updates: in the frame being made.
        _play->EditAs();                    // picked from and drawn over, whatever its graph is
        _play->SetInput(&_game->SceneInput(), &_game->SceneWindow());   // a script that is steered reads the Game panel's keys and mouse, in its size
        _play->OnFrame([this] { if (_session) _session->Tick(SceneInput(), static_cast<float>(SceneTime().FrameTime())); });
        Document::Alias(&Graph(), &_play->Graph());     // whoever is handed this scene is after the one it edits
        _session->HostScripts(_scriptsHost);
        if (!_projectPath.empty()) _session->OpenProject(_projectPath);
        _session->window = &SceneWindow();      // what the title bar's own buttons act on
        // In the theme this user left it in — unless it keeps nothing of theirs (a test's).
        _ui = std::make_unique<kui::Ui>(EditorRoot(_session), kui::UiSettings {}.SetTheme(std::getenv("KORAL_ENGINE_NO_LAYOUT") ? kui::Theme::Dark() : Session::SavedTheme()));
        Graph().Add<kui::UiPass>(*_ui);
    }

    void EditorScene::Update()
    {
        // Gone to another window: what was held is let go, as the letting go will not be heard. Looking round, above all.
        if (const bool focused = SceneWindow().IsFocused(); focused != _focused) {
            if (std::exchange(_focused, focused)) SceneInput().ReleaseAll();
        }
        // Looking round, the mouse's own movement: not the pointer's, a pixel of the screen at a time and sped up.
        SceneInput().SetCursorMode(_session->Aiming() ? kor::Input::CursorMode::eCaptured : kor::Input::CursorMode::eNormal);
        _session->UpdateLook(static_cast<float>(Time::FrameTime()));
        _session->UpdateThumbnails(static_cast<float>(Time::FrameTime()));
        _session->UpdateView(static_cast<float>(Time::FrameTime()));
        _ui->Update();
        // A theme chosen from a menu: put on now the menu is done with, every panel built again in it.
        if (_session->themeChosen) {
            _ui->SetTheme(*_session->themeChosen);
            _session->themeChosen.reset();
        }
        // Control+S saves, Control+R runs — unless a field is being typed into.
        const kor::Input& input = SceneInput();
        const bool control = input.IsKeyHeld(kor::Key::eLeftControl) || input.IsKeyHeld(kor::Key::eRightControl);
        // The keys an editor's hands know: taking back, and — with nothing typed into and the view not being flown —
        // the gizmo's tools, framing, and removing what is picked.
        if (!input.InterfaceWantsKeyboard()) {
            Session& session = *_session;
            const bool shift = input.IsKeyHeld(kor::Key::eLeftShift) || input.IsKeyHeld(kor::Key::eRightShift);
            const kecs::Entity picked = session.Doc().World().Alive(session.selectedEntity) ? session.selectedEntity : kecs::NullEntity;
            if (control && input.IsKeyPressed(kor::Key::eZ)) { if (shift) session.Redo(); else session.Undo(); }
            else if (control && input.IsKeyPressed(kor::Key::eY)) session.Redo();
            else if (control && input.IsKeyPressed(kor::Key::eD)) { if (!picked.IsNull()) session.Duplicate(picked); }
            else if (!control && !input.IsMouseButtonHeld(kor::MouseButton::eRight)) {
                if (input.IsKeyPressed(kor::Key::eQ)) session.SetGizmo(false, session.gizmo);
                if (input.IsKeyPressed(kor::Key::eW)) session.SetGizmo(true, kor::GizmoMode::eTranslate);
                if (input.IsKeyPressed(kor::Key::eE)) session.SetGizmo(true, kor::GizmoMode::eRotate);
                if (input.IsKeyPressed(kor::Key::eR)) session.SetGizmo(true, kor::GizmoMode::eScale);
                if (input.IsKeyPressed(kor::Key::eF) && !picked.IsNull()) session.Frame(picked);
                if (input.IsKeyPressed(kor::Key::eDelete)) {
                    if (!picked.IsNull()) session.Delete(picked);
                    else if (!session.selectedNode.empty() && !session.InPlayMode()) { session.Doc().RemoveNodes({ session.selectedNode }); session.Select(std::string()); }
                }
            }
        }
        if (!input.InterfaceWantsKeyboard() && control) {
            if (input.IsKeyPressed(kor::Key::eS)) _session->Save();
            if (input.IsKeyPressed(kor::Key::eSpace)) _session->AskForNodes();     // Control+Space: the nodes there are to add
            if (input.IsKeyPressed(kor::Key::eR)) _session->Run();
        }
    }

    void EditorScene::Shutdown()
    {
        Session::KeepWindow(SceneWindow());
        if (_play) { _play->OnFrame({}); _play->SetInput(nullptr); }
        _ui.reset();
        _session.reset();
        Document::Alias(&Graph(), nullptr);
        if (_second) kor::App::Current().Close(*_second);
        _second = nullptr;
        if (_game) kor::App::Current().Close(*_game);     // it follows the play scene's: it goes first
        _game = nullptr;
        if (_play) kor::App::Current().Close(*_play);
        _play = nullptr;
    }
}
