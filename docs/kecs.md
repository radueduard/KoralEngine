# kecs: the ECS

`modules/kecs` is a library by itself: no GPU, no framework. Include `<kecs/kecs.h>`.

```cpp
kecs::World world;
const kecs::Entity ship = world.Create(Position { 0, 0 }, Velocity { 1, 0 });
world.Add<Player>(ship);                                    // a tag: a component with nothing in it

world.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; });
```

## Component types

A component type is a description registered with a world: a name, a size, how to make, copy, move and
destroy one, and its fields. `world.Id<T>()` describes a C++ type from the type itself, the first time
it is used. A type made at run time, such as the component generated from a pipeline's per-entity
parameters or one a script declares, is registered with `world.Register(description)` and used by id:

```cpp
kecs::ComponentDescription blur { .name = "blur.entity", .size = 12, .alignment = 4 };
blur.fields = { { "radius", "float", 0, 4 }, { "direction", "kor::Vec2", 4, 8 } };
const kecs::ComponentId id = world.Register(blur);
world.Add(entity, id, &values);          // a copy of 12 bytes; with no value, zeroes
```

- An operation left null is the plain one: zeroes, a byte copy, nothing to destroy. The operations are
  given the description, so types generated at run time can share functions and read their shape from
  `context`.
- A size of zero is a **tag**.
- A type is named after itself (`"Position"`, `"kecs::Parent"`) unless `kecs::Name<T>` is specialised.
  The name is what a type will be saved and matched across a reload by.

## Entities and components

| | |
|---|---|
| `Create()`, `Create(A{}, B{})` | A new entity, with nothing or with these. |
| `Destroy(e)` | It, and everything under it in the hierarchy. |
| `Add<T>(e, value)`, `Add(e, id, bytes)` | Gives it one, or replaces the one it has. `T` is always written out. |
| `Remove<T>(e)` | Takes it away. |
| `Get<T>(e)` / `Mutable<T>(e)` | To read, or to write (marked changed). Null when it has none. |
| `SetEnabled<T>(e, false)` | Switches it off for this entity without taking it away. |

Entities with the same components are stored together, a column a component. Adding or removing one
moves the entity to another group. Switching one off flips a bit and moves nothing.

## Queries

```cpp
world.Query<Position, const Velocity>()         // has both, switched on
     .With<Player>().Without<Frozen>()          // narrowed by tags
     .Changed<Velocity>(since)                  // only where velocity was written after `since`
     .Each([](kecs::Entity e, Position& p, const Velocity& v) { ... });
```

| In the query | The callback gets | Marked changed |
|---|---|---|
| `T` | `T&` | Every entity visited. |
| `const T` | `const T&` | Never. |
| `kecs::Mut<T>` | `Mut<T>`: read with `*m`, write with `m.Write()` | Only where written. |
| `kecs::Maybe<const T>` | `const T*`, null where the entity has none | Never. |

- A component switched off is passed by, and counts as not had for `Without`. `IncludeDisabled()`
  visits those too.
- `world.Each(QueryDescription, visit)` is the same by id, a group at a time, for queries made at run
  time: a node's, a tool's.
- **Change:** a system calls `AdvanceTick()` as each run starts and keeps what it returned. Next run,
  `Changed<T>(thatTick)` is everything written since.

## While a query runs

`Create`, `Destroy`, `Add`, `Remove` and `SetParent` throw: they would move what the query is walking.
Queue them, and apply them after:

```cpp
kecs::Commands commands(world);
world.Query<const Health>().Each([&](kecs::Entity e, const Health& h) { if (h.value <= 0) commands.Destroy(e); });
commands.Apply();
```

`commands.Create()` gives an entity that can be referred to at once and exists from `Apply` on.

## Hierarchy and observers

- `SetParent(child, parent)`, `ParentOf`, `ChildrenOf`. It keeps both ends right and refuses to put an
  entity under itself. Destroying an entity destroys what is under it.
- `OnAdd(id, f)` runs after an entity is given a component (not when one is replaced). `OnRemove(id, f)`
  runs before it loses one, including when the entity is destroyed.

## Fields

A type that is saved, shown in a tool or reshaped says what fields it has. A C++ type declares them
beside itself; a type made at run time lists them in its description.

```cpp
struct Position { float x = 0, y = 0; };
KECS_FIELDS(Position, x, y)            // at namespace scope, up to twelve members
```

Field types go by name. Numbers are `"f32"`, `"i64"`, `"u8"` and so on, the same on every platform;
`std::string` is `"string"` and `kecs::Entity` is `"entity"`. Any other type is named after itself, or
by `kecs::Name<T>`.

## A type that changes shape

`world.Reshape(id, newDescription)` gives a component type a new shape, which is what an edited shader
or a reloaded script does to the component generated from it.

- The id stays the same, so queries and observers of it go on working.
- Each entity's value starts as a default one. A field both shapes have, with the same name, type and
  size, is carried over wherever it has moved to. Anything else is left at its default.
- Whether the component is switched off for an entity is kept, and every one counts as changed.
- It returns a `Migration`: which fields were kept, reset and removed, and how many entities had it.

## Saving and loading

```cpp
const kecs::Saved saved = kecs::Save(world);         // saved.json, saved.problems
const kecs::Loaded loaded = kecs::Load(other, saved.json);   // loaded.entities, loaded.problems
```

A component is saved a field at a time, by name, each by the codec registered for its type's name
(`kecs::Codecs`). Numbers, `bool`, `string` and `entity` have codecs to begin with; register others for
your own types. The file also holds each component type's description, entities' children in order, and
which components are switched off.

| | Saved as |
|---|---|
| A field with a codec | What the codec writes: `"x": 1.5`. |
| A plain-data field with none | Its bytes: `{ "$bytes": "…" }`. |
| A field that owns something, with none | Not saved, and reported. |
| A plain-data component with no fields | Its bytes. |
| A component with no fields that owns something | Not saved, and reported. |

Loading adds to the world and reads by name, with the same rule as `Reshape`: a field the file and the
world's type both have, of the same type, is read, and any other is left at its default and reported.

- A plain-data type the world doesn't have is registered from the file, so run-time types load without
  whoever made them.
- A type that owns something has to be registered already (`world.Id<T>()`); otherwise it is left out
  and reported.
- An `entity` field is saved as the entity's number in the file and points at the entity that number
  became.
- A file saved, loaded into an empty world and saved again is the same file.

## Building and testing

```sh
cmake -S . -B cmake-build-debug -G Ninja && cmake --build cmake-build-debug
ctest --test-dir cmake-build-debug
cmake -S . -B cmake-build-sanitize -G Ninja -DKORAL_ENGINE_SANITIZE=ON    # ASan and UBSan
```

In a Release build on the development machine, a million entities are created with two components in
about 36 ms, and half a million are walked with two components in about 0.3 ms.
