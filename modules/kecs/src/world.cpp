//
// kecs: the world. Entities of one shape live together in an archetype, a column a component; an entity gaining or
// losing a component moves between archetypes, and the last row of the one it left takes its place.
//

#include <kecs/world.h>

#include <atomic>
#include <cassert>

namespace kecs
{
    std::uint32_t detail::NextTypeIndex()
    {
        static std::atomic<std::uint32_t> next { 0 };
        return next++;
    }

    int detail::Archetype::ColumnOf(const ComponentId id) const
    {
        const auto it = std::ranges::lower_bound(ids, id);
        return it != ids.end() && *it == id ? static_cast<int>(it - ids.begin()) : -1;
    }

    namespace {
        using detail::Archetype;
        using detail::Column;

        void SetDisabled(Column& column, const std::size_t row, const bool disabled)
        {
            const std::uint64_t bit = std::uint64_t { 1 } << (row & 63);
            std::uint64_t& word = column.disabled[row >> 6];
            const bool was = (word & bit) != 0;
            if (was == disabled) return;
            word = disabled ? word | bit : word & ~bit;
            column.disabledCount += disabled ? 1 : static_cast<std::size_t>(-1);
        }

        std::byte* AllocateColumn(const ComponentDescription& type, const std::size_t capacity)
        {
            if (type.IsTag() || capacity == 0) return nullptr;
            return static_cast<std::byte*>(::operator new(type.size * capacity, std::align_val_t { type.alignment }));
        }

        void FreeColumn(const ComponentDescription& type, std::byte* data)
        {
            if (data) ::operator delete(data, std::align_val_t { type.alignment });
        }

        const std::vector<Entity> NoChildren;
    }

    // ---- chunks ------------------------------------------------------------------------------------------------

    bool ChunkView::Matches(const std::size_t row) const
    {
        const auto& q = *_query;
        if (!q.includeDisabled)
            for (const ComponentId id : q.all)
                if (_archetype->columns[static_cast<std::size_t>(_archetype->ColumnOf(id))].Disabled(row)) return false;
        // A component it must not have, had but switched off, is as good as not had; switched on, the entity is out.
        for (const ComponentId id : q.none)
            if (const int column = _archetype->ColumnOf(id); column >= 0 && !_archetype->columns[static_cast<std::size_t>(column)].Disabled(row)) return false;
        for (const ComponentId id : q.changed) {
            const int column = _archetype->ColumnOf(id);
            if (column < 0 || _archetype->columns[static_cast<std::size_t>(column)].changed[row] <= q.since) return false;
        }
        return true;
    }

    void* ChunkView::Column(const ComponentId id) const
    {
        const int column = _archetype->ColumnOf(id);
        return column < 0 ? nullptr : _archetype->columns[static_cast<std::size_t>(column)].data;
    }

    void ChunkView::MarkChanged(const ComponentId id, const std::size_t row) const
    {
        if (const int column = _archetype->ColumnOf(id); column >= 0) _archetype->columns[static_cast<std::size_t>(column)].changed[row] = _now;
    }

    // ---- the world ----------------------------------------------------------------------------------------------

    World::World()
    {
        ArchetypeOf({});    // the archetype of entities with nothing
    }

    World::~World()
    {
        for (const auto& archetype : _archetypes) {
            for (auto& column : archetype->columns) {
                if (column.type->destroy)
                    for (std::size_t row = 0; row < archetype->entities.size(); ++row) column.type->Destroy(column.At(row));
                FreeColumn(*column.type, column.data);
            }
        }
    }

    void World::RequireStill(const char* what) const
    {
        if (_iterating != 0)
            throw std::logic_error(std::string("kecs: ") + what + " while a query runs would move what it is walking: queue it in a kecs::Commands, and Apply that afterwards");
    }

    ComponentId World::Register(ComponentDescription description)
    {
        if (description.name.empty()) throw std::logic_error("kecs: a component type needs a name");
        if (description.alignment == 0) description.alignment = 1;
        if (const auto known = _byName.find(description.name); known != _byName.end()) {
            const auto& existing = *_types[known->second];
            if (existing.size != description.size || existing.alignment != description.alignment)
                throw std::logic_error("kecs: '" + description.name + "' is registered already, with another size");
            return known->second;
        }
        const auto id = static_cast<ComponentId>(_types.size());
        _byName.emplace(description.name, id);
        _types.push_back(std::make_unique<ComponentDescription>(std::move(description)));
        _onAdd.emplace_back();
        _onRemove.emplace_back();
        return id;
    }

