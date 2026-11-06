#include <kscript/scripts.h>

#include <cmath>

#include <algorithm>
#include <cstring>
#include <format>
#include <ranges>
#include <utility>

namespace kscript
{
    namespace {
        std::uint64_t KeyOf(const kecs::Entity entity) { return (std::uint64_t { entity.index } << 32) | entity.generation; }

        /** Whether what a script type's callbacks are does piece @p piece (Scripts::Pieces). */
        bool Does(const Callbacks& c, const std::string_view piece)
        {
            if (piece == "PerFrame") return c.update || c.lateUpdate;
            if (piece == "FixedStep") return c.fixedUpdate;
            if (piece == "BeforeDraw") return c.prepare;
            return false;
        }

        // What marks a component type as a script's: nothing else constructs one with this. It starts as its source
        // says (the description's context: its bytes), or as zeros.
        void ConstructScript(void* at, const kecs::ComponentDescription& type)
        {
            std::memset(at, 0, type.size);
            if (const auto* initial = static_cast<const std::vector<std::byte>*>(type.context.get()))
                std::memcpy(at, initial->data(), std::min(initial->size(), type.size));
        }

        /** What a type registered again without a start of its own starts as: what each field of the same name and type did before. */
        std::vector<std::byte> Carried(const kecs::ComponentDescription& before, const ScriptType& type)
        {
            const auto* was = static_cast<const std::vector<std::byte>*>(before.context.get());
            if (!was || was->empty()) return {};
            std::vector<std::byte> initial(std::max<std::size_t>(type.size, 1), std::byte { 0 });
            bool any = false;
            for (const auto& field : type.fields)
                for (const auto& old : before.fields)
                    if (old.SameAs(field) && old.offset + old.size <= was->size() && field.offset + field.size <= initial.size()) {
                        std::memcpy(initial.data() + field.offset, was->data() + old.offset, field.size);
                        any = true;
                    }
            return any ? initial : std::vector<std::byte> {};
        }
    }

    Scripts::Scripts(kecs::World& world) : _world(world) {}

    Scripts::~Scripts() { *_alive = false; }

