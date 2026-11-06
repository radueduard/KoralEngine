//
// kecs: a world to JSON and back, a field at a time, by name.
//

#include <kecs/serialize.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <set>

#include <kecs/world.h>

#include <kecs/json.h>

namespace kecs
{
    namespace {
        constexpr std::string_view BytesKey = "$bytes";

        std::string_view Trimmed(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
            return text;
        }

        template <typename T>
        Codec NumberCodec()
        {
            return {
                .write = [](const void* value, const SaveContext&) -> std::string {
                    const T v = *static_cast<const T*>(value);
                    if constexpr (std::is_floating_point_v<T>) {
                        // JSON has no word for these.
                        if (std::isnan(v)) return "\"nan\"";
                        if (std::isinf(v)) return v > 0 ? "\"inf\"" : "\"-inf\"";
                    }
                    char buffer[64];
                    const auto end = std::to_chars(buffer, buffer + sizeof buffer, v).ptr;   // the shortest text that reads back the same
                    return std::string(buffer, end);
                },
                .read = [](void* value, const std::string_view json, const LoadContext&) {
                    const std::string_view text = Trimmed(json);
                    if constexpr (std::is_floating_point_v<T>) {
                        if (text == "\"nan\"") { *static_cast<T*>(value) = std::numeric_limits<T>::quiet_NaN(); return true; }
                        if (text == "\"inf\"") { *static_cast<T*>(value) = std::numeric_limits<T>::infinity(); return true; }
                        if (text == "\"-inf\"") { *static_cast<T*>(value) = -std::numeric_limits<T>::infinity(); return true; }
                    }
                    T v {};
                    const auto [end, problem] = std::from_chars(text.data(), text.data() + text.size(), v);
                    if (problem != std::errc() || end != text.data() + text.size()) {
                        // A whole number written as one with a fraction, or the other way: read as a double.
                        double wide = 0.0;
                        const auto [wideEnd, wideProblem] = std::from_chars(text.data(), text.data() + text.size(), wide);
                        if (wideProblem != std::errc() || wideEnd != text.data() + text.size()) return false;
                        v = static_cast<T>(wide);
                    }
                    *static_cast<T*>(value) = v;
                    return true;
                },
            };
        }

        std::string Hex(const std::byte* bytes, const std::size_t count)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out(count * 2, '0');
            for (std::size_t i = 0; i < count; ++i) {
                out[i * 2] = digits[static_cast<unsigned>(bytes[i]) >> 4];
                out[i * 2 + 1] = digits[static_cast<unsigned>(bytes[i]) & 15];
            }
            return out;
        }

        bool FromHex(const std::string_view hex, std::byte* bytes, const std::size_t count)
        {
            if (hex.size() != count * 2) return false;
            const auto digit = [](const char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
            for (std::size_t i = 0; i < count; ++i) {
                const int high = digit(hex[i * 2]), low = digit(hex[i * 2 + 1]);
                if (high < 0 || low < 0) return false;
                bytes[i] = static_cast<std::byte>(high * 16 + low);
            }
            return true;
        }

        std::string BytesObject(const std::byte* bytes, const std::size_t count)
        {
            return std::format("{{ \"{}\": \"{}\" }}", BytesKey, Hex(bytes, count));
        }

        /** Whether a component type is plain data as a whole: its bytes are all there is to it. How one is first made is its own business. */
        bool Plain(const ComponentDescription& type) { return !type.copy && !type.relocate && !type.destroy; }
        /** Whether one can be made from a size and its fields alone: nothing in it owns anything. */
        bool Rebuildable(const ComponentDescription& type)
        {
            return !type.copy && !type.relocate && !type.destroy && std::ranges::none_of(type.fields, [](const FieldDescription& f) { return f.assign != nullptr; });
        }
    }

