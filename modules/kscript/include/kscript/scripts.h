//
// kscript: scripts, as the engine sees them. A script type is a component whose data the world keeps and whose
// behaviour is a set of functions called on each one: when it is made, each frame while it is switched on, when it
// goes. Which language the functions are written in is nothing to this — C#, Kotlin and a test all hand it plain
// function pointers.
//
// It needs kecs and nothing else.
//

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <map>
#include <vector>

#include <kecs/kecs.h>

namespace kscript
{
    /**
     * @brief What is called on each component of a script type. Any may be null. @p data is the component's bytes,
     *        where the world keeps them now: good for the call, and no longer.
     */
    struct Callbacks {
        void* user = nullptr;                                                       ///< Handed back to each.
        void (*created)(void* user, kecs::Entity entity, void* data) = nullptr;     ///< It was added: once, switched on or off.
        void (*enabled)(void* user, kecs::Entity entity, void* data) = nullptr;     ///< It was switched on, or made switched on.
        void (*started)(void* user, kecs::Entity entity, void* data) = nullptr;     ///< Before its first update: once.
        void (*fixedUpdate)(void* user, kecs::Entity entity, void* data, float step) = nullptr;
        void (*update)(void* user, kecs::Entity entity, void* data, float delta) = nullptr;
        void (*lateUpdate)(void* user, kecs::Entity entity, void* data, float delta) = nullptr;
        /// After every update and before anything is drawn: where a graphics script makes what the passes take.
        void (*prepare)(void* user, kecs::Entity entity, void* data) = nullptr;
        void (*disabled)(void* user, kecs::Entity entity, void* data) = nullptr;    ///< It was switched off, or is about to go while on.
        void (*destroyed)(void* user, kecs::Entity entity, void* data) = nullptr;   ///< It is about to go: removed, or its entity destroyed.
    };

    /**
     * @brief A resource a ResourceScript makes, as it says: a field of one of its resource types — `Image`, `Buffer`,
     *        `ImageView`, `BufferView`, `Sampler` — and what it is made with.
     */
    struct DeclaredResource {
        std::string name;           ///< The field: what the Script node's output, and its inputs for it, are called.
        std::string kind;           ///< "Image", "Buffer", "ImageView", "BufferView", "Sampler".
        /// What it is made with, by its node type's inputs — `format`, `extent.x`, `usage`, `image` — each as JSON:
        /// `"eR32_SFLOAT"`, `256`, `["eSampled", "eStorage"]`, `"height"` (another of the script's resources).
        std::vector<std::pair<std::string, std::string>> settings;
    };

    /** @brief A script type: the component it is, and what is called on it. */
    struct ScriptType {
        std::string name;                           ///< The component's name: what it is saved and matched by.
        std::size_t size = 0;                       ///< Of its data: plain bytes, copied as they are. 0 is a script with no fields.
        std::size_t alignment = 8;
        std::vector<kecs::FieldDescription> fields; ///< By name and type: what the inspector shows, a file holds and a reload keeps.
        /// What a new one starts as, `size` bytes: what its source initialises its fields to. Empty, every field is zero —
        /// or, for a type registered again without one, what it started as before, field by field.
        std::vector<std::byte> initial;
        /// What each of its text fields — a `string`, kept where the field is as a std::string — starts as, by its name: what
        /// its source says, or nothing. A text field's bytes in `initial` are none of it.
        std::map<std::string, std::string, std::less<>> texts;
        Callbacks callbacks;
        /// What it takes besides Setup, as its source says (`PerFrame`, `MeshGenerator`): what an editor offers it for.
        std::vector<std::string> pieces;
        /// A ResourceScript: no component, but what a Script node of the graph runs — it makes resources and fills them.
        bool makesResources = false;
        std::vector<DeclaredResource> resources;    ///< A ResourceScript's, in the order it declares them.
        /// A Material: a kind of what a drawn thing is made of, data and nothing else. No component of the Scripts': its
        /// pictures are shared references, which what keeps them makes it with (kgraph::Runtime::DeclareMaterialKind).
        bool material = false;
        /// A Material's pictures, each a field of type "Image" at the front of it, by name: what each starts as (`$white`,
        /// `@textures/ramp.png`), in the order they are laid out.
        std::vector<std::pair<std::string, std::string>> textures;
        /// Data: structured data and nothing else — a Data's, or a Material's. Nothing is called on it, no host runs it,
        /// and it takes no pieces: what it is for is the graph's, which reads it.
        bool data = false;
        /// An abstract Data: a kind (kecs::ComponentDescription::abstract). No entity has it itself; one is of it by having
        /// one of what derives from it, and a query for it finds that one. Its fields are those at the front of each.
        bool abstract = false;
        /// The abstract Data it derives from, by their components' names, nearest first: the kinds it is one of.
        std::vector<std::string> kinds;
        /// The file its class is declared in, as it was read (kscript::DeclareFiles); empty for one read from a string.
        std::string file;
    };