    kecs::ComponentId Scripts::Add(ScriptType type, const bool live, std::string* error, kecs::Migration* migration)
    {
        kecs::ComponentDescription description;
        description.name = type.name;
        // A script with no fields is still something to call on, entity by entity: a byte, so it is no tag.
        description.size = std::max<std::size_t>(type.size, 1);
        description.alignment = std::max<std::size_t>(type.alignment, 1);
        description.construct = &ConstructScript;
        description.kinds = type.kinds;
        // A kind is had by nobody itself: nothing makes one, and it is only its fields' places.
        if (type.abstract) {
            if (live) {
                if (error) *error = std::format("{} is abstract: it is a kind, which nothing runs", type.name);
                return kecs::NoComponent;
            }
            description.abstract = true;
            description.construct = nullptr;
            description.size = type.size;
        }
        // What its source says the fields are kept within, when what registers it does not say: a host gives the shape.
        if (const kecs::ComponentId existing = _world.Find(type.name); existing != kecs::NoComponent)
            for (auto& field : type.fields)
                if (const kecs::FieldDescription* was = _world.Describe(existing).FindField(field.name);
                    was && was->SameAs(field) && !std::isfinite(field.min) && !std::isfinite(field.max) && !field.logarithmic) {
                    field.min = was->min;
                    field.max = was->max;
                    field.logarithmic = was->logarithmic;
                }
        description.fields = type.fields;
        if (const kecs::ComponentId existing = _world.Find(type.name); existing != kecs::NoComponent && type.initial.empty())
            type.initial = Carried(_world.Describe(existing), type);
        // An entity a field holds starts as none: all zeros would be the first entity there is.
        for (const auto& field : type.fields) {
            if (field.type != "entity" || field.size < sizeof(kecs::Entity)) continue;
            if (type.initial.empty()) type.initial.assign(description.size, std::byte { 0 });
            if (type.initial.size() < field.offset + sizeof(kecs::Entity)) continue;
            std::byte* at = type.initial.data() + field.offset;
            if (std::all_of(at, at + sizeof(kecs::Entity), [](const std::byte b) { return b == std::byte { 0 }; }))
                std::memcpy(at, &kecs::NullEntity, sizeof(kecs::Entity));
        }
        if (type.abstract) type.initial.clear();
        if (!type.initial.empty()) {
            type.initial.resize(description.size, std::byte { 0 });
            description.context = std::make_shared<const std::vector<std::byte>>(type.initial);
        }

        const auto known = std::ranges::find_if(_types, [&](const auto& t) { return t->description.name == type.name; });
        Type* entry = known == _types.end() ? nullptr : known->get();

        kecs::ComponentId id = _world.Find(type.name);
        if (id == kecs::NoComponent) {
            id = _world.Register(std::move(description));
        } else {
            const kecs::ComponentDescription& now = _world.Describe(id);
            // One a file brought in before its script was loaded is plain data of the same name: a script's all the same.
            const bool ours = now.construct == &ConstructScript || now.abstract || (!now.construct && !now.copy && !now.relocate && !now.destroy && !now.IsTag());
            if (!ours) {
                if (error) *error = std::format("there is a component named '{}' already, and it is not a script's", type.name);
                return kecs::NoComponent;
            }
            // A component entities have cannot become a kind under them: what each has would be had by nobody.
            if (description.abstract && !now.abstract) {
                kecs::QueryDescription having;
                having.all = { id };
                having.includeDisabled = true;
                if (_world.Count(having) > 0) {
                    if (error) *error = std::format("{} is abstract now, and entities have it: give each one of what derives from it first", type.name);
                    return kecs::NoComponent;
                }
            }
            const auto* before = static_cast<const std::vector<std::byte>*>(now.context.get());
            const auto* after = static_cast<const std::vector<std::byte>*>(description.context.get());
            const bool same = now.size == description.size && now.construct == description.construct && now.abstract == description.abstract && now.kinds == description.kinds
                && std::ranges::equal(now.fields, description.fields, [](const auto& a, const auto& b) { return a.SameAs(b) && a.offset == b.offset; })
                && (before == after || (before && after && *before == *after));
            if (!same) {
                try {
                    const kecs::Migration moved = _world.Reshape(id, std::move(description));
                    if (migration) *migration = moved;
                } catch (const std::exception& e) {
                    if (error) *error = e.what();
                    return kecs::NoComponent;
                }
            }
        }

        if (!entry) {
            entry = _types.emplace_back(std::make_unique<Type>()).get();
            entry->id = id;
            // Told before an entity loses it — taken away, or destroyed with it — for as long as this is here.
            _world.OnRemove(id, [this, entry, alive = std::weak_ptr<bool>(_alive)](kecs::World&, const kecs::Entity entity) {
                if (const auto here = alive.lock(); here && *here) Gone(*entry, entity);
            });
        }
        entry->description = std::move(type);
        entry->live = live;
        // What it does may be other than it was (a script edited): each entity running it is looked at again.
        for (const std::uint64_t key : entry->states | std::views::keys) {
            const kecs::Entity entity { static_cast<std::uint32_t>(key >> 32), static_cast<std::uint32_t>(key) };
            GiveTags(*entry, entity);
            _untagged.push_back(entity);
        }
        return id;
    }

    kecs::ComponentId Scripts::Register(ScriptType type, std::string* error, kecs::Migration* migration) { return Add(std::move(type), true, error, migration); }

    kecs::ComponentId Scripts::Declare(ScriptType type, std::string* error, kecs::Migration* migration)
    {
        // Declared over a script that runs: its functions stay, with the data's new shape.
        if (const auto known = std::ranges::find_if(_types, [&](const auto& t) { return t->description.name == type.name && t->live; }); known != _types.end()) {
            type.callbacks = (*known)->description.callbacks;
            return Add(std::move(type), true, error, migration);
        }
        type.callbacks = {};
        return Add(std::move(type), false, error, migration);
    }

    void Scripts::Unregister(const std::string_view name)
    {
        for (auto& type : _types)
            if (type->description.name == name) {
                type->live = false;
                type->description.callbacks = {};
            }
    }

    bool Scripts::Registered(const std::string_view name) const
    {
        return std::ranges::any_of(_types, [&](const auto& type) { return type->live && type->description.name == name; });
    }

