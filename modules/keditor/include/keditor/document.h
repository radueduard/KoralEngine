//
// keditor: a scene being worked on. Its pass graph and its entities, the runtime that runs them, and every change an
// editor makes to either, each applied at once so the next frame shows it. Saved and loaded as one JSON file.
//
// It knows nothing of an interface: the editor's panels call it, and so does a test, or a program that only runs a
// scene.
//

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <kecs/kecs.h>
#include <kecs/serialize.h>
#include <kgraph/graph.h>
#include <kgraph/runtime.h>
#include <kscript/scripts.h>

namespace kor { class FrameGraph; }

namespace keditor
{
    /**
     * @brief What an entity was called in a scene saved before a name was the world's own: the component "Name".
     *        Known only so that such a file still reads; a name it holds is given to the world, and it is taken off.
     */
    struct Label {
        std::string text;
    };

    class Document {
    public:
        /** @brief An empty scene, whose passes run in @p frameGraph. It must outlive the document. */
        explicit Document(kor::FrameGraph& frameGraph);
        ~Document();
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;

        [[nodiscard]] kecs::World& World() { return _world; }
        [[nodiscard]] const kecs::World& World() const { return _world; }
        [[nodiscard]] kgraph::Runtime& Runtime() { return *_runtime; }
        [[nodiscard]] const kgraph::Runtime& Runtime() const { return *_runtime; }
        [[nodiscard]] const kgraph::Graph& Graph() const { return _runtime->Current(); }
        /** @brief The scripts run on its entities. Whoever runs the scene calls their updates (PlayScene does). */
        [[nodiscard]] kscript::Scripts& Scripts() { return _scripts; }
        /** @brief The document whose passes run in @p frameGraph, or null: how code handed a scene finds what runs in it. */
        [[nodiscard]] static Document* Of(const kor::FrameGraph* frameGraph);
        /**
         * @brief Makes Of(@p other) the document of @p frameGraph too, until @p frameGraph is null: an editor's own
         *        scene answering for the scene it edits, whichever document that scene has at the time.
         */
        static void Alias(const kor::FrameGraph* other, const kor::FrameGraph* frameGraph);

        /** @brief Each frame, before the frame graph runs. */
        void Update();
        /** @brief Goes up with every change: what an interface built from the document watches. */
        [[nodiscard]] std::uint64_t Revision() const { return _revision; }
        /**
         * @brief Goes up when anything there is to show of it changed: the document, or what the runtime says of it —
         *        which entities a pass runs for and which it passes by, known only the frame after the change.
         */
        [[nodiscard]] std::uint64_t Version() const { return _revision + _reported; }
        /** @brief Whether it changed since it was last saved or loaded. */
        [[nodiscard]] bool Dirty() const { return _revision != _saved; }
        /** @brief What happened that the user should hear of, oldest first: an input an edited shader took away. */
        [[nodiscard]] const std::vector<std::string>& Notes() const { return _notes; }

        // ---- the graph ----------------------------------------------------------------------------------------

        /**
         * @brief Adds a node of @p type. Its id is @p id when given and free, and otherwise made from the type:
         *        "image", "image 2". A pipeline's id is what its entities' component is named, so it is worth giving.
         * @return The id it got; empty when there is no such type.
         */
        std::string AddNode(std::string_view type, double x, double y, std::string_view id = {});
        /** @brief Takes nodes out, with every wire to them. */
        void RemoveNodes(const std::vector<std::string>& ids);
        void MoveNodes(const std::vector<std::string>& ids, double dx, double dy);
        bool SetTitle(std::string_view node, std::string title);
        /** @brief Sets input @p input of @p node to a value, in place of whatever it was. False when there is no such node. */
        bool SetInput(std::string_view node, std::string_view input, kgraph::Value value);
        /** @brief Wires input @p input of @p node to another node's output, in place of whatever it was. */
        bool Connect(std::string_view fromNode, std::string_view output, std::string_view node, std::string_view input);
        /** @brief Takes an input off a node: its wire, or its value. */
        bool ClearInput(std::string_view node, std::string_view input);
        /** @brief Takes away every wire that leaves @p output of @p node. How many there were. */
        std::size_t DisconnectOutput(std::string_view node, std::string_view output);
        /** @brief Makes @p graph the whole graph. */
        void SetGraph(kgraph::Graph graph);

        // ---- entities -----------------------------------------------------------------------------------------

