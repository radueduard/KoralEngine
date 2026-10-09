#pragma once

/**
 * @file world.h
 * @brief The world: entities, their components, and the queries that run over them.
 *
 * Entities with the same set of components are stored together, a column a component (an *archetype*), so a
 * query walks plain arrays. Giving an entity a component, or taking one away, moves it to another archetype;
 * switching a component off for one entity does not — that is a bit beside it, which queries respect.
 *
 * @code
 * kecs::World world;
 * const kecs::Entity ship = world.Create(Position { 0, 0 }, Velocity { 1, 0 });
 * world.Add<Player>(ship);                                   // a tag: a component with nothing in it
 *
 * world.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; });
 * world.Query<const Position>().With<Player>().Each([](kecs::Entity e, const Position& p) { ... });
 *
 * world.SetEnabled<Velocity>(ship, false);                   // still has it; queries pass it by
 * @endcode
 *
 * While a query runs, the world's shape must not change under it: Create, Destroy, Add, Remove and SetParent
 * throw. Queue them in a Commands, and Apply it afterwards.
 */

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "component.h"
#include "transform.h"

namespace kecs
{
    class World;

    namespace detail
    {
        /** One component's values for the entities of an archetype, with when each was added and last written. */
        struct Column {
            const ComponentDescription* type = nullptr;
            std::byte* data = nullptr;
            std::vector<Tick> added, changed;
            std::vector<std::uint64_t> disabled;    ///< A bit a row: set where the component is switched off.
            std::size_t disabledCount = 0;

            [[nodiscard]] void* At(const std::size_t row) const { return data + row * type->size; }
            [[nodiscard]] bool Disabled(const std::size_t row) const { return disabledCount != 0 && ((disabled[row >> 6] >> (row & 63)) & 1u) != 0; }
        };

        /** Every entity that has exactly these components. */
        struct Archetype {
            std::vector<ComponentId> ids;           ///< Sorted.
            std::vector<Column> columns;            ///< One an id, in the same order.
            std::vector<Entity> entities;
            std::size_t capacity = 0;
            std::unordered_map<ComponentId, Archetype*> with, without;   ///< Where an entity goes on gaining or losing one.

            [[nodiscard]] int ColumnOf(ComponentId id) const;
            [[nodiscard]] bool Has(const ComponentId id) const { return ColumnOf(id) >= 0; }
        };

        template <typename T> struct Unwrapped { using type = std::remove_const_t<T>; };
    }

    /**
     * @brief A component in a query's callback that is marked changed only when it is written: `Mut<Health> h`,
     *        read with `*h` or `h->`, written with `h.Write()`. A plain `Health&` is marked changed for every
     *        entity the query visits, written or not.
     */
    template <typename T>
    class Mut {
    public:
        Mut(T* value, Tick* stamp, const Tick now) : _value(value), _stamp(stamp), _now(now) {}
        [[nodiscard]] const T& operator*() const { return *_value; }
        [[nodiscard]] const T* operator->() const { return _value; }
        /** @brief The component, to write: it counts as changed from here. */
        [[nodiscard]] T& Write() const { *_stamp = _now; return *_value; }

    private:
        T* _value;
        Tick* _stamp;
        Tick _now;
    };

    /**
     * @brief A component a query takes where an entity has it, and does without where it has not:
     *        `Query<Position, Maybe<const Velocity>>`, whose callback gets a `const Velocity*` — null for an
     *        entity with no velocity, or with it switched off. `Maybe<Velocity>` gives one to write, marked changed.
     */
    template <typename T>
    struct Maybe {};

    namespace detail
    {
        template <typename T> struct Unwrapped<Mut<T>> { using type = T; };
        template <typename T> struct Unwrapped<Maybe<T>> { using type = std::remove_const_t<T>; };
        template <typename T> inline constexpr bool IsMaybe = false;
        template <typename T> inline constexpr bool IsMaybe<Maybe<T>> = true;
        template <typename T> struct MaybeOf { using type = T; };
        template <typename T> struct MaybeOf<Maybe<T>> { using type = T; };
        template <typename T> using Component = typename Unwrapped<T>::type;
        template <typename T> inline constexpr bool IsMut = false;
        template <typename T> inline constexpr bool IsMut<Mut<T>> = true;