    std::vector<std::string> Scripts::Types() const
    {
        std::vector<std::string> names;
        for (const auto& type : _types) if (type->live) names.push_back(type->description.name);
        return names;
    }

    kecs::ComponentId Scripts::PieceTag(const std::string_view piece)
    {
        if (const kecs::ComponentId id = _world.Find(piece); id != kecs::NoComponent) return id;
        kecs::ComponentDescription tag;
        tag.name = std::string(piece);
        tag.saved = false;      // worked out again from the scripts: a file would only repeat it
        return _world.Register(std::move(tag));
    }

    void Scripts::GiveTags(const Type& type, const kecs::Entity entity)
    {
        if (!type.live) return;
        for (const std::string_view piece : Pieces)
            if (Does(type.description.callbacks, piece))
                if (const kecs::ComponentId tag = PieceTag(piece); _world.Alive(entity) && !_world.Has(entity, tag)) _world.Add(entity, tag);
    }

    void Scripts::TakeTags()
    {
        for (const kecs::Entity entity : std::exchange(_untagged, {})) {
            if (!_world.Alive(entity)) continue;
            for (const std::string_view piece : Pieces) {
                const kecs::ComponentId tag = _world.Find(piece);
                if (tag == kecs::NoComponent || !_world.Has(entity, tag)) continue;
                const bool still = std::ranges::any_of(_types, [&](const auto& type) {
                    return type->live && Does(type->description.callbacks, piece) && _world.Has(entity, type->id);
                });
                if (!still) _world.Remove(entity, tag);
            }
        }
    }

    void Scripts::Gone(Type& type, const kecs::Entity entity)
    {
        _untagged.push_back(entity);    // looked at once the world is done changing: not while it takes a component away
        const auto state = type.states.find(KeyOf(entity));
        if (state == type.states.end()) return;
        const bool wasEnabled = state->second.enabled;
        type.states.erase(state);
        if (!type.live) return;
        void* data = const_cast<void*>(std::as_const(_world).Get(entity, type.id));
        const Callbacks& callbacks = type.description.callbacks;
        if (wasEnabled && callbacks.disabled) callbacks.disabled(callbacks.user, entity, data);
        if (callbacks.destroyed) callbacks.destroyed(callbacks.user, entity, data);
    }

    std::vector<kecs::Entity> Scripts::Sync(Type& type)
    {
        // Who has it, and whether it is switched on: gathered first, so what is called may change the world.
        struct Seen { kecs::Entity entity; bool enabled; };
        std::vector<Seen> seen;
        kecs::QueryDescription query;
        query.all = { type.id };
        query.includeDisabled = true;
        _world.Each(query, [&](const kecs::ChunkView& view) {
            for (std::size_t row = 0; row < view.Count(); ++row) {
                const kecs::Entity entity = view.EntityAt(row);
                seen.push_back({ entity, _world.IsEnabled(entity, type.id) });
            }
        });
        // In the order they were made, whichever tags each has been given — and so wherever the world keeps it.
        std::ranges::sort(seen, {}, [](const Seen& one) { return one.entity.index; });

        const Callbacks& callbacks = type.description.callbacks;
        const auto data = [&](const kecs::Entity entity) -> void* {
            return _world.Alive(entity) ? const_cast<void*>(std::as_const(_world).Get(entity, type.id)) : nullptr;
        };
        std::vector<kecs::Entity> running;
        for (const auto& [entity, enabled] : seen) {
            const std::uint64_t key = KeyOf(entity);
            if (!type.states.contains(key)) {
                void* bytes = data(entity);
                if (!bytes) continue;   // gone already, by something called before it
                type.states.emplace(key, State {});
                GiveTags(type, entity);
                bytes = data(entity);   // given a tag, its data is somewhere else
                if (callbacks.created && bytes) callbacks.created(callbacks.user, entity, bytes);
            }
            // Anything called may have taken it away: looked up again each time.
            auto state = type.states.find(key);
            if (state == type.states.end()) continue;
            if (enabled != state->second.enabled) {
                state->second.enabled = enabled;
                if (void* bytes = data(entity)) {
                    if (enabled && callbacks.enabled) callbacks.enabled(callbacks.user, entity, bytes);
                    if (!enabled && callbacks.disabled) callbacks.disabled(callbacks.user, entity, bytes);
                }
                state = type.states.find(key);
                if (state == type.states.end()) continue;
            }
            if (!enabled) continue;
            if (!state->second.started) {
                state->second.started = true;
                if (void* bytes = data(entity); bytes && callbacks.started) callbacks.started(callbacks.user, entity, bytes);
                if (!type.states.contains(key)) continue;
            }
            running.push_back(entity);
        }
        return running;
    }