    ComponentId World::IdOf(const std::uint32_t typeIndex, ComponentDescription (*describe)())
    {
        if (typeIndex < _byTypeIndex.size() && _byTypeIndex[typeIndex] != NoComponent) return _byTypeIndex[typeIndex];
        if (typeIndex >= _byTypeIndex.size()) _byTypeIndex.resize(typeIndex + 1, NoComponent);
        return _byTypeIndex[typeIndex] = Register(describe());
    }

    ComponentId World::Find(const std::string_view name) const
    {
        const auto it = _byName.find(std::string(name));
        return it == _byName.end() ? NoComponent : it->second;
    }

    const ComponentDescription& World::Describe(const ComponentId id) const
    {
        if (id >= _types.size()) throw std::logic_error("kecs: no component type has that id");
        return *_types[id];
    }

    // ---- archetypes ---------------------------------------------------------------------------------------------

    Archetype& World::ArchetypeOf(std::vector<ComponentId> sortedIds)
    {
        if (const auto it = _byIds.find(sortedIds); it != _byIds.end()) return *it->second;
        auto archetype = std::make_unique<Archetype>();
        archetype->ids = sortedIds;
        archetype->columns.resize(sortedIds.size());
        for (std::size_t i = 0; i < sortedIds.size(); ++i) archetype->columns[i].type = _types[sortedIds[i]].get();
        Archetype& made = *archetype;
        _archetypes.push_back(std::move(archetype));
        _byIds.emplace(std::move(sortedIds), &made);
        return made;
    }

    Archetype& World::ArchetypeWith(Archetype& from, const ComponentId id)
    {
        if (const auto it = from.with.find(id); it != from.with.end()) return *it->second;
        std::vector<ComponentId> ids = from.ids;
        ids.insert(std::ranges::lower_bound(ids, id), id);
        Archetype& to = ArchetypeOf(std::move(ids));
        from.with.emplace(id, &to);
        to.without.emplace(id, &from);
        return to;
    }

    Archetype& World::ArchetypeWithout(Archetype& from, const ComponentId id)
    {
        if (const auto it = from.without.find(id); it != from.without.end()) return *it->second;
        std::vector<ComponentId> ids = from.ids;
        std::erase(ids, id);
        Archetype& to = ArchetypeOf(std::move(ids));
        from.without.emplace(id, &to);
        to.with.emplace(id, &from);
        return to;
    }

    void World::Reserve(Archetype& archetype, const std::size_t capacity)
    {
        if (capacity <= archetype.capacity) return;
        const std::size_t count = archetype.entities.size();
        for (auto& column : archetype.columns) {
            std::byte* data = AllocateColumn(*column.type, capacity);
            if (column.data) {
                const std::size_t size = column.type->size;
                if (column.type->relocate) {
                    for (std::size_t row = 0; row < count; ++row) column.type->Relocate(data + row * size, column.data + row * size);
                } else {
                    std::memcpy(data, column.data, count * size);
                }
                FreeColumn(*column.type, column.data);
            }
            column.data = data;
            column.added.resize(capacity);
            column.changed.resize(capacity);
            column.disabled.resize((capacity + 63) / 64, 0);
        }
        archetype.entities.reserve(capacity);
        archetype.capacity = capacity;
    }

    std::uint32_t World::Push(Archetype& archetype, const Entity entity)
    {
        const std::size_t row = archetype.entities.size();
        if (row == archetype.capacity) Reserve(archetype, std::max<std::size_t>(archetype.capacity * 2, 16));
        archetype.entities.push_back(entity);
        for (auto& column : archetype.columns) {
            column.added[row] = column.changed[row] = _tick;
            // The bit may be left from a row that was here before.
            SetDisabled(column, row, false);
        }
        return static_cast<std::uint32_t>(row);
    }