    class Scripts {
    public:
        /** @brief Runs scripts on the components of @p world, which must outlive it. */
        explicit Scripts(kecs::World& world);
        ~Scripts();
        Scripts(const Scripts&) = delete;
        Scripts& operator=(const Scripts&) = delete;

        /**
         * @brief Makes @p type one of the world's script types, or — when there is one so named — gives it its new
         *        shape and its new functions: a script edited and loaded again. Entities keep the fields whose name
         *        and type are the same (kecs::World::Reshape); a component that had started is not started again.
         * @param migration When not null, what became of the fields of a type that changed shape.
         * @return The component, or NoComponent (and why in @p error) when the name is a component's that is no script's.
         */
        kecs::ComponentId Register(ScriptType type, std::string* error = nullptr, kecs::Migration* migration = nullptr);
        /**
         * @brief Makes @p type a component of the world, or gives the one so named its new shape, as Register does —
         *        but as data only: nothing is called on it, and it is not Registered. What a script's source says of
         *        its component, before anything runs the script. Registered later, it is the same component.
         */
        kecs::ComponentId Declare(ScriptType type, std::string* error = nullptr, kecs::Migration* migration = nullptr);
        /**
         * @brief Stops calling anything on script type @p name: its code is going. The component type stays, and
         *        every entity keeps its data, for the type to be registered again. Nothing is told it was destroyed.
         */
        void Unregister(std::string_view name);
        /** @brief Whether @p name is a script type with its functions in place. */
        [[nodiscard]] bool Registered(std::string_view name) const;
        /** @brief Every script type with its functions in place, by name. */
        [[nodiscard]] std::vector<std::string> Types() const;

        /** @brief A steady step of the game: fixedUpdate on every script component that is switched on. */
        void FixedUpdate(float step);
        /** @brief A frame: created, enabled, disabled and started where they are due, then update. */
        void Update(float delta);
        void LateUpdate(float delta);
        /**
         * @brief After the updates, before anything is drawn: prepare, on every one switched on. Called whether or not
         *        the scene plays, so it creates, enables and starts nothing: a script is started by the first update.
         */
        void Prepare();
        /** @brief Before the world goes: disabled and destroyed on everything, as if each were removed. */
        void Shutdown();

        /**
         * @brief The tags an entity running a script is given for what the script does, each a component of no data that
         *        is not saved: `PerFrame` for one that updates (Update, LateUpdate), `FixedStep` for one called on the
         *        fixed step, `BeforeDraw` for one that prepares what is drawn. What a graph's Filter asks for, to find
         *        the entities something runs on; taken away when no script of the entity's does that any more.
         */
        static constexpr std::array<std::string_view, 3> Pieces { "PerFrame", "FixedStep", "BeforeDraw" };

    private:
        struct State {
            bool enabled = false, started = false;
        };
        struct Type {
            ScriptType description;
            kecs::ComponentId id = kecs::NoComponent;
            bool live = false;      // its functions are in place
            std::unordered_map<std::uint64_t, State> states;   // an entity that has it, once created was called
        };

        kecs::ComponentId Add(ScriptType type, bool live, std::string* error, kecs::Migration* migration);
        /** The tag a piece is (Pieces), made the first time it is asked for. */
        kecs::ComponentId PieceTag(std::string_view piece);
        /** Gives @p entity the tags of what @p type does. */
        void GiveTags(const Type& type, kecs::Entity entity);
        /** Takes away the tags no live script of each entity left behind does any more: what Gone could not, while the world was changing. */
        void TakeTags();
        /** Tells of what changed since it last looked: components made, switched on and off. Then the entities to run. */
        std::vector<kecs::Entity> Sync(Type& type);
        void Gone(Type& type, kecs::Entity entity);
        /** Each entity of each type that @p has says has something to call, told of its life first (Sync): @p call on each. */
        template <typename F, typename Has> void Each(F&& call, Has&& has);

        kecs::World& _world;
        std::vector<std::unique_ptr<Type>> _types;      // never erased: an observer of each is out there
        std::vector<kecs::Entity> _untagged;            // entities that lost a script, whose tags are looked at again
        std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    };
}