    Codecs::Codecs()
    {
        Register("f32", NumberCodec<float>());
        Register("f64", NumberCodec<double>());
        Register("i8", NumberCodec<std::int8_t>());
        Register("i16", NumberCodec<std::int16_t>());
        Register("i32", NumberCodec<std::int32_t>());
        Register("i64", NumberCodec<std::int64_t>());
        Register("u8", NumberCodec<std::uint8_t>());
        Register("u16", NumberCodec<std::uint16_t>());
        Register("u32", NumberCodec<std::uint32_t>());
        Register("u64", NumberCodec<std::uint64_t>());
        Register("bool", {
            .write = [](const void* value, const SaveContext&) -> std::string { return *static_cast<const bool*>(value) ? "true" : "false"; },
            .read = [](void* value, const std::string_view json, const LoadContext&) {
                const std::string_view text = Trimmed(json);
                if (text != "true" && text != "false") return false;
                *static_cast<bool*>(value) = text == "true";
                return true;
            },
        });
        Register("string", {
            .write = [](const void* value, const SaveContext&) { return json::Quote(*static_cast<const std::string*>(value)); },
            .read = [](void* value, const std::string_view text, const LoadContext&) {
                json::Value parsed;
                std::string error;
                if (!json::Parse(text, parsed, error) || parsed.kind != json::Value::Kind::eString) return false;
                *static_cast<std::string*>(value) = std::move(parsed.string);
                return true;
            },
        });
        // An entity is saved as its number in the file, and found again by it.
        Register("entity", {
            .write = [](const void* value, const SaveContext& context) -> std::string {
                const std::int64_t id = context.idOf ? context.idOf(*static_cast<const Entity*>(value)) : -1;
                return id < 0 ? "null" : std::to_string(id);
            },
            .read = [](void* value, const std::string_view json, const LoadContext& context) {
                const std::string_view text = Trimmed(json);
                if (text == "null") { *static_cast<Entity*>(value) = NullEntity; return true; }
                std::int64_t id = -1;
                const auto [end, problem] = std::from_chars(text.data(), text.data() + text.size(), id);
                if (problem != std::errc() || end != text.data() + text.size()) return false;
                *static_cast<Entity*>(value) = context.entityOf ? context.entityOf(id) : NullEntity;
                return true;
            },
        });
    }

    void Codecs::Register(std::string type, Codec codec) { _codecs.insert_or_assign(std::move(type), std::move(codec)); }

    const Codec* Codecs::Find(const std::string_view type) const
    {
        const auto it = _codecs.find(type);
        return it == _codecs.end() ? nullptr : &it->second;
    }

    void Codecs::Reads(std::string type, std::string was) { _reads.emplace(std::move(type), std::move(was)); }

    bool Codecs::ReadsAs(const std::string_view type, const std::string_view was) const
    {
        return type == was || _reads.contains(std::pair { std::string(type), std::string(was) });
    }

    void Codecs::Becomes(std::string was, std::function<std::string(std::string_view saved)> which) { _becomes.insert_or_assign(std::move(was), std::move(which)); }

    std::string Codecs::BecomesOf(const std::string_view was, const std::string_view saved) const
    {
        const auto it = _becomes.find(was);
        return it == _becomes.end() || !it->second ? std::string() : it->second(saved);
    }

    Codecs& Codecs::Default()
    {
        static Codecs codecs;
        return codecs;
    }

    // ---- saving --------------------------------------------------------------------------------------------------