        /** A query callback's argument for @p T, as it asked for it: read, written (and so stamped), or written if it says so. */
        template <typename T>
        decltype(auto) Argument(Column* optional, const std::size_t row, const Tick now)
        {
            using C = Component<T>;
            if constexpr (IsMaybe<T>) {
                using Asked = typename MaybeOf<T>::type;   // const or not, as it was asked for
                if (!optional || optional->Disabled(row)) return static_cast<Asked*>(nullptr);
                if constexpr (!std::is_const_v<Asked>) optional->changed[row] = now;
                return static_cast<Asked*>(static_cast<C*>(optional->At(row)));
            } else if constexpr (IsMut<T>) {
                return Mut<C>(static_cast<C*>(optional->At(row)), &optional->changed[row], now);
            } else if constexpr (std::is_const_v<T>) {
                return static_cast<const C&>(*static_cast<C*>(optional->At(row)));
            } else {
                optional->changed[row] = now;
                return static_cast<C&>(*static_cast<C*>(optional->At(row)));
            }
        }
        template <typename T> using ArgumentType = decltype(Argument<T>(std::declval<Column*>(), 0, 0));
    }

    /** @brief What became of a component type's fields when it changed shape. @see World::Reshape */
    struct Migration {
        std::vector<std::string> kept;      ///< In both shapes, with the same name, type and size: every entity kept its value.
        std::vector<std::string> reset;     ///< New, or changed in type or size: every entity has the default.
        std::vector<std::string> removed;   ///< In the old shape only: gone.
        std::size_t entities = 0;           ///< How many entities had the component.
    };

    /** @brief What a query asks for, by id: for queries made at run time — a node's, a tool's. */
    struct QueryDescription {
        std::vector<ComponentId> all;       ///< Has every one of these, switched on.
        std::vector<ComponentId> none;      ///< Has none of these (one switched off counts as not had).
        std::vector<ComponentId> changed;   ///< Each of these was added or written after @ref since.
        Tick since = 0;
        bool includeDisabled = false;       ///< Also entities with one of @ref all switched off.
    };

    /** @brief The entities of one archetype that a query matched: a run of rows, with a column a component. */
    class ChunkView {
    public:
        [[nodiscard]] std::size_t Count() const { return _archetype->entities.size(); }
        [[nodiscard]] Entity EntityAt(const std::size_t row) const { return _archetype->entities[row]; }
        /** @brief Whether @p row passes what the archetype alone cannot tell: switched-off components, and changed-since. */
        [[nodiscard]] bool Matches(std::size_t row) const;
        /** @brief Whether every row passes, so Matches need not be asked. */
        [[nodiscard]] bool AllMatch() const { return _allMatch; }
        /** @brief The first value of component @p id, the rest following it; null for a tag, or one it does not have. */
        [[nodiscard]] void* Column(ComponentId id) const;
        /** @brief Marks @p id written for @p row. */
        void MarkChanged(ComponentId id, std::size_t row) const;
        [[nodiscard]] bool Has(const ComponentId id) const { return _archetype->Has(id); }

    private:
        friend class World;
        template <typename...> friend class Query;
        detail::Archetype* _archetype = nullptr;
        const QueryDescription* _query = nullptr;
        Tick _now = 0;
        bool _allMatch = true;
    };

    /**
     * @brief A query over the C++ component types @p Ts: `Query<Position, const Velocity, Mut<Health>>`.
     *
     * Made by World::Query. Narrow it with With, Without and Changed, then run it with Each, whose callback takes
     * the components in order — after the Entity, if it wants that first.
     */
    template <typename... Ts>
    class Query {
    public:
        explicit Query(World& world);

        /** @brief Only entities that also have @p T (a tag, usually), switched on. */
        template <typename T> Query& With();
        Query& With(ComponentId id) { _description.all.push_back(id); return *this; }
        /** @brief Only entities without @p T, or with it switched off. */
        template <typename T> Query& Without();
        Query& Without(ComponentId id) { _description.none.push_back(id); return *this; }
        /** @brief Only entities whose @p T was added or written after @p since. */
        template <typename T> Query& Changed(Tick since);
        /** @brief Also entities with one of the components switched off. */
        Query& IncludeDisabled() { _description.includeDisabled = true; return *this; }

        /** @brief Calls @p f for every entity that matches. */
        template <typename F> void Each(F&& f);
        /** @brief How many entities match. */
        [[nodiscard]] std::size_t Count();

    private:
        World& _world;
        QueryDescription _description;
    };