        /** @brief A new entity so called: last under @p parent, or at the top. */
        kecs::Entity CreateEntity(std::string_view name = "Entity", kecs::Entity parent = kecs::NullEntity);
        /** @brief Takes an entity away, and everything under it. */
        bool DestroyEntity(kecs::Entity entity);
        /** @brief Every entity, parents before their children. */
        [[nodiscard]] std::vector<kecs::Entity> Entities() const;
        [[nodiscard]] std::string NameOf(kecs::Entity entity) const;
        bool Rename(kecs::Entity entity, std::string name);
        /**
         * @brief Moves @p entity under @p parent (to the top, for NullEntity), at @p index among what is there (last
         *        when below 0). It stays where it is in the world. False when it would go under itself.
         */
        bool SetParent(kecs::Entity entity, kecs::Entity parent, int index = -1);
        /** @brief Where an entity is, how it is turned (Euler angles, in degrees) and how big, relative to what it is under. */
        bool SetPosition(kecs::Entity entity, const kecs::Vec3& position);
        bool SetRotation(kecs::Entity entity, const kecs::Vec3& degrees);
        bool SetScale(kecs::Entity entity, const kecs::Vec3& scale);

        /** @brief A tag: a component with nothing in it, for a node's `with` and `without`. The one so named, when there is one. */
        kecs::ComponentId AddTag(std::string_view name);
        /** @brief Every component type an entity can be given, by name: pipelines' components, tags, Mesh. */
        [[nodiscard]] std::vector<std::string> ComponentNames() const;
        /** @brief The components @p entity has, by name, but for its Name and its place in the hierarchy. */
        [[nodiscard]] std::vector<std::string> ComponentsOf(kecs::Entity entity) const;
        bool AddComponent(kecs::Entity entity, std::string_view component);
        bool RemoveComponent(kecs::Entity entity, std::string_view component);
        bool SetEnabled(kecs::Entity entity, std::string_view component, bool enabled);
        [[nodiscard]] bool IsEnabled(kecs::Entity entity, std::string_view component) const;

        /**
         * @brief A field of a component, as a value: a number, true or false, text, a list of numbers for a vector,
         *        and for a resource the id of the node that makes it (null when it holds nothing, or something
         *        no node made). Null as well for a field of a type that cannot be shown.
         */
        [[nodiscard]] kgraph::Value Field(kecs::Entity entity, std::string_view component, std::string_view field) const;
        /** @brief Sets one. False, with @p error saying why, when there is no such field or the value is not one it takes. */
        bool SetField(kecs::Entity entity, std::string_view component, std::string_view field, const kgraph::Value& value, std::string* error = nullptr);
        /** @brief The nodes whose output a resource field of type @p fieldType can hold, by id. */
        [[nodiscard]] std::vector<std::string> NodesFor(std::string_view fieldType) const;

        // ---- the file -----------------------------------------------------------------------------------------

        /** @brief The scene as JSON: its graph and its entities. */
        [[nodiscard]] std::string ToJson() const;
        /** @brief Makes the scene what @p json holds. For a document with nothing in it yet. */
        bool FromJson(std::string_view json, std::string& error);
        bool Save(const std::filesystem::path& path, std::string& error);
        bool Load(const std::filesystem::path& path, std::string& error);

    private:
        void Changed() { ++_revision; }
        void Apply(kgraph::Graph graph);

        kecs::World _world;                         // before the runtime, which is gone first
        kscript::Scripts _scripts { _world };
        const kor::FrameGraph* _frameGraph = nullptr;
        std::unique_ptr<kgraph::Runtime> _runtime;
        kecs::Codecs _codecs;
        std::vector<std::string> _tags;
        std::vector<std::string> _notes;
        std::uint64_t _revision = 0, _saved = 0;
        std::uint64_t _reported = 0;
        std::size_t _reports = 0;       // a digest of every pass's report, as last seen
    };

    /** @brief Whether a component field of this type is one Document::Field can show and SetField can set. */
    [[nodiscard]] bool Editable(std::string_view fieldType);
    /** @brief Whether a component field of this type holds a resource. */
    [[nodiscard]] bool HoldsResource(std::string_view fieldType);
}

template <> struct kecs::Name<keditor::Label> { static constexpr std::string_view value = "Name"; };
template <> struct kecs::Fields<keditor::Label> {
    static void Describe(kecs::FieldList<keditor::Label>& list) { list.Field("text", &keditor::Label::text); }
};
