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
        std::string type;           ///< By name: "f32", "kor::Vec3", "Image". What it is saved and matched by.
        std::size_t offset = 0;
        std::size_t size = 0;
        /**
         * @brief Copies one field's value over another's, both alive: how a field is carried from a component to
         *        the one that replaces it when its type changes shape. Null is a byte copy, which is right for
         *        plain data and wrong for anything that owns something.
         */
        void (*assign)(void* to, const void* from, const FieldDescription& field) = nullptr;
        /// The least and the most a number of it is — each component of a vector's — as a tool lets it be set: what a
        /// script's `[Range(0, 10)]` or `[Min(0)]` says. Unbounded, when nothing says.
        float min = -std::numeric_limits<float>::infinity(), max = std::numeric_limits<float>::infinity();
        /// Set by so much of what it is, not by so much: a script's `[Logarithmic]` — a distance, an intensity, a size.
        bool logarithmic = false;
        /// A whole number's names, when it is one of so many things: 0 is the first, 1 the second... It is kept, saved
        /// and set as the number; a tool shows, and is given, the name.
        std::vector<std::string> choices;

        /** @brief Whether @p other is this field for the purposes of keeping a value: the same name, type and size. */
        [[nodiscard]] bool SameAs(const FieldDescription& other) const { return name == other.name && type == other.type && size == other.size; }
        void Assign(void* to, const void* from) const { if (assign) assign(to, from, *this); else std::memcpy(to, from, size); }
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

        /**
         * @brief Describes the C++ type @p T, named @p name: its own constructors and destructor, and the fields
         *        it declares (KECS_FIELDS), if it declares any.
         */
        template <typename T>
        static ComponentDescription Of(std::string name);

        /** @brief The field so named, or null. */
        [[nodiscard]] const FieldDescription* FindField(const std::string_view field) const
        {
            for (const auto& f : fields) if (f.name == field) return &f;
            return nullptr;
        }

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

    /**
     * @brief Numbers are named by what they are, not by how a compiler spells them: "f32", "i64", "u8", "bool" —
     *        the same on every platform, which a saved file needs.
     */
    template <typename T> requires std::is_arithmetic_v<T>
    struct Name<T> {
        static constexpr std::string_view value =
            std::is_same_v<T, bool> ? std::string_view("bool")
            : std::is_floating_point_v<T> ? (sizeof(T) == 4 ? std::string_view("f32") : std::string_view("f64"))
            : std::is_signed_v<T> ? (sizeof(T) == 1 ? std::string_view("i8") : sizeof(T) == 2 ? std::string_view("i16")
                                     : sizeof(T) == 4 ? std::string_view("i32") : std::string_view("i64"))
            : (sizeof(T) == 1 ? std::string_view("u8") : sizeof(T) == 2 ? std::string_view("u16")
               : sizeof(T) == 4 ? std::string_view("u32") : std::string_view("u64"));
    };
    template <> struct Name<std::string> { static constexpr std::string_view value = "string"; };
    template <> struct Name<Entity> { static constexpr std::string_view value = "entity"; };

    /** @brief Collects a C++ type's fields: what Fields<T>::Describe is handed. */
    template <typename T>
    class FieldList {
    public:
        explicit FieldList(std::vector<FieldDescription>& fields) : _fields(fields) {}

        /** @brief Declares the member @p member as a field called @p name. */
        template <typename F>
        FieldList& Field(std::string name, F T::* member)
        {
            FieldDescription field;
            field.name = std::move(name);
            field.type = std::string(Name<F>::value);
            // Where the member is in a T, without making one.
            alignas(T) static const std::byte storage[sizeof(T)] {};
            const T* object = reinterpret_cast<const T*>(storage);
            field.offset = static_cast<std::size_t>(reinterpret_cast<const std::byte*>(&(object->*member)) - storage);
            field.size = sizeof(F);
            if constexpr (!std::is_trivially_copyable_v<F>) {
                static_assert(std::is_copy_assignable_v<F>, "a field that is not plain data has to be assignable, to be carried to a new shape");
                field.assign = [](void* to, const void* from, const FieldDescription&) { *static_cast<F*>(to) = *static_cast<const F*>(from); };
            }
            _fields.push_back(std::move(field));
            return *this;
        }

    private:
        std::vector<FieldDescription>& _fields;
    };

    /**
     * @brief What fields a C++ component type has: specialise it, or use KECS_FIELDS, for a type that is saved,
     *        shown in a tool, or reshaped. A type that declares none is stored and queried all the same.
     *
     * @code
     * struct Position { float x = 0, y = 0; };
     * KECS_FIELDS(Position, x, y)
     * @endcode
     */
    template <typename T>
    struct Fields {
        static void Describe(FieldList<T>&) {}
    };

    template <typename T>
    ComponentDescription ComponentDescription::Of(std::string name)
    {
        static_assert(std::is_destructible_v<T> && std::is_move_constructible_v<T>, "a component is moved as its entity gains and loses others");
        ComponentDescription d;
        d.name = std::move(name);
        if (std::is_empty_v<T>) return d;   // a tag
        FieldList<T> fields(d.fields);
        Fields<T>::Describe(fields);
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

#define KECS_DETAIL_FIELD(Type, member) list.Field(#member, &Type::member);
#define KECS_DETAIL_EACH_1(Type, a) KECS_DETAIL_FIELD(Type, a)
#define KECS_DETAIL_EACH_2(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_1(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_3(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_2(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_4(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_3(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_5(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_4(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_6(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_5(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_7(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_6(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_8(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_7(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_9(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_8(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_10(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_9(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_11(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_10(Type, __VA_ARGS__)
#define KECS_DETAIL_EACH_12(Type, a, ...) KECS_DETAIL_FIELD(Type, a) KECS_DETAIL_EACH_11(Type, __VA_ARGS__)
#define KECS_DETAIL_COUNT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, N, ...) N
#define KECS_DETAIL_EACH(Type, ...) \
    KECS_DETAIL_COUNT(__VA_ARGS__, KECS_DETAIL_EACH_12, KECS_DETAIL_EACH_11, KECS_DETAIL_EACH_10, KECS_DETAIL_EACH_9, KECS_DETAIL_EACH_8, \
                      KECS_DETAIL_EACH_7, KECS_DETAIL_EACH_6, KECS_DETAIL_EACH_5, KECS_DETAIL_EACH_4, KECS_DETAIL_EACH_3, KECS_DETAIL_EACH_2, \
                      KECS_DETAIL_EACH_1)(Type, __VA_ARGS__)

/**
 * Declares up to twelve members of @p Type as its fields, each named as the member is. At namespace scope, outside
 * any namespace. More than twelve, or other names, are declared by specialising kecs::Fields<Type> by hand.
 */
#define KECS_FIELDS(Type, ...) \
    template <> struct kecs::Fields<Type> { \
        static void Describe(kecs::FieldList<Type>& list) { KECS_DETAIL_EACH(Type, __VA_ARGS__) } \
    };