    /**
     * @brief The world.
     */
    class World {
    public:
        World();
        ~World();
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        // ---- component types ------------------------------------------------------------------------------

        /**
         * @brief Registers a component type and returns its id. A name registered already gives the id it has, when
         *        the size and alignment are the same; a different shape under one name throws.
         */
        ComponentId Register(ComponentDescription description);
        /** @brief Registers a tag: a component with nothing in it. */
        ComponentId RegisterTag(std::string name)
        {
            ComponentDescription tag;
            tag.name = std::move(name);
            return Register(std::move(tag));
        }
        /** @brief The id of the C++ type @p T, registering it on first use under Name<T>. */
        template <typename T> ComponentId Id();
        /** @brief The type so named, or NoComponent. */
        [[nodiscard]] ComponentId Find(std::string_view name) const;
        [[nodiscard]] const ComponentDescription& Describe(ComponentId id) const;
        [[nodiscard]] std::size_t ComponentCount() const { return _types.size(); }
        /**
         * @brief Gives component type @p id a new shape, and every entity that has it a value of that shape.
         *
         * What a shader edited, or a script reloaded, does to the component generated from it. The id stays, so
         * queries and observers of it go on working. Each entity's new value starts as a default one; a field
         * the two shapes share — the same name, type and size — is then carried over (FieldDescription::assign),
         * and the old value is destroyed. A field that is new, or whose type or size changed, is left at its
         * default. Whether the component is switched off for an entity is kept; every one counts as changed.
         *
         * @p newer may carry another name (the type is renamed) or none (it keeps its name). Throws while a query
         * runs, for an unknown id, or for a name another type has.
         */
        Migration Reshape(ComponentId id, ComponentDescription newer);

        // ---- entities -------------------------------------------------------------------------------------

        /** @brief A new entity, with nothing. */
        Entity Create();
        /** @brief A new entity with these components, put where it belongs at once. */
        template <typename... Ts> Entity Create(Ts&&... components);
        /** @brief Destroys @p entity, and every entity under it in the hierarchy. False when it was not alive. */
        bool Destroy(Entity entity);
        [[nodiscard]] bool Alive(Entity entity) const;
        /** @brief The entity alive at place @p index, whichever generation it is; NullEntity when the place is free. */
        [[nodiscard]] Entity EntityAt(std::uint32_t index) const;
        [[nodiscard]] std::size_t EntityCount() const { return _alive; }

        // ---- components, by type --------------------------------------------------------------------------

        /**
         * @brief Gives @p entity a @p T, replacing the one it has. Throws for an entity that is not alive.
         *        @p T is always written out — `Add<Health>(e, { 5 })` — so that a call with an id is never taken for it.
         */
        template <typename T> T& Add(Entity entity, std::type_identity_t<T> value = {});
        /** @brief Takes @p entity 's @p T away. False when it had none. */
        template <typename T> bool Remove(Entity entity);
        /** @brief @p entity 's @p T, to read; null when it has none (or is not alive). Switched off or not. */
        template <typename T> [[nodiscard]] const T* Get(Entity entity) const;
        /** @brief @p entity 's @p T, to write — marked changed; null when it has none. */
        template <typename T> [[nodiscard]] T* Mutable(Entity entity);
        template <typename T> [[nodiscard]] bool Has(Entity entity) const;
        /** @brief Switches @p entity 's @p T off or on, without taking it away. False when it has none. */
        template <typename T> bool SetEnabled(Entity entity, bool enabled);
        /** @brief Whether @p entity has a @p T that is switched on. */
        template <typename T> [[nodiscard]] bool IsEnabled(Entity entity) const;

        // ---- components, by id ----------------------------------------------------------------------------

        /**
         * @brief Gives @p entity component @p id: a copy of @p value, or a default one when that is null. Replaces
         *        the one it has. False for an entity that is not alive, an unknown id, or a value of a type that
         *        cannot be copied.
         */
        bool Add(Entity entity, ComponentId id, const void* value = nullptr);
        bool Remove(Entity entity, ComponentId id);
        /** @brief The component's value, or null: for a tag, or one it does not have. */
        [[nodiscard]] const void* Get(Entity entity, ComponentId id) const;
        /** @brief The same, to write: marked changed. */
        [[nodiscard]] void* Mutable(Entity entity, ComponentId id);
        [[nodiscard]] bool Has(Entity entity, ComponentId id) const;
        bool SetEnabled(Entity entity, ComponentId id, bool enabled);
        [[nodiscard]] bool IsEnabled(Entity entity, ComponentId id) const;
        /** @brief Marks @p entity 's component @p id written now. */
        bool MarkChanged(Entity entity, ComponentId id);
        /** @brief Every component @p entity has, switched on or off. */
        [[nodiscard]] std::vector<ComponentId> ComponentsOf(Entity entity) const;

