#pragma once

/**
 * @file component.h
 * @brief Entities, and component types described at run time.
 *
 * A component type is not a C++ type as far as the world is concerned: it is a description — a name, a size, how
 * to make, copy, move and destroy one, and what fields it has — registered with a world, which hands back an id.
 * A C++ struct is described from the struct (ComponentDescription::Of); a component generated from a shader's
 * parameters, or declared by a script, is described by whoever generates it. All of them are stored, queried and
 * saved the same way.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kecs
{
    /** @brief A component type's id in one world: what Register returns. Not the same number in another world. */
    using ComponentId = std::uint32_t;
    inline constexpr ComponentId NoComponent = std::numeric_limits<ComponentId>::max();

    /**
     * @brief Counts change: what a component is stamped with when it is added or written, and what a query asks
     *        "changed since" of. @see World::AdvanceTick
     */
    using Tick = std::uint32_t;

    /**
     * @brief An entity: a place in the world's table, and which use of that place it is. An entity destroyed leaves
     *        its place to another with a later generation, so a stale Entity is told apart from the new one.
     */
    struct Entity {
        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;

        [[nodiscard]] constexpr bool IsNull() const { return index == std::numeric_limits<std::uint32_t>::max(); }
        constexpr bool operator==(const Entity&) const = default;
        constexpr auto operator<=>(const Entity&) const = default;
    };
    inline constexpr Entity NullEntity {};

    /** @brief One field of a component: what a tool shows, what is saved, and what a changed type keeps by name and type. */
    struct FieldDescription {
        std::string name;
        std::string type;           ///< By name: "float", "kor::Vec3", "Image".
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    /**
     * @brief A component type: its name, its size, and how one is made, copied, moved and destroyed.
     *
     * The four operations take the description itself, so a type generated at run time (a struct of so many
     * resource references, say) can share one set of functions and read its own shape from @ref context. An
     * operation left null is the plain one: zeroes for construct, a byte copy for copy and relocate, nothing for
     * destroy — which is all a plain-data component needs.
     *
     * A size of zero is a tag: an entity has it or not, and nothing is stored.
     */
    struct ComponentDescription {
        std::string name;
        std::size_t size = 0;
        std::size_t alignment = 1;
        /** @brief Makes a default one in raw memory. */
        void (*construct)(void* at, const ComponentDescription& type) = nullptr;
        /** @brief Makes one in raw memory as a copy of another. */
        void (*copy)(void* to, const void* from, const ComponentDescription& type) = nullptr;
        /** @brief Moves one into raw memory, and destroys where it came from. */
        void (*relocate)(void* to, void* from, const ComponentDescription& type) = nullptr;
        void (*destroy)(void* at, const ComponentDescription& type) = nullptr;
        /// Whether one can be copied at all: false for a type that can only be moved, which is then added by
        /// moving it in (World::Add<T>) and never by copying a value.
        bool copyable = true;
        std::vector<FieldDescription> fields;
        std::shared_ptr<const void> context;    ///< Whatever the operations need of a type made at run time.

        [[nodiscard]] bool IsTag() const { return size == 0; }

        /** @brief Describes the C++ type @p T, named @p name: its own constructors and destructor. */
        template <typename T>
        static ComponentDescription Of(std::string name);

        // The operations, with what a null one stands for.
        void Construct(void* at) const { if (construct) construct(at, *this); else std::memset(at, 0, size); }
        void Copy(void* to, const void* from) const { if (copy) copy(to, from, *this); else std::memcpy(to, from, size); }
        void Relocate(void* to, void* from) const { if (relocate) relocate(to, from, *this); else std::memcpy(to, from, size); }
        void Destroy(void* at) const { if (destroy) destroy(at, *this); }
    };

    namespace detail
    {
        /** "kecs::Parent" out of the compiler's spelling of a function that names T. */
        template <typename T>
        constexpr std::string_view RawTypeName()
        {
#if defined(__clang__) || defined(__GNUC__)
            constexpr std::string_view function = __PRETTY_FUNCTION__;
            constexpr std::string_view marker = "T = ";
            const auto start = function.find(marker) + marker.size();
            const auto end = function.find_first_of(";]", start);
            return function.substr(start, end - start);
#elif defined(_MSC_VER)
            constexpr std::string_view function = __FUNCSIG__;
            constexpr std::string_view marker = "RawTypeName<";
            auto start = function.find(marker) + marker.size();
            for (const std::string_view prefix : { "struct ", "class ", "enum " })
                if (function.substr(start, prefix.size()) == prefix) start += prefix.size();
            const auto end = function.rfind(">(void)");
            return function.substr(start, end - start);
#else
            return "unknown";
#endif
        }

        /** A number a C++ type, the same in every world: what a world looks its ComponentId up by. */
        std::uint32_t NextTypeIndex();
        template <typename T>
        std::uint32_t TypeIndex()
        {
            static const std::uint32_t index = NextTypeIndex();
            return index;
        }
    }

    /**
     * @brief What a C++ component type is called: its qualified name, unless it is given another by specialising
     *        this — `template <> struct kecs::Name<Position> { static constexpr std::string_view value = "Position"; };`.
     *        The name is what a type is saved, shown and matched across a reload by.
     */
    template <typename T>
    struct Name {
        static constexpr std::string_view value = detail::RawTypeName<T>();
    };

    template <typename T>
    ComponentDescription ComponentDescription::Of(std::string name)
    {
        static_assert(std::is_destructible_v<T> && std::is_move_constructible_v<T>, "a component is moved as its entity gains and loses others");
        ComponentDescription d;
        d.name = std::move(name);
        if (std::is_empty_v<T>) return d;   // a tag
        d.size = sizeof(T);
        d.alignment = alignof(T);
        if constexpr (std::is_default_constructible_v<T>) {
            // Value-initialised: a plain struct starts zeroed, and its member initialisers are run.
            d.construct = [](void* at, const ComponentDescription&) { ::new (at) T(); };
        }
        if constexpr (!std::is_trivially_copyable_v<T>) {
            if constexpr (std::is_copy_constructible_v<T>)
                d.copy = [](void* to, const void* from, const ComponentDescription&) { ::new (to) T(*static_cast<const T*>(from)); };
            else
                d.copyable = false;
            d.relocate = [](void* to, void* from, const ComponentDescription&) {
                ::new (to) T(std::move(*static_cast<T*>(from)));
                static_cast<T*>(from)->~T();
            };
            d.destroy = [](void* at, const ComponentDescription&) { static_cast<T*>(at)->~T(); };
        }
        return d;
    }
}
