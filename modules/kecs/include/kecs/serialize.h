#pragma once

/**
 * @file serialize.h
 * @brief A world, to JSON and back.
 *
 * What is saved is what components say of themselves: their fields, by name. A component type with fields
 * (KECS_FIELDS for a C++ type; whatever a generator or a script declared for one made at run time) is saved a field
 * at a time, each by the Codec registered for its type's name. The component types are saved with the entities, so
 * one made at run time loads again without whoever made it.
 *
 * Loading reads by name too. A field the file has and the world's type has, of the same type, is read; any other is
 * left at its default and reported — the rule a component type changing shape follows (World::Reshape).
 *
 * @code
 * const kecs::Saved saved = kecs::Save(world);
 * kecs::World other;
 * const kecs::Loaded loaded = kecs::Load(other, saved.json);
 * @endcode
 */

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "component.h"

namespace kecs
{
    class World;

    /** @brief What a codec writing a value may ask: which number an entity is saved as. */
    struct SaveContext {
        /** @brief The number @p entity has in the file, or -1 when it is not in it. */
        std::function<std::int64_t(Entity entity)> idOf;
    };

    /** @brief What a codec reading a value may ask: which entity a number in the file became. */
    struct LoadContext {
        std::function<Entity(std::int64_t id)> entityOf;
    };

    /** @brief Writes and reads the values of one field type as JSON text. */
    struct Codec {
        /** @brief The value at @p value, as JSON: a number, a string, an array, an object. */
        std::function<std::string(const void* value, const SaveContext& context)> write;
        /** @brief Sets the value at @p value, which is alive, from @p json. False when the text is not one of these. */
        std::function<bool(void* value, std::string_view json, const LoadContext& context)> read;
    };

    /**
     * @brief The codecs, by field type name. Numbers ("f32", "i64", "u8"...), "bool", "string" and "entity" are
     *        there to begin with; whoever has other types registers theirs.
     */
    class Codecs {
    public:
        Codecs();
        void Register(std::string type, Codec codec);
        [[nodiscard]] const Codec* Find(std::string_view type) const;
        /** @brief The ones Save and Load use unless given others. */
        static Codecs& Default();

    private:
        std::map<std::string, Codec, std::less<>> _codecs;
    };

    /** @brief A world saved. */
    struct Saved {
        std::string json;
        /// What could not be saved, and why: a component type with no fields that owns something, a field of a
        /// type with no codec that is not plain data.
        std::vector<std::string> problems;
    };

    /** @brief Saves every entity of @p world, with the components it has and whether each is switched off. */
    [[nodiscard]] Saved Save(const World& world, const Codecs& codecs = Codecs::Default());

    /** @brief What loading made. */
    struct Loaded {
        bool ok = false;
        std::string error;                  ///< Why not, when not ok: the text is not JSON, or not a saved world.
        std::vector<Entity> entities;       ///< The entities made, in the file's order.
        std::vector<std::string> problems;  ///< What the file had that the world could not take: each said once.
    };

    /**
     * @brief Adds what @p json holds to @p world: its entities, with their components, hierarchy and what is
     *        switched off. A component type the world does not have is registered as the file describes it.
     *        Nothing is made when the text cannot be read at all.
     */
    Loaded Load(World& world, std::string_view json, const Codecs& codecs = Codecs::Default());
}