        // ---- queries --------------------------------------------------------------------------------------

        template <typename... Ts> [[nodiscard]] kecs::Query<Ts...> Query() { return kecs::Query<Ts...>(*this); }
        /** @brief Calls @p visit for every archetype @p query matches that has entities. */
        void Each(const QueryDescription& query, const std::function<void(const ChunkView&)>& visit);
        /** @brief How many entities @p query matches. */
        [[nodiscard]] std::size_t Count(const QueryDescription& query);
        /** @brief Calls @p visit for every entity alive. */
        void EachEntity(const std::function<void(Entity)>& visit) const;
        [[nodiscard]] std::size_t ArchetypeCount() const { return _archetypes.size(); }

        // ---- change ---------------------------------------------------------------------------------------

        /** @brief What a component added or written now is stamped with. */
        [[nodiscard]] Tick CurrentTick() const { return _tick; }
        /**
         * @brief Starts a new tick and returns it. A system calls this as it starts each run, and keeps what it
         *        returned: next run, `Changed<T>(thatTick)` is everything written since — by anything but itself.
         */
        Tick AdvanceTick() { return ++_tick; }

        // ---- names, the hierarchy and transforms --------------------------------------------------------
        //
        // Every entity has a name, a place in a hierarchy and a transform relative to what it is under. They are
        // the world's own and not components: nothing lists, adds or removes them, no query asks for them, and
        // changing them moves no entity to another archetype.

        /** @brief What @p entity is called: "Entity" until it is renamed. Empty for one that is not alive. */
        [[nodiscard]] const std::string& NameOf(Entity entity) const;
        bool Rename(Entity entity, std::string name);
        /** @brief The first entity so called, parents before their children; NullEntity when there is none. */
        [[nodiscard]] Entity FindEntity(std::string_view name) const;

        /**
         * @brief Puts @p child under @p parent, or at the top (NullEntity): at @p index among what is there (last,
         *        when below 0 or past the end). With @p keepWorldTransform it stays where it is in the world, its
         *        own transform changed to make up for the new parent's. False when either is not alive, or when it
         *        would put an entity under itself.
         */
        bool SetParent(Entity child, Entity parent, int index = -1, bool keepWorldTransform = true);
        [[nodiscard]] Entity ParentOf(Entity entity) const;
        /** @brief The entities directly under @p entity, in order. */
        [[nodiscard]] const std::vector<Entity>& ChildrenOf(Entity entity) const;
        /** @brief The entities at the top of the hierarchy, in order. */
        [[nodiscard]] const std::vector<Entity>& Roots() const;
        /** @brief Every entity, parents before their children. */
        [[nodiscard]] std::vector<Entity> All() const;
        /** @brief How far down the hierarchy @p entity is: 0 at the top. */
        [[nodiscard]] std::uint32_t DepthOf(Entity entity) const;
        /** @brief Whether @p ancestor is above @p entity, however far. */
        [[nodiscard]] bool IsAncestor(Entity ancestor, Entity entity) const;

        /** @brief Where @p entity is, relative to what it is under. */
        [[nodiscard]] Vec3 PositionOf(Entity entity) const;
        /** @brief How it is turned, relative to what it is under: Euler angles, in degrees. */
        [[nodiscard]] Vec3 RotationOf(Entity entity) const;
        [[nodiscard]] Vec3 ScaleOf(Entity entity) const;
        bool SetPosition(Entity entity, const Vec3& position);
        bool SetRotation(Entity entity, const Vec3& degrees);
        bool SetScale(Entity entity, const Vec3& scale);
        /** @brief Its own transform as a matrix. */
        [[nodiscard]] Mat4 LocalMatrix(Entity entity) const;
        /** @brief Where it is in the world: the transforms of everything above it, and its own. Worked out when asked for, and kept until something above it or it moves. */
        [[nodiscard]] Mat4 WorldMatrix(Entity entity) const;
        /**
         * @brief Places @p entity at @p matrix in the world by changing its own transform: only the parts asked
         *        for, so that moving a thing does not also round its rotation off.
         */
        bool SetWorldMatrix(Entity entity, const Mat4& matrix, bool position = true, bool rotation = true, bool scale = true);
        /** @brief Goes up whenever an entity is made, destroyed, renamed, moved in the hierarchy or transformed. */
        [[nodiscard]] std::uint64_t SceneVersion() const { return _sceneVersion; }