    void World::SwapRemove(Archetype& archetype, const std::uint32_t row)
    {
        // What was at @p row has been moved out or destroyed already: the last row takes its place.
        const std::size_t last = archetype.entities.size() - 1;
        for (auto& column : archetype.columns) {
            const bool lastDisabled = column.Disabled(last);
            SetDisabled(column, row, false);
            if (row != last) {
                if (!column.type->IsTag()) column.type->Relocate(column.At(row), column.At(last));
                column.added[row] = column.added[last];
                column.changed[row] = column.changed[last];
                SetDisabled(column, row, lastDisabled);
            }
            SetDisabled(column, last, false);
        }
        if (row != last) {
            const Entity moved = archetype.entities[last];
            archetype.entities[row] = moved;
            _records[moved.index].row = row;
        }
        archetype.entities.pop_back();
    }

    std::uint32_t World::Move(Record& record, const Entity entity, Archetype& to)
    {
        Archetype& from = *record.archetype;
        const std::uint32_t oldRow = record.row;
        const std::uint32_t newRow = Push(to, entity);
        for (std::size_t i = 0; i < from.columns.size(); ++i) {
            Column& source = from.columns[i];
            const int target = to.ColumnOf(from.ids[i]);
            if (target < 0) {
                if (!source.type->IsTag()) source.type->Destroy(source.At(oldRow));
                continue;
            }
            Column& destination = to.columns[static_cast<std::size_t>(target)];
            if (!source.type->IsTag()) source.type->Relocate(destination.At(newRow), source.At(oldRow));
            destination.added[newRow] = source.added[oldRow];
            destination.changed[newRow] = source.changed[oldRow];
            SetDisabled(destination, newRow, source.Disabled(oldRow));
        }
        SwapRemove(from, oldRow);
        record.archetype = &to;
        record.row = newRow;
        return newRow;
    }

    // ---- entities ------------------------------------------------------------------------------------------------

    const World::Record* World::RecordOf(const Entity entity) const
    {
        if (entity.index >= _records.size()) return nullptr;
        const Record& record = _records[entity.index];
        return record.alive && record.generation == entity.generation ? &record : nullptr;
    }

    World::Record* World::RecordOf(const Entity entity) { return const_cast<Record*>(std::as_const(*this).RecordOf(entity)); }

    Entity World::Reserve()
    {
        std::uint32_t index;
        if (!_free.empty()) {
            index = _free.back();
            _free.pop_back();
        } else {
            index = static_cast<std::uint32_t>(_records.size());
            _records.emplace_back();
        }
        Record& record = _records[index];
        record.alive = true;
        record.archetype = nullptr;
        ++_alive;
        return { index, record.generation };
    }

    void World::Place(const Entity entity)
    {
        Record& record = _records[entity.index];
        if (record.archetype) return;
        Archetype& empty = *_archetypes.front();
        record.archetype = &empty;
        record.row = Push(empty, entity);
    }

    Entity World::Create()
    {
        RequireStill("Create");
        const Entity entity = Reserve();
        Place(entity);
        return entity;
    }

    bool World::Alive(const Entity entity) const { return RecordOf(entity) != nullptr; }

    bool World::Destroy(const Entity entity)
    {
        RequireStill("Destroy");
        if (!RecordOf(entity)) return false;

        // Everything under it goes first; the list is copied, since each one takes itself out of it.
        if (_children != NoComponent)
            if (const auto* children = static_cast<const Children*>(Get(entity, _children))) {
                const std::vector<Entity> under = children->entities;
                for (const Entity child : under) Destroy(child);
            }
        // And it leaves what it was under.
        if (_parent != NoComponent)
            if (const auto* parent = static_cast<const Parent*>(Get(entity, _parent)))
                if (auto* siblings = static_cast<Children*>(const_cast<void*>(Get(parent->entity, _children)))) std::erase(siblings->entities, entity);

        Record* record = RecordOf(entity);
        if (record->archetype) {
            for (const ComponentId id : std::vector(record->archetype->ids)) NotifyRemove(entity, id);
            record = RecordOf(entity);
            Archetype& archetype = *record->archetype;
            for (auto& column : archetype.columns)
                if (!column.type->IsTag()) column.type->Destroy(column.At(record->row));
            SwapRemove(archetype, record->row);
        }
        record->alive = false;
        record->archetype = nullptr;
        ++record->generation;
        _free.push_back(entity.index);
        --_alive;
        return true;
    }