    template <typename F, typename Has>
    void Scripts::Each(F&& call, Has&& has)
    {
        TakeTags();
        // By index: something called may register a type.
        for (std::size_t i = 0; i < _types.size(); ++i) {
            Type& type = *_types[i];
            if (!type.live || !has(type.description.callbacks)) continue;     // nothing of it to call: nobody is looked at
            for (const kecs::Entity entity : Sync(type)) {
                if (!type.live) break;
                const auto state = type.states.find(KeyOf(entity));
                if (state == type.states.end() || !state->second.enabled) continue;     // gone, or switched off, since
                if (!_world.Alive(entity) || !_world.IsEnabled(entity, type.id)) continue;
                void* bytes = const_cast<void*>(std::as_const(_world).Get(entity, type.id));
                if (bytes) call(type.description.callbacks, entity, bytes);
            }
        }
    }

    void Scripts::FixedUpdate(const float step)
    {
        Each([step](const Callbacks& c, const kecs::Entity entity, void* data) { c.fixedUpdate(c.user, entity, data, step); },
             [](const Callbacks& c) { return c.fixedUpdate != nullptr; });
    }
    void Scripts::Update(const float delta)
    {
        // Every type with anything to be told, whether it updates or not: here is where each is told it was made, switched
        // and started. One with nothing at all — a script that is only data — is not looked at.
        Each([delta](const Callbacks& c, const kecs::Entity entity, void* data) { if (c.update) c.update(c.user, entity, data, delta); },
             [](const Callbacks& c) { return c.created || c.enabled || c.started || c.fixedUpdate || c.update || c.lateUpdate || c.prepare || c.disabled || c.destroyed; });
    }
    void Scripts::LateUpdate(const float delta)
    {
        Each([delta](const Callbacks& c, const kecs::Entity entity, void* data) { c.lateUpdate(c.user, entity, data, delta); },
             [](const Callbacks& c) { return c.lateUpdate != nullptr; });
    }
    // Not a step of the game: it is called whether or not the scene plays (an editor's scene is drawn while it is
    // edited), so nothing is created, enabled or started here — that is the updates' to do, once it plays.
    void Scripts::Prepare()
    {
        TakeTags();
        for (std::size_t i = 0; i < _types.size(); ++i) {
            Type& type = *_types[i];
            if (!type.live || !type.description.callbacks.prepare) continue;
            std::vector<kecs::Entity> entities;
            kecs::QueryDescription query;
            query.all = { type.id };
            _world.Each(query, [&](const kecs::ChunkView& view) {
                for (std::size_t row = 0; row < view.Count(); ++row) entities.push_back(view.EntityAt(row));
            });
            for (const kecs::Entity entity : entities) {
                if (!type.live) break;
                if (!_world.Alive(entity) || !_world.IsEnabled(entity, type.id)) continue;     // gone, or switched off, since
                GiveTags(type, entity);     // drawn in an editor that does not play: made, and never updated
                const Callbacks& c = type.description.callbacks;
                if (void* bytes = const_cast<void*>(std::as_const(_world).Get(entity, type.id))) c.prepare(c.user, entity, bytes);
            }
        }
    }

    void Scripts::Shutdown()
    {
        for (std::size_t i = 0; i < _types.size(); ++i) {
            Type& type = *_types[i];
            std::vector<kecs::Entity> entities;
            kecs::QueryDescription query;
            query.all = { type.id };
            query.includeDisabled = true;
            _world.Each(query, [&](const kecs::ChunkView& view) {
                for (std::size_t row = 0; row < view.Count(); ++row) entities.push_back(view.EntityAt(row));
            });
            for (const kecs::Entity entity : entities) Gone(type, entity);
        }
    }
}