        // ---- observers ------------------------------------------------------------------------------------

        /** @brief Calls @p observer after an entity is given component @p id (not when one it has is replaced). */
        void OnAdd(ComponentId id, std::function<void(World&, Entity)> observer);
        /** @brief Calls @p observer before an entity loses component @p id: taken away, or destroyed with it. */
        void OnRemove(ComponentId id, std::function<void(World&, Entity)> observer);

        /** @brief Whether a query is running, and the world's shape may not change. */
        [[nodiscard]] bool Iterating() const { return _iterating != 0; }

    private:
        template <typename...> friend class kecs::Query;
        friend class Commands;
        friend class ChunkView;

        struct Record {
            detail::Archetype* archetype = nullptr;     ///< Null for a place that is free, or reserved by a Commands.
            std::uint32_t row = 0;
            std::uint32_t generation = 0;
            bool alive = false;
        };
        struct Cached {
            std::vector<detail::Archetype*> matches;
            std::size_t seen = 0;   ///< How many archetypes had been looked at when it was last brought up to date.
        };

        [[nodiscard]] const Record* RecordOf(Entity entity) const;
        [[nodiscard]] Record* RecordOf(Entity entity);
        void RequireStill(const char* what) const;
        /** An entity's place, alive but in no archetype yet: what Commands hands out while a query runs. */
        Entity Reserve();
        void Place(Entity entity);
        detail::Archetype& ArchetypeOf(std::vector<ComponentId> sortedIds);
        detail::Archetype& ArchetypeWith(detail::Archetype& from, ComponentId id);
        detail::Archetype& ArchetypeWithout(detail::Archetype& from, ComponentId id);
        /** Moves an entity's row to @p to: what both have is carried over, what only @p from has is destroyed. Returns the new row. */
        std::uint32_t Move(Record& record, Entity entity, detail::Archetype& to);
        std::uint32_t Push(detail::Archetype& archetype, Entity entity);
        void Reserve(detail::Archetype& archetype, std::size_t capacity);
        void SwapRemove(detail::Archetype& archetype, std::uint32_t row);
        /** The slot for @p entity 's component @p id, making room: raw memory unless @p existed. Null for a tag. */
        void* Slot(Entity entity, ComponentId id, bool& existed);
        /** After a slot is filled: stamps it and tells observers. */
        void Added(Entity entity, ComponentId id, bool existed);
        void NotifyRemove(Entity entity, ComponentId id);
        [[nodiscard]] const std::vector<detail::Archetype*>& Matching(const QueryDescription& query);
        [[nodiscard]] ChunkView View(detail::Archetype& archetype, const QueryDescription& query) const;
        /** Whether no entity of @p archetype can match: all of them have something the query must not have. */
        [[nodiscard]] bool Excluded(const detail::Archetype& archetype, const QueryDescription& query) const;
        ComponentId IdOf(std::uint32_t typeIndex, ComponentDescription (*describe)());

        std::vector<std::unique_ptr<ComponentDescription>> _types;
        std::unordered_map<std::string, ComponentId> _byName;
        std::vector<ComponentId> _byTypeIndex;
        std::vector<std::unique_ptr<detail::Archetype>> _archetypes;
        std::map<std::vector<ComponentId>, detail::Archetype*> _byIds;
        std::vector<Record> _records;
        std::vector<std::uint32_t> _free;
        std::size_t _alive = 0;
        std::map<std::pair<std::vector<ComponentId>, std::vector<ComponentId>>, Cached> _queries;
        std::vector<std::vector<std::function<void(World&, Entity)>>> _onAdd, _onRemove;
        Tick _tick = 1;
        int _iterating = 0;