    void World::EachEntity(const std::function<void(Entity)>& visit) const
    {
        for (std::uint32_t index = 0; index < _records.size(); ++index)
            if (_records[index].alive) visit({ index, _records[index].generation });
    }

    // ---- components -----------------------------------------------------------------------------------------------

    void* World::Slot(const Entity entity, const ComponentId id, bool& existed)
    {
        Record& record = _records[entity.index];
        Place(entity);
        Archetype& from = *record.archetype;
        if (const int column = from.ColumnOf(id); column >= 0) {
            existed = true;
            return from.columns[static_cast<std::size_t>(column)].type->IsTag() ? nullptr : from.columns[static_cast<std::size_t>(column)].At(record.row);
        }
        existed = false;
        Archetype& to = ArchetypeWith(from, id);
        const std::uint32_t row = Move(record, entity, to);
        Column& column = to.columns[static_cast<std::size_t>(to.ColumnOf(id))];
        return column.type->IsTag() ? nullptr : column.At(row);
    }

    void World::Added(const Entity entity, const ComponentId id, const bool existed)
    {
        const Record& record = _records[entity.index];
        Column& column = record.archetype->columns[static_cast<std::size_t>(record.archetype->ColumnOf(id))];
        column.changed[record.row] = _tick;
        if (existed) return;
        column.added[record.row] = _tick;
        // Copied: an observer may register another.
        for (const auto& observer : std::vector(_onAdd[id])) observer(*this, entity);
    }

    void World::NotifyRemove(const Entity entity, const ComponentId id)
    {
        if (_onRemove[id].empty()) return;
        for (const auto& observer : std::vector(_onRemove[id])) observer(*this, entity);
    }

    bool World::Add(const Entity entity, const ComponentId id, const void* value)
    {
        RequireStill("Add");
        if (!RecordOf(entity) || id >= _types.size()) return false;
        const ComponentDescription& type = *_types[id];
        if (value && !type.copyable) return false;
        bool existed = false;
        void* slot = Slot(entity, id, existed);
        if (slot) {
            if (existed) type.Destroy(slot);
            if (value) type.Copy(slot, value);
            else type.Construct(slot);
        }
        Added(entity, id, existed);
        return true;
    }

    bool World::Remove(const Entity entity, const ComponentId id)
    {
        RequireStill("Remove");
        Record* record = RecordOf(entity);
        if (!record || !record->archetype || !record->archetype->Has(id)) return false;
        NotifyRemove(entity, id);
        record = RecordOf(entity);
        if (!record || !record->archetype || !record->archetype->Has(id)) return false;   // an observer took it, or the entity
        Move(*record, entity, ArchetypeWithout(*record->archetype, id));
        return true;
    }

    const void* World::Get(const Entity entity, const ComponentId id) const
    {
        const Record* record = RecordOf(entity);
        if (!record || !record->archetype) return nullptr;
        const int column = record->archetype->ColumnOf(id);
        if (column < 0) return nullptr;
        const Column& c = record->archetype->columns[static_cast<std::size_t>(column)];
        return c.type->IsTag() ? nullptr : c.At(record->row);
    }

    void* World::Mutable(const Entity entity, const ComponentId id)
    {
        void* value = const_cast<void*>(std::as_const(*this).Get(entity, id));
        if (value) MarkChanged(entity, id);
        return value;
    }

    bool World::Has(const Entity entity, const ComponentId id) const
    {
        const Record* record = RecordOf(entity);
        return record && record->archetype && record->archetype->Has(id);
    }

    bool World::SetEnabled(const Entity entity, const ComponentId id, const bool enabled)
    {
        const Record* record = RecordOf(entity);
        if (!record || !record->archetype) return false;
        const int column = record->archetype->ColumnOf(id);
        if (column < 0) return false;
        SetDisabled(record->archetype->columns[static_cast<std::size_t>(column)], record->row, !enabled);
        return true;
    }

    bool World::IsEnabled(const Entity entity, const ComponentId id) const
    {
        const Record* record = RecordOf(entity);
        if (!record || !record->archetype) return false;
        const int column = record->archetype->ColumnOf(id);
        return column >= 0 && !record->archetype->columns[static_cast<std::size_t>(column)].Disabled(record->row);
    }

