#include <kscript/scripts.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace kscript
{
    namespace {
        std::uint64_t KeyOf(const kecs::Entity entity) { return (std::uint64_t { entity.index } << 32) | entity.generation; }

        // What marks a component type as a script's: nothing else constructs one with this.
        void ConstructScript(void* at, const kecs::ComponentDescription& type) { std::memset(at, 0, type.size); }
    }

    Scripts::Scripts(kecs::World& world) : _world(world) {}

    Scripts::~Scripts() { *_alive = false; }

    kecs::ComponentId Scripts::Register(ScriptType type, std::string* error, kecs::Migration* migration)
    {
        kecs::ComponentDescription description;
        description.name = type.name;
        // A script with no fields is still something to call on, entity by entity: a byte, so it is no tag.
        description.size = std::max<std::size_t>(type.size, 1);
        description.alignment = std::max<std::size_t>(type.alignment, 1);
        description.construct = &ConstructScript;
        description.fields = type.fields;

        const auto known = std::ranges::find_if(_types, [&](const auto& t) { return t->description.name == type.name; });
        Type* entry = known == _types.end() ? nullptr : known->get();

        kecs::ComponentId id = _world.Find(type.name);
        if (id == kecs::NoComponent) {
            id = _world.Register(std::move(description));
        } else {
            const kecs::ComponentDescription& now = _world.Describe(id);
            // One a file brought in before its script was loaded is plain data of the same name: a script's all the same.
            const bool ours = now.construct == &ConstructScript || (!now.construct && !now.copy && !now.relocate && !now.destroy && !now.IsTag());
            if (!ours) {
                if (error) *error = std::format("there is a component named '{}' already, and it is not a script's", type.name);
                return kecs::NoComponent;
            }
            const bool same = now.size == description.size && now.construct == &ConstructScript
                && std::ranges::equal(now.fields, description.fields, [](const auto& a, const auto& b) { return a.SameAs(b) && a.offset == b.offset; });
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
        entry->live = true;
        return id;
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

    void Scripts::Gone(Type& type, const kecs::Entity entity)
    {
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
                if (callbacks.created) callbacks.created(callbacks.user, entity, bytes);
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

    template <typename F>
    void Scripts::Each(F&& call)
    {
        // By index: something called may register a type.
        for (std::size_t i = 0; i < _types.size(); ++i) {
            Type& type = *_types[i];
            if (!type.live) continue;
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
        Each([step](const Callbacks& c, const kecs::Entity entity, void* data) { if (c.fixedUpdate) c.fixedUpdate(c.user, entity, data, step); });
    }
    void Scripts::Update(const float delta)
    {
        Each([delta](const Callbacks& c, const kecs::Entity entity, void* data) { if (c.update) c.update(c.user, entity, data, delta); });
    }
    void Scripts::LateUpdate(const float delta)
    {
        Each([delta](const Callbacks& c, const kecs::Entity entity, void* data) { if (c.lateUpdate) c.lateUpdate(c.user, entity, data, delta); });
    }
    void Scripts::Prepare()
    {
        Each([](const Callbacks& c, const kecs::Entity entity, void* data) { if (c.prepare) c.prepare(c.user, entity, data); });
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