        /** What is fundamental to an entity, kept here rather than as components: a place an entity index. */
        struct Node {
            std::string name;
            Entity parent;
            std::vector<Entity> children;
            std::uint32_t depth = 0;
            Vec3 position, rotation, scale { 1.f, 1.f, 1.f };
            Mat4 world;
            bool dirty = true;      ///< world is stale; then so is every descendant's
            std::uint32_t rootSlot = 0;     ///< Where it is in _roots, while it is at the top.
        };
        /** Takes the places of roots that are gone out of _roots: left there as they go, so that going is quick. */
        void CompactRoots() const;
        [[nodiscard]] Node* NodeOf(Entity entity) const;
        void Detach(Entity entity);
        void Attach(Entity entity, Entity parent, int index);
        void SetDepth(Entity entity, std::uint32_t depth);
        void MarkDirty(Entity entity) const;
        [[nodiscard]] const Mat4& WorldOf(Entity entity) const;

        mutable std::vector<Node> _nodes;
        mutable std::vector<Entity> _roots;     ///< In order, with a NullEntity where one has gone since it was last compacted.
        mutable std::size_t _rootHoles = 0;
        std::uint64_t _sceneVersion = 0;
    };

    /**
     * @brief Changes to make to a world later: what a query's callback does instead of changing the world's shape
     *        under the query. Apply makes them, in the order they were asked for.
     *
     * @code
     * kecs::Commands commands(world);
     * world.Query<const Health>().Each([&](kecs::Entity e, const Health& h) { if (h.value <= 0) commands.Destroy(e); });
     * commands.Apply();
     * @endcode
     */
    class Commands {
    public:
        explicit Commands(World& world) : _world(world) {}
        ~Commands();
        Commands(const Commands&) = delete;
        Commands& operator=(const Commands&) = delete;

        /** @brief An entity that exists from Apply on; it can be given components through this before then. */
        Entity Create();
        void Destroy(Entity entity);
        template <typename T> void Add(Entity entity, std::type_identity_t<T> value = {});
        template <typename T> void Remove(Entity entity) { Remove(entity, _world.Id<T>()); }
        /** @brief Adds component @p id as a copy of @p value, or a default one when that is null. */
        void Add(Entity entity, ComponentId id, const void* value = nullptr);
        void Remove(Entity entity, ComponentId id);
        void SetParent(Entity child, Entity parent);
        /** @brief Makes every change asked for, and forgets them. */
        void Apply();
        [[nodiscard]] bool Empty() const { return _commands.empty(); }

    private:
        enum class Kind : std::uint8_t { eCreate, eDestroy, eAdd, eRemove, eSetParent };
        struct Command {
            Kind kind;
            Entity entity;
            ComponentId id = NoComponent;
            void* value = nullptr;      ///< For eAdd: one of the component, owned until applied; null for a default one.
            Entity other;               ///< For eSetParent.
        };
        void* Allocate(ComponentId id);
        World& _world;
        std::vector<Command> _commands;
    };

    // ---- templates ---------------------------------------------------------------------------------------------

    template <typename T>
    ComponentId World::Id()
    {
        using Type = std::remove_cvref_t<T>;
        return IdOf(detail::TypeIndex<Type>(), [] { return ComponentDescription::Of<Type>(std::string(Name<Type>::value)); });
    }

    template <typename T>
    T& World::Add(const Entity entity, std::type_identity_t<T> value)
    {
        const ComponentId id = Id<T>();
        RequireStill("Add");
        if (!Alive(entity)) throw std::logic_error("kecs: Add on an entity that is not alive");
        bool existed = false;
        void* slot = Slot(entity, id, existed);
        if constexpr (std::is_empty_v<T>) {
            static T tag {};   // a tag has no storage: every entity's is this one
            Added(entity, id, existed);
            return tag;
        } else {
            if (existed) *static_cast<T*>(slot) = std::move(value);
            else ::new (slot) T(std::move(value));
            Added(entity, id, existed);
            // An observer may have moved the entity: where the component is now is asked again.
            return *static_cast<T*>(const_cast<void*>(std::as_const(*this).Get(entity, id)));
        }
    }