    Saved Save(const World& world, const Codecs& codecs)
    {
        Saved saved;
        std::set<std::string> problems;

        // Parents before their children, and the top of the hierarchy in its order: read back the same way, the
        // order is the same.
        const std::vector<Entity> entities = world.All();
        std::map<Entity, std::int64_t> numbers;
        for (std::size_t i = 0; i < entities.size(); ++i) numbers.emplace(entities[i], static_cast<std::int64_t>(i));
        const SaveContext context { [&](const Entity e) { const auto it = numbers.find(e); return it == numbers.end() ? std::int64_t { -1 } : it->second; } };

        // A component as JSON, or nothing when it cannot be saved.
        const auto component = [&](const Entity e, const ComponentId id) -> std::optional<std::string> {
            const ComponentDescription& type = world.Describe(id);
            if (type.IsTag()) return "{}";
            const auto* value = static_cast<const std::byte*>(world.Get(e, id));
            if (type.fields.empty()) {
                if (Plain(type)) return BytesObject(value, type.size);
                problems.insert(std::format("'{}' declares no fields and is not plain data: not saved", type.name));
                return std::nullopt;
            }
            std::string out = "{";
            bool first = true;
            for (const auto& field : type.fields) {
                std::string text;
                if (const Codec* codec = codecs.Find(field.type); codec && codec->write) {
                    text = codec->write(value + field.offset, context);
                } else if (!field.assign) {
                    text = BytesObject(value + field.offset, field.size);   // plain data nobody can spell: kept as bytes
                } else {
                    problems.insert(std::format("'{}.{}' is a {}, which has no codec: not saved", type.name, field.name, field.type));
                    continue;
                }
                out += std::format("{} {}: {}", first ? "" : ",", json::Quote(field.name), text);
                first = false;
            }
            return out + " }";
        };

        std::set<std::string> usedTypes;
        std::string entityText;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const Entity e = entities[i];
            // By name, so the order is the same whatever order the types were registered in.
            std::vector<std::pair<std::string, ComponentId>> components;
            for (const ComponentId id : world.ComponentsOf(e))
                if (world.Describe(id).saved) components.emplace_back(world.Describe(id).name, id);
            std::ranges::sort(components);

            std::string componentText, disabled;
            for (const auto& [name, id] : components) {
                const auto text = component(e, id);
                if (!text) continue;
                usedTypes.insert(name);
                componentText += std::format("{}\n        {}: {}", componentText.empty() ? "" : ",", json::Quote(name), *text);
                if (!world.IsEnabled(e, id)) disabled += std::format("{}{}", disabled.empty() ? "" : ", ", json::Quote(name));
            }
            entityText += std::format("{}\n    {{\n      \"id\": {}", i == 0 ? "" : ",", i);
            entityText += std::format(",\n      \"name\": {}", json::Quote(world.NameOf(e)));
            // Where it is, relative to what it is under: said only where it is not where everything starts.
            const auto vector = [&](const char* key, const Vec3& value, const Vec3& usual) {
                if (value != usual) entityText += std::format(",\n      \"{}\": [{}, {}, {}]", key, value.x, value.y, value.z);
            };
            vector("position", world.PositionOf(e), {});
            vector("rotation", world.RotationOf(e), {});
            vector("scale", world.ScaleOf(e), { 1.f, 1.f, 1.f });
            // What is under it, in the order it is under it.
            if (const auto& children = world.ChildrenOf(e); !children.empty()) {
                std::string list;
                for (const Entity child : children) list += std::format("{}{}", list.empty() ? "" : ", ", context.idOf(child));
                entityText += std::format(",\n      \"children\": [{}]", list);
            }
            if (!disabled.empty()) entityText += std::format(",\n      \"disabled\": [{}]", disabled);
            entityText += std::format(",\n      \"components\": {{{}{}}}\n    }}", componentText, componentText.empty() ? "" : "\n      ");
        }

        std::string typeText;
        for (const auto& name : usedTypes) {
            const ComponentDescription& type = world.Describe(world.Find(name));
            std::string fields;
            for (const auto& field : type.fields)
                fields += std::format("{}\n        {{ \"name\": {}, \"type\": {}, \"offset\": {}, \"size\": {} }}", fields.empty() ? "" : ",",
                                      json::Quote(field.name), json::Quote(field.type), field.offset, field.size);
            // The kinds it is one of: a world that loads it without whoever declared it still finds it by them.
            std::string kinds;
            for (const auto& kind : type.kinds) kinds += std::format("{}{}", kinds.empty() ? "" : ", ", json::Quote(kind));
            typeText += std::format("{}\n    {{ \"name\": {}, \"size\": {}, \"alignment\": {}, \"plain\": {}{}, \"fields\": [{}{}] }}", typeText.empty() ? "" : ",",
                                    json::Quote(name), type.size, type.alignment, Rebuildable(type) ? "true" : "false",
                                    kinds.empty() ? std::string() : std::format(", \"kinds\": [{}]", kinds), fields, fields.empty() ? "" : "\n      ");
        }

