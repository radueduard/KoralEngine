#pragma once

// Component types the tests share.

#include <memory>
#include <string>

#include <kecs/kecs.h>

struct Position { float x = 0, y = 0; };
struct Velocity { float x = 0, y = 0; };
struct Health { int value = 100; };
struct Player {};      // tags
struct Frozen {};

/** Counts how many of it are alive: what tells a component destroyed twice, or never. */
struct Counted {
    static inline int alive = 0;
    std::string text;
    explicit Counted(std::string t = {}) : text(std::move(t)) { ++alive; }
    Counted(const Counted& o) : text(o.text) { ++alive; }
    Counted(Counted&& o) noexcept : text(std::move(o.text)) { ++alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --alive; }
};

struct Owned {      // can only be moved
    std::unique_ptr<int> value;
};

template <> struct kecs::Name<Health> { static constexpr std::string_view value = "Health"; };

KECS_FIELDS(Position, x, y)
KECS_FIELDS(Velocity, x, y)
KECS_FIELDS(Health, value)

/** One with a field that owns something, and one that names another entity. */
struct Named {
    std::string name;
    kecs::Entity target;
    double weight = 1.5;
};
KECS_FIELDS(Named, name, target, weight)