    bool World::MarkChanged(const Entity entity, const ComponentId id)
    {
        const Record* record = RecordOf(entity);
        if (!record || !record->archetype) return false;
        const int column = record->archetype->ColumnOf(id);
        if (column < 0) return false;
        record->archetype->columns[static_cast<std::size_t>(column)].changed[record->row] = _tick;
        return true;
    }

    std::vector<ComponentId> World::ComponentsOf(const Entity entity) const
    {
        const Record* record = RecordOf(entity);
        return record && record->archetype ? record->archetype->ids : std::vector<ComponentId> {};
    }

    // ---- queries --------------------------------------------------------------------------------------------------

    const std::vector<Archetype*>& World::Matching(const QueryDescription& query)
    {
        // By what it must have, and — where switching off is not looked at — what it must not: an archetype that has
        // one of those may still hold entities with it switched off, so then every archetype with all of the first stays in.
        std::pair<std::vector<ComponentId>, std::vector<ComponentId>> key { query.all, {} };
        std::ranges::sort(key.first);
        Cached& cached = _queries[std::move(key)];
        for (; cached.seen < _archetypes.size(); ++cached.seen) {
            Archetype& archetype = *_archetypes[cached.seen];
            if (std::ranges::all_of(query.all, [&](const ComponentId id) { return archetype.Has(id); })) cached.matches.push_back(&archetype);
        }
        return cached.matches;
    }

    bool World::Excluded(const Archetype& archetype, const QueryDescription& query) const
    {
        // Every entity here has something it must not have, switched on: none of them can match.
        return std::ranges::any_of(query.none, [&](const ComponentId id) {
            const int column = archetype.ColumnOf(id);
            return column >= 0 && archetype.columns[static_cast<std::size_t>(column)].disabledCount == 0;
        });
    }

    ChunkView World::View(Archetype& archetype, const QueryDescription& query) const
    {
        ChunkView view;
        view._archetype = &archetype;
        view._query = &query;
        view._now = _tick;
        // Nothing to ask row by row when nothing here is switched off, none of what it must not have is here, and
        // nothing is asked about change.
        view._allMatch = query.changed.empty();
        if (view._allMatch && !query.includeDisabled)
            for (const ComponentId id : query.all)
                if (archetype.columns[static_cast<std::size_t>(archetype.ColumnOf(id))].disabledCount != 0) view._allMatch = false;
        for (const ComponentId id : query.none)
            if (archetype.Has(id)) view._allMatch = false;
        return view;
    }

    void World::Each(const QueryDescription& query, const std::function<void(const ChunkView&)>& visit)
    {
        const auto& archetypes = Matching(query);
        ++_iterating;
        struct Unlock { int& count; ~Unlock() { --count; } } unlock { _iterating };
        for (Archetype* archetype : archetypes) {
            if (archetype->entities.empty()) continue;
            if (Excluded(*archetype, query)) continue;
            visit(View(*archetype, query));
        }
    }

    std::size_t World::Count(const QueryDescription& query)
    {
        std::size_t count = 0;
        Each(query, [&](const ChunkView& chunk) {
            if (chunk.AllMatch()) { count += chunk.Count(); return; }
            for (std::size_t row = 0; row < chunk.Count(); ++row) count += chunk.Matches(row) ? 1 : 0;
        });
        return count;
    }

    // ---- the hierarchy ----------------------------------------------------------------------------------------------

    bool World::SetParent(const Entity child, const Entity parent)
    {
        RequireStill("SetParent");
        if (!Alive(child) || (!parent.IsNull() && !Alive(parent)) || child == parent) return false;
        if (_parent == NoComponent) { _parent = Id<Parent>(); _children = Id<Children>(); }
        // Not under something that is under it.
        for (Entity at = parent; !at.IsNull(); at = ParentOf(at))
            if (at == child) return false;

        if (const Entity old = ParentOf(child); !old.IsNull()) {
            if (old == parent) return true;
            if (auto* siblings = static_cast<Children*>(const_cast<void*>(Get(old, _children)))) std::erase(siblings->entities, child);
        }
        if (parent.IsNull()) {
            Remove(child, _parent);
            return true;
        }
        Add<Parent>(child, Parent { parent });
        if (!Has(parent, _children)) Add<Children>(parent, Children {});
        static_cast<Children*>(const_cast<void*>(Get(parent, _children)))->entities.push_back(child);
        MarkChanged(parent, _children);
        return true;
    }