        saved.json = std::format("{{\n  \"kecs\": 1,\n  \"types\": [{}{}],\n  \"entities\": [{}{}]\n}}\n", typeText, typeText.empty() ? "" : "\n  ",
                                 entityText, entityText.empty() ? "" : "\n  ");
        saved.problems.assign(problems.begin(), problems.end());
        return saved;
    }

    // ---- loading -------------------------------------------------------------------------------------------------

    Loaded Load(World& world, const std::string_view text, const Codecs& codecs)
    {
        Loaded loaded;
        json::Value root;
        if (!json::Parse(text, root, loaded.error)) return loaded;
        const json::Value* version = root.Find("kecs");
        const json::Value* types = root.Find("types");
        const json::Value* entities = root.Find("entities");
        if (root.kind != json::Value::Kind::eObject || !version || !types || !entities || types->kind != json::Value::Kind::eArray
            || entities->kind != json::Value::Kind::eArray) {
            loaded.error = "not a saved world: it needs \"kecs\", \"types\" and \"entities\"";
            return loaded;
        }
        if (version->number != 1.0) {
            loaded.error = std::format("a saved world of version {}, which this does not read", version->number);
            return loaded;
        }

        std::set<std::string> problems;
        const auto number = [](const json::Value* value) { return value && value->kind == json::Value::Kind::eNumber ? static_cast<std::size_t>(value->number) : std::size_t { 0 }; };

        // The types, as the file has them: registered where the world has none of that name, and otherwise only what
        // each field was, to tell whether the world's field of that name is the same.
        struct SavedType { ComponentId id; std::map<std::string, std::pair<std::string, std::size_t>, std::less<>> fields; };
        std::map<std::string, SavedType, std::less<>> savedTypes;
        for (const auto& type : types->items) {
            const json::Value* name = type.Find("name");
            if (!name || name->kind != json::Value::Kind::eString) continue;
            ComponentDescription description;
            description.name = name->string;
            description.size = number(type.Find("size"));
            description.alignment = std::max<std::size_t>(number(type.Find("alignment")), 1);
            if (const json::Value* kinds = type.Find("kinds"))
                for (const auto& kind : kinds->items)
                    if (kind.kind == json::Value::Kind::eString) description.kinds.push_back(kind.string);
            SavedType entry {};
            if (const json::Value* fields = type.Find("fields"))
                for (const auto& field : fields->items) {
                    const json::Value* fieldName = field.Find("name");
                    const json::Value* fieldType = field.Find("type");
                    if (!fieldName || !fieldType) continue;
                    FieldDescription f;
                    f.name = fieldName->string;
                    f.type = fieldType->string;
                    f.offset = number(field.Find("offset"));
                    f.size = number(field.Find("size"));
                    entry.fields.emplace(f.name, std::pair { f.type, f.size });
                    description.fields.push_back(std::move(f));
                }
            entry.id = world.Find(description.name);
            if (entry.id == NoComponent) {
                // Made from what the file says of it — which is enough only for plain data. One that owns something
                // has to be registered by whoever knows how it is made.
                const json::Value* plain = type.Find("plain");
                if (!plain || !plain->boolean) {
                    problems.insert(std::format("'{}' is not plain data, and the world has no such type: left out", description.name));
                    continue;
                }
                entry.id = world.Register(std::move(description));
            }
            savedTypes.emplace(name->string, std::move(entry));
        }

        // Every entity first, so one can refer to another whichever comes first in the file.
        std::map<std::int64_t, Entity> made;
        for (const auto& entity : entities->items) {
            const Entity e = world.Create();
            loaded.entities.push_back(e);
            if (const json::Value* id = entity.Find("id"); id && id->kind == json::Value::Kind::eNumber) made.emplace(static_cast<std::int64_t>(id->number), e);
        }
        const LoadContext context { [&](const std::int64_t id) { const auto it = made.find(id); return it == made.end() ? NullEntity : it->second; } };

        const auto bytes = [](const json::Value& value) -> const json::Value* {
            if (value.kind != json::Value::Kind::eObject) return nullptr;
            const json::Value* hex = value.Find(BytesKey);
            return hex && hex->kind == json::Value::Kind::eString ? hex : nullptr;
        };

        for (std::size_t i = 0; i < entities->items.size(); ++i) {
            const json::Value& entity = entities->items[i];
            const Entity e = loaded.entities[i];
            if (const json::Value* components = entity.Find("components")) {
                for (const auto& [name, value] : components->members) {
                    const auto saved = savedTypes.find(name);
                    if (saved == savedTypes.end()) {
                        if (!problems.contains(std::format("'{}' is not plain data, and the world has no such type: left out", name)))
                            problems.insert(std::format("'{}' is not among the file's types: left out", name));
                        continue;
                    }
                    ComponentId id = saved->second.id;
                    // Saved as what is a kind now: loaded as the one of it that what it holds says.
                    const bool became = world.Describe(id).abstract;
                    if (became) {
                        const std::string which = codecs.BecomesOf(name, value.raw);
                        const ComponentId one = which.empty() ? NoComponent : world.Find(which);
                        if (one == NoComponent || world.Describe(one).abstract || !world.IsOf(one, id)) {
                            problems.insert(std::format("'{}' is a kind now, and nothing says which of its components one saved is: left out", name));
                            continue;
                        }
                        id = one;
                    }
                    if (!world.Add(e, id)) {
                        problems.insert(std::format("'{}' could not be given: left out", name));
                        continue;
                    }
                    const ComponentDescription& type = world.Describe(id);
                    if (type.IsTag()) continue;
                    auto* target = static_cast<std::byte*>(world.Mutable(e, id));
                    if (!target) continue;

                    if (const json::Value* whole = bytes(value)) {
                        if (!Plain(type) || !type.fields.empty() || !FromHex(whole->string, target, type.size))
                            problems.insert(std::format("'{}' was saved as bytes, which it no longer is: left at its default", name));
                        continue;
                    }
                    for (const auto& [fieldName, fieldValue] : value.members) {
                        const FieldDescription* field = type.FindField(fieldName);
                        if (became && !field) continue;     // what told the kind's components apart: said by which it is now
                        const auto was = saved->second.fields.find(fieldName);
                        // A field the file's type does not list — newer than it, written in by hand or by a script — is read
                        // as the world's type has it; one it lists otherwise than the world has it is left at its default.
                        if (!field || (was != saved->second.fields.end() && (!codecs.ReadsAs(field->type, was->second.first) || was->second.second != field->size))) {
                            problems.insert(std::format("'{}.{}' is not what it was when saved: left at its default", name, fieldName));
                            continue;
                        }
                        bool read = false;
                        if (const json::Value* raw = bytes(fieldValue)) {
                            read = !field->assign && FromHex(raw->string, target + field->offset, field->size);
                        } else if (const Codec* codec = codecs.Find(field->type); codec && codec->read) {
                            read = codec->read(target + field->offset, fieldValue.raw, context);
                        }
                        if (!read) problems.insert(std::format("'{}.{}' could not be read as a {}: left at its default", name, fieldName, field->type));
                    }
                }
            }
            if (const json::Value* disabled = entity.Find("disabled"))
                for (const auto& name : disabled->items)
                    if (const auto saved = savedTypes.find(name.string); saved != savedTypes.end()) world.SetEnabled(e, saved->second.id, false);
        }

        // The hierarchy last, each entity's children in the order they were; then where each is, which is relative
        // to what it is now under.
        for (std::size_t i = 0; i < entities->items.size(); ++i)
            if (const json::Value* children = entities->items[i].Find("children"))
                for (const auto& child : children->items)
                    if (child.kind == json::Value::Kind::eNumber)
                        world.SetParent(context.entityOf(static_cast<std::int64_t>(child.number)), loaded.entities[i], -1, false);
        for (std::size_t i = 0; i < entities->items.size(); ++i) {
            const json::Value& entity = entities->items[i];
            if (const json::Value* name = entity.Find("name"); name && name->kind == json::Value::Kind::eString) world.Rename(loaded.entities[i], name->string);
            const auto vector = [&](const char* key, Vec3& into) {
                const json::Value* value = entity.Find(key);
                if (!value || value->items.size() != 3) return false;
                into = { static_cast<float>(value->items[0].number), static_cast<float>(value->items[1].number), static_cast<float>(value->items[2].number) };
                return true;
            };
            Vec3 value;
            if (vector("position", value)) world.SetPosition(loaded.entities[i], value);
            if (vector("rotation", value)) world.SetRotation(loaded.entities[i], value);
            if (vector("scale", value)) world.SetScale(loaded.entities[i], value);
        }

        loaded.problems.assign(problems.begin(), problems.end());
        loaded.ok = true;
        return loaded;
    }
}