    template <typename... Ts>
    Entity World::Create(Ts&&... components)
    {
        RequireStill("Create");
        std::vector<ComponentId> ids { Id<Ts>()... };
        std::ranges::sort(ids);
        if (std::ranges::adjacent_find(ids) != ids.end()) throw std::logic_error("kecs: Create was given one component type twice");
        const Entity entity = Reserve();
        detail::Archetype& archetype = ArchetypeOf(std::move(ids));
        Record& record = _records[entity.index];
        record.archetype = &archetype;
        record.row = Push(archetype, entity);
        const auto fill = [&]<typename T>(T&& component) {
            using Type = std::remove_cvref_t<T>;
            if constexpr (!std::is_empty_v<Type>)
                ::new (archetype.columns[static_cast<std::size_t>(archetype.ColumnOf(Id<Type>()))].At(record.row)) Type(std::forward<T>(component));
        };
        (fill(std::forward<Ts>(components)), ...);
        (Added(entity, Id<Ts>(), false), ...);
        return entity;
    }

    template <typename T> bool World::Remove(const Entity entity) { return Remove(entity, Id<T>()); }
    template <typename T> const T* World::Get(const Entity entity) const
    {
        return static_cast<const T*>(Get(entity, const_cast<World*>(this)->Id<T>()));
    }
    template <typename T> T* World::Mutable(const Entity entity) { return static_cast<T*>(Mutable(entity, Id<T>())); }
    template <typename T> bool World::Has(const Entity entity) const { return Has(entity, const_cast<World*>(this)->Id<T>()); }
    template <typename T> bool World::SetEnabled(const Entity entity, const bool enabled) { return SetEnabled(entity, Id<T>(), enabled); }
    template <typename T> bool World::IsEnabled(const Entity entity) const { return IsEnabled(entity, const_cast<World*>(this)->Id<T>()); }

    template <typename... Ts>
    Query<Ts...>::Query(World& world) : _world(world)
    {
        // What it may do without is not asked of an archetype.
        const auto require = [&]<typename T>() { if (!detail::IsMaybe<T>) _description.all.push_back(world.Id<detail::Component<T>>()); };
        (require.template operator()<Ts>(), ...);
    }

    template <typename... Ts> template <typename T>
    Query<Ts...>& Query<Ts...>::With() { return With(_world.Id<T>()); }
    template <typename... Ts> template <typename T>
    Query<Ts...>& Query<Ts...>::Without() { return Without(_world.Id<T>()); }
    template <typename... Ts> template <typename T>
    Query<Ts...>& Query<Ts...>::Changed(const Tick since)
    {
        _description.changed.push_back(_world.Id<T>());
        _description.since = since;
        return *this;
    }

    template <typename... Ts>
    std::size_t Query<Ts...>::Count() { return _world.Count(_description); }

    template <typename... Ts> template <typename F>
    void Query<Ts...>::Each(F&& f)
    {
        static_assert((!std::is_empty_v<detail::Component<Ts>> && ...), "a tag has nothing to hand a callback: ask for it with With<Tag>()");
        const Tick now = _world.CurrentTick();
        const std::array<ComponentId, sizeof...(Ts)> ids { _world.Id<detail::Component<Ts>>()... };
        const auto& archetypes = _world.Matching(_description);
        ++_world._iterating;
        struct Unlock { int& count; ~Unlock() { --count; } } unlock { _world._iterating };
        for (detail::Archetype* archetype : archetypes) {
            const std::size_t count = archetype->entities.size();
            if (count == 0 || _world.Excluded(*archetype, _description)) continue;
            const ChunkView view = _world.View(*archetype, _description);
            std::array<detail::Column*, sizeof...(Ts)> columns {};
            for (std::size_t i = 0; i < sizeof...(Ts); ++i) {
                const int column = archetype->ColumnOf(ids[i]);   // absent only for a Maybe
                columns[i] = column < 0 ? nullptr : &archetype->columns[static_cast<std::size_t>(column)];
            }
            for (std::size_t row = 0; row < count; ++row) {
                if (!view.AllMatch() && !view.Matches(row)) continue;
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    if constexpr (std::is_invocable_v<F&, Entity, detail::ArgumentType<Ts>...>)
                        f(archetype->entities[row], detail::Argument<Ts>(columns[I], row, now)...);
                    else
                        f(detail::Argument<Ts>(columns[I], row, now)...);
                }(std::index_sequence_for<Ts...> {});
            }
        }
    }

    template <typename T>
    void Commands::Add(const Entity entity, std::type_identity_t<T> value)
    {
        const ComponentId id = _world.Id<T>();
        void* stored = nullptr;
        if constexpr (!std::is_empty_v<T>) stored = ::new (Allocate(id)) T(std::move(value));
        _commands.push_back({ Kind::eAdd, entity, id, stored, {} });
    }
}