    Entity World::ParentOf(const Entity entity) const
    {
        if (_parent == NoComponent) return NullEntity;
        const auto* parent = static_cast<const Parent*>(Get(entity, _parent));
        return parent ? parent->entity : NullEntity;
    }

    const std::vector<Entity>& World::ChildrenOf(const Entity entity) const
    {
        if (_children == NoComponent) return NoChildren;
        const auto* children = static_cast<const Children*>(Get(entity, _children));
        return children ? children->entities : NoChildren;
    }

    // ---- observers --------------------------------------------------------------------------------------------------

    void World::OnAdd(const ComponentId id, std::function<void(World&, Entity)> observer)
    {
        if (id >= _onAdd.size()) throw std::logic_error("kecs: no component type has that id");
        _onAdd[id].push_back(std::move(observer));
    }

    void World::OnRemove(const ComponentId id, std::function<void(World&, Entity)> observer)
    {
        if (id >= _onRemove.size()) throw std::logic_error("kecs: no component type has that id");
        _onRemove[id].push_back(std::move(observer));
    }

    // ---- commands ---------------------------------------------------------------------------------------------------

    void* Commands::Allocate(const ComponentId id)
    {
        const ComponentDescription& type = _world.Describe(id);
        return ::operator new(type.size, std::align_val_t { type.alignment });
    }

    Commands::~Commands()
    {
        // What was never applied still owns what it carried.
        for (const auto& command : _commands) {
            if (command.kind != Kind::eAdd || !command.value) continue;
            const ComponentDescription& type = _world.Describe(command.id);
            type.Destroy(command.value);
            ::operator delete(command.value, std::align_val_t { type.alignment });
        }
    }

    Entity Commands::Create()
    {
        const Entity entity = _world.Reserve();
        _commands.push_back({ Kind::eCreate, entity, NoComponent, nullptr, {} });
        return entity;
    }

    void Commands::Destroy(const Entity entity) { _commands.push_back({ Kind::eDestroy, entity, NoComponent, nullptr, {} }); }

    void Commands::Add(const Entity entity, const ComponentId id, const void* value)
    {
        const ComponentDescription& type = _world.Describe(id);
        void* stored = nullptr;
        if (value && !type.IsTag() && type.copyable) {
            stored = Allocate(id);
            type.Copy(stored, value);
        }
        _commands.push_back({ Kind::eAdd, entity, id, stored, {} });
    }

    void Commands::Remove(const Entity entity, const ComponentId id) { _commands.push_back({ Kind::eRemove, entity, id, nullptr, {} }); }

    void Commands::SetParent(const Entity child, const Entity parent) { _commands.push_back({ Kind::eSetParent, child, NoComponent, nullptr, parent }); }

    void Commands::Apply()
    {
        _world.RequireStill("Commands::Apply");
        // Taken first: what applying one does — an observer — may queue more.
        std::vector<Command> commands = std::move(_commands);
        _commands.clear();
        for (auto& command : commands) {
            switch (command.kind) {
            case Kind::eCreate:
                if (_world.Alive(command.entity)) _world.Place(command.entity);
                break;
            case Kind::eDestroy:
                _world.Destroy(command.entity);
                break;
            case Kind::eAdd: {
                const ComponentDescription& type = _world.Describe(command.id);
                if (_world.Alive(command.entity)) {
                    bool existed = false;
                    void* slot = _world.Slot(command.entity, command.id, existed);
                    if (slot) {
                        if (existed) type.Destroy(slot);
                        if (command.value) type.Relocate(slot, command.value);
                        else type.Construct(slot);
                    }
                    _world.Added(command.entity, command.id, existed);
                } else if (command.value) {
                    type.Destroy(command.value);
                }
                if (command.value) ::operator delete(command.value, std::align_val_t { type.alignment });
                command.value = nullptr;
                break;
            }
            case Kind::eRemove:
                _world.Remove(command.entity, command.id);
                break;
            case Kind::eSetParent:
                _world.SetParent(command.entity, command.other);
                break;
            }
        }
    }
}
