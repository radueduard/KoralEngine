//
// keditor: a scene being worked on. Its pass graph and its entities, the runtime that runs them, and every change an
// editor makes to either, each applied at once so the next frame shows it. Saved and loaded as one JSON file.
//
// It knows nothing of an interface: the editor's panels call it, and so does a test, or a program that only runs a
// scene.
//

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <kecs/kecs.h>
#include <kecs/serialize.h>
#include <kgraph/graph.h>
#include <limits>
#include <map>
#include <optional>

#include <kasset/database.h>
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

    /**
     * @brief On the entity over a prefab's instance: which prefab — its file, from the project's folder — and what of
     *        it this instance has made its own (Document::OverridesOf), a key a line.
     */
    struct PrefabLink {
        std::string prefab;
        std::string overrides;
        /// Placed by another prefab — this instance one of the entities of the instance it is under: which part of that
        /// one's prefab it is. -1 for one placed by hand.
        std::int32_t outer = -1;
    };
    /** @brief On every entity of a prefab's instance, the one over it too: which entity of the prefab it is. */
    struct PrefabPart {
        std::int32_t part = 0;
    };

    /**
     * @brief On an entity whose materials follow Material assets — or whose generator follows a `.kgen` — which asset each
     *        component of its follows, and what of them the entity has made its own (Document::MaterialOverridesOf):
     *        every other field is the asset's.
     */
    struct MaterialLink {
        std::string assets;     ///< "<kind> <asset>" a line: `PbrMaterial @sponza/Sponza.gltf#material:3`
        std::string overrides;  ///< "<kind>.<field>" a line
    };

    /**
     * @brief A Material kept in the project, which entities share: one of a model file's (`@sponza/Sponza.gltf#material:3`),
     *        a project's own file (`@materials/brass.kmat`), or one of the editor's (`material:Gold`). A field it does
     *        not say is as its kind starts it.
     */
    struct MaterialAsset {
        std::string id;
        std::string name;       ///< As it is shown: the file's name for it.
        std::string kind;
        std::vector<std::pair<std::string, kgraph::Value>> values;     ///< By field.
        /// A model's: what the file reported, before anything was changed of it here — what an edit is kept apart from.
        std::vector<std::pair<std::string, kgraph::Value>> reported;
    };

    /** @brief What a function says it is for, beside its graph in its file: what the list of nodes offers it under and finds it by. */
    struct FunctionAbout {
        std::string purpose;            ///< What it is for, as the list of nodes groups functions: "Physics", "Sort and scan".
        std::string description;        ///< What it does, in a sentence or two.
        std::vector<std::string> tags;  ///< More words it is found by: "blur", "prefix sum".
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
         *        which entities a pass runs for and which it passes by, known only the frame after the change. Not when
         *        an entity is only moved, turned or scaled (dragged, every frame of the drag): a change all the same
         *        (Revision, Dirty), but what shows where an entity is follows it by itself, and an interface built again
         *        for each frame of a drag, with thousands of entities, took most of the frame.
         */
        [[nodiscard]] std::uint64_t Version() const { return _revision - _moves + _reported; }
        /** @brief Whether it changed since it was last saved or loaded. */
        [[nodiscard]] bool Dirty() const { return _revision != _saved; }
        /** @brief Says whether it has changed since it was saved: a scene put back as it was, changed or not. */
        void SetDirty(const bool dirty) { _saved = dirty ? std::numeric_limits<std::uint64_t>::max() : _revision; }
        /** @brief What happened that the user should hear of, oldest first: an input an edited shader took away. */
        [[nodiscard]] const std::vector<std::string>& Notes() const { return _notes; }

        // ---- the graph ----------------------------------------------------------------------------------------

        /**
         * @brief Adds a node of @p type. Its id is @p id when given and free, and otherwise made from the type:
         *        "image", "image 2". A pipeline's id is what its entities' component is named, so it is worth giving.
         * @return The id it got; empty when there is no such type.
         */
        std::string AddNode(std::string_view type, double x, double y, std::string_view id = {}, std::string_view pass = {});

        // ---- passes -------------------------------------------------------------------------------------------
        //
        // A pass is a page of the graph: what it makes for itself, what it runs, and what it takes from the passes
        // before it. The order of the passes is the order they are drawn in, where what they use does not say.

        [[nodiscard]] const std::vector<std::string>& Passes() const { return Graph().passes; }
        /** @brief Whether @p pass is one of setup: its passes are recorded once, when they are built. @see kgraph::Graph::setup */
        [[nodiscard]] bool IsSetup(std::string_view pass) const { return Graph().IsSetup(pass); }
        /** @brief How often @p pass runs: once, on the fixed step, or every frame. @see kgraph::PassRate */
        [[nodiscard]] kgraph::PassRate RateOf(std::string_view pass) const { return Graph().RateOf(pass); }
        /**
         * @brief A new pass so named, at @p index among those of its @p rate — setup first, then fixed, then every frame
         *        — last of them when below 0. False when there is one already.
         */
        bool AddPass(std::string_view name, int index = -1, kgraph::PassRate rate = kgraph::PassRate::eFrame);
        /**
         * @brief Makes a pass one of @p rate, as near where it was as it can be: the first of its new rate's passes when
         *        it comes from an earlier rate (setup, then fixed, then every frame), the last when from a later one.
         */
        bool SetPassRate(std::string_view pass, kgraph::PassRate rate);
        /** @brief Gives a pass another name, and every node of it with it. */
        bool RenamePass(std::string_view pass, std::string_view name);
        /** @brief Moves a pass to @p index among them: earlier is drawn first. A pass of setup stays before those of every frame. */
        bool MovePass(std::string_view pass, int index);
        /** @brief Takes a pass away: with its nodes, or — @p keepNodes — leaving them in no pass. */
        bool RemovePass(std::string_view pass, bool keepNodes = false);
        /** @brief Puts a node in a pass; in none, for an empty name. */
        bool SetNodePass(std::string_view node, std::string_view pass);
        /**
         * @brief Puts nodes @p ids in @p pass — one that is not there yet is made, last — as one edit, and wires them
         *        as passes are wired: what they take from an earlier pass, and what a later one takes of them, through
         *        a Reference in the pass that takes it (one there already, or a new one); what is now in one pass
         *        with them, straight. A Reference left standing for a node of its own pass goes. False when none is a node.
         */
        bool MoveNodesToPass(const std::vector<std::string>& ids, std::string_view pass);
        /**
         * @brief Brings what node @p node makes into @p pass: a Reference to it there, which is wired from as the
         *        node itself would be. The Reference's id; empty when there is no such node.
         */
        std::string Import(std::string_view node, std::string_view pass, double x, double y);
        /**
         * @brief Makes the buffer a pipeline's shaders expect of one of its parameters: a ShaderBuffer node laid out
         *        as the shader declares that buffer — an input for each field of its struct, to wire the data to —
         *        beside the pipeline, in its pass, and wired to the parameter. `frame`, of a pipeline whose shader
         *        has `ConstantBuffer<FrameData> frame`, is a node with FrameData's fields.
         * @param parameter The parameter, by the name the shader gives it, with or without `param.` before it.
         * @return The node's id; empty, with @p error saying why, when the pipeline names no shader that declares
         *         such a buffer.
         */
        std::string MakeBufferFor(std::string_view pipeline, std::string_view parameter, std::string* error = nullptr);
        /**
         * @brief Sets where @p node 's input @p input is set: on the node (`kgraph::Placement::eNode`), or in the
         *        inspector — what its type says, or otherwise. One its type has wired only stays so: false.
         */
        bool SetPlacement(std::string_view node, std::string_view input, kgraph::Placement where);
        /**
         * @brief Makes the node that sets up @p node 's wired-only input @p input (its port's companion: a VertexLayout
         *        for a pipeline's `vertex.layout`), beside it on its page, and wires it in.
         * @return The new node's id; empty, with @p error saying why, when the input has no companion.
         */
        std::string MakeCompanion(std::string_view node, std::string_view input, std::string* error = nullptr);
        /** @brief The nodes of the passes before @p pass, and of none: what there is for it to import, by id. */
        [[nodiscard]] std::vector<std::string> Importable(std::string_view pass) const;
        /** @brief Takes nodes out, with every wire to them. */
        void RemoveNodes(const std::vector<std::string>& ids);
        void MoveNodes(const std::vector<std::string>& ids, double dx, double dy);
        bool SetTitle(std::string_view node, std::string title);
        /**
         * @brief How a model file's meshes are read: which attributes are left out (not held at all) and which buffers
         *        the rest are put in. @p file is the file as its meshes' names have it: `sponza.gltf` of
         *        `@sponza.gltf#3`. The default is no entry. An edit of the graph like any: taken back, and saved with
         *        the scene; the file is read again, and every entity holding its meshes holds the new ones.
         */
        void SetMeshImport(const std::string& file, kgraph::MeshImport import);
        [[nodiscard]] kgraph::MeshImport MeshImportOf(std::string_view file) const;
        /**
         * @brief How the scene's picture files are kept, and the packs of their channels (Graph::textureImports,
         *        texturePacks): an edit of the graph like any, taken back and saved with the scene. Each picture whose
         *        settings changed is read again, as it is now kept.
         */
        void SetTextureSettings(std::map<std::string, kgraph::TextureImport> imports, std::map<std::string, kgraph::TexturePack> packs);
        /** @brief What the default meshes have of their surface's directions (Graph::defaultMeshTangents): a change to undo. */
        void SetDefaultMeshTangents(kgraph::Tangents tangents);
        /**
         * @brief Puts copies of @p nodes — of this graph, or as they were copied from one — into the graph, the
         *        top-left of them all at (@p x, @p y): each by a name of its own, wired among themselves as they
         *        were, and to whatever else they were wired from that this graph still has.
         * @param pass The pass they go into; null, each stays in its own. Into another pass than the copied ones were
         *             of, they are wired as MoveNodesToPass wires: what they take of other passes, through References.
         * @return The copies' ids, in the order of @p nodes.
         */
        std::vector<std::string> PasteNodes(std::vector<kgraph::Node> nodes, double x, double y, const std::string* pass = nullptr);
        /** @brief Copies of nodes @p ids beside them, (@p dx, @p dy) away. @see PasteNodes */
        std::vector<std::string> DuplicateNodes(const std::vector<std::string>& ids, double dx = 40, double dy = 40);
        /** @brief Puts each node where @p where says, in one go. */
        void PlaceNodes(const std::map<std::string, std::pair<double, double>>& where);
        /**
         * @brief Where each node is when every node is seen at once: a layout of its own, not its pass's page. Until a
         *        node is put somewhere there, it is where it is on its page, the passes' pages side by side in the
         *        order they are drawn (what is of no pass first) so that none is over another.
         */
        [[nodiscard]] std::map<std::string, std::pair<double, double>> OverviewPlaces() const;
        /** @brief Puts each node where @p where says when every node is seen at once; the rest stay where they are seen. */
        void PlaceInOverview(const std::map<std::string, std::pair<double, double>>& where);
        /** @brief Moves nodes @p ids by (@p dx, @p dy) where every node is seen at once. */
        void MoveInOverview(const std::vector<std::string>& ids, double dx, double dy);
        /**
         * @brief While true, a node made is made where every node is seen at once: where it is put is where it is
         *        there (and, at first, on its page). The graph panel says so while it shows every node.
         */
        void MakeInOverview(const bool overview) { _makeInOverview = overview; }
        [[nodiscard]] bool MakesInOverview() const { return _makeInOverview; }
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
        /**
         * @brief Gives @p entity picture @p id in its field @p field — of whichever of its components has a picture field
         *        so named (its material's `baseColorMap`). False when none of them has.
         */
        bool GivePicture(kecs::Entity entity, std::string_view field, const std::string& id);
        /**
         * @brief Gives @p entity the generator a `.kgen` keeps (@p file): its script as a component, with the fields the
         *        file has, which the entity follows from then on — but for what is set on the entity itself, as a
         *        material follows its asset. False, with why, when it is none.
         */
        bool GiveGenerator(kecs::Entity entity, const std::filesystem::path& file, std::string* error = nullptr);
        /** @brief Writes @p entity 's component @p script, its fields as they are, to @p file as a `.kgen`. */
        bool SaveGenerator(kecs::Entity entity, std::string_view script, const std::filesystem::path& file, std::string* error = nullptr);
        /** @brief What a `.kgen` keeps: its script, by name, and each of its fields. Empty script for what is none. */
        struct GeneratorAsset { std::string script; std::vector<std::pair<std::string, kgraph::Value>> fields; };
        [[nodiscard]] GeneratorAsset GeneratorOf(const std::filesystem::path& file) const;
        /** @brief Sets field @p field of a `.kgen`, in the file, and in every entity that follows it and has not made the field its own. */
        bool SetGeneratorField(const std::filesystem::path& file, const std::string& field, const kgraph::Value& value, std::string* error = nullptr);
        bool SetEnabled(kecs::Entity entity, std::string_view component, bool enabled);
        [[nodiscard]] bool IsEnabled(kecs::Entity entity, std::string_view component) const;

        /**
         * @brief A field of a component, as a value: a number, true or false, text, a list of numbers for a vector,
         *        and for a resource the id of the node that makes it (null when it holds nothing, or something
         *        no node made). Null as well for a field of a type that cannot be shown.
         */
        [[nodiscard]] kgraph::Value Field(kecs::Entity entity, std::string_view component, std::string_view field) const;
        /** @brief Sets one. False, with @p error saying why, when there is no such field or the value is not one it takes. */
        /** @brief What field @p field of component @p id of @p entity in @p world holds; what Field reads, of a world of no document. */
        [[nodiscard]] static kgraph::Value ReadField(const kecs::World& world, kecs::Entity entity, kecs::ComponentId id, std::string_view field);
        /**
         * @brief Writes @p value into field @p field of component @p id of @p entity in @p world: what SetField does to the
         *        bytes, and nothing of what it does to a document. A resource's id is made one by @p resolve, and what the
         *        field held is handed to @p retire, when given. For a world of no document: a preview's.
         */
        static bool WriteField(kecs::World& world, kecs::Entity entity, kecs::ComponentId id, std::string_view field, const kgraph::Value& value,
                               const std::function<std::shared_ptr<kgraph::Resource>(const std::string&)>& resolve,
                               const std::function<void(std::shared_ptr<kgraph::Resource>)>& retire, std::string* error = nullptr);
        bool SetField(kecs::Entity entity, std::string_view component, std::string_view field, const kgraph::Value& value, std::string* error = nullptr);
        /** @brief The nodes whose output a resource field of type @p fieldType can hold, by id. */
        [[nodiscard]] std::vector<std::string> NodesFor(std::string_view fieldType) const;

        // ---- entities as data ---------------------------------------------------------------------------------

        /**
         * @brief An entity and everything under it, as a value: its `name`, its `position`, `rotation` and `scale`,
         *        its `components` by name (each a field's value by the field's name), what is `disabled`, and its
         *        `children`. What an object file holds, and what a copy is made from.
         */
        [[nodiscard]] kgraph::Value WriteEntity(kecs::Entity entity) const;
        /**
         * @brief Makes an entity, and everything under it, from such a value: last under @p parent, or at the top.
         *        What it names that the scene does not have — a component, a field — is left out and said in
         *        @p problems.
         */
        kecs::Entity ReadEntity(const kgraph::Value& data, kecs::Entity parent = kecs::NullEntity, std::vector<std::string>* problems = nullptr);
        /** @brief A copy of @p entity and what is under it, beside it, under a name of its own. */
        kecs::Entity Duplicate(kecs::Entity entity);
        /** @brief @p name, or with a number after it when one of @p siblings has it: "Cube", "Cube 2". */
        [[nodiscard]] static std::string UniqueName(const std::string& name, const std::vector<std::string>& siblings);

        // ---- functions -------------------------------------------------------------------------------------------
        //
        // A function is nodes as one, kept in the project — `functions/<name>.kfunc` — and called by Function nodes:
        // each call is written out into its nodes before the graph runs, in the call's pass. FunctionInput and
        // FunctionOutput nodes are its edges: a port of every call each, top to bottom as they are on its page.

        /** @brief Where the project's functions are: its `functions` folder. Empty for a scene in no project. */
        [[nodiscard]] std::filesystem::path FunctionsDirectory() const;
        /**
         * @brief Where the engine's functions are, in every project: its package's `functions` folder — Scan,
         *        RadixSort, BitonicSort, OddEvenSort, Compact, IndirectArgs. A project's function of the same name is called in its place, and one
         *        of the engine's opened and changed is saved as the project's own.
         */
        [[nodiscard]] static std::filesystem::path EngineFunctionsDirectory();
        /** @brief The functions there are to call, by name: the project's, its modules', and the engine's. */
        [[nodiscard]] std::vector<std::string> Functions() const;
        /** @brief What function @p name says it is for (its file's `purpose`, `description` and `tags`): the project's, or the engine's. */
        [[nodiscard]] FunctionAbout AboutFunction(std::string_view name) const;
        /** @brief Whether @p name is one of the engine's functions, and the project has none of its own so called. */
        [[nodiscard]] bool IsEngineFunction(std::string_view name) const;
        /**
         * @brief The file function @p name is read from: the project's, or — when it has none so called — the first of its
         *        modules' that has one, or the engine's. Empty when there is no such function.
         */
        [[nodiscard]] std::filesystem::path FunctionFile(std::string_view name) const;

        /**
         * @brief The folders of the modules the scene's project imports (Project::Imported), the nearest first: their
         *        scripts, shaders, functions and assets are the project's too. Found when the project is (SetAssetDirectory).
         */
        [[nodiscard]] const std::vector<std::filesystem::path>& Modules() const { return _modules; }
        /** @brief Finds what the project imports again — after its project.json changed — and declares and scans what that is. */
        void ReloadModules();
        /**
         * @brief What a node can be made as besides the types there are: the runtime's, a node for each component —
         *        and a call of each of the project's functions, by its name (or `<name> (function)`, where a type has it).
         */
        [[nodiscard]] std::vector<kgraph::Preset> Presets() const;
        /**
         * @brief Makes nodes @p ids a function called @p name — a file of the project's — and puts a call of it in their
         *        place, as one edit. Each wire into them is an input of it, each wire out of them an output, named for
         *        the port it went to or came from; the call is wired as they were. The call's id; empty, with @p error
         *        saying why, when it cannot be made.
         */
        std::string MakeFunction(const std::vector<std::string>& ids, const std::string& name, std::string* error = nullptr);
        /** @brief A function opened in place: the call it stands for, the function, and the pass its nodes are on. */
        struct OpenedFunction {
            kgraph::Node call;
            std::string function, pass, prefix;
        };
        /**
         * @brief Opens call @p call's function in place: its nodes on a page of their own — a pass after the call's —
         *        each FunctionInput given what the call was given, and what was wired from the call wired from its
         *        FunctionOutputs. What is done there is the function's. The page's pass; empty, with @p error saying why.
         */
        std::string OpenFunction(std::string_view call, std::string* error = nullptr);
        /**
         * @brief Closes the function opened: with @p keep, what its page holds now is written to its file — every call
         *        of it follows — and the call is put back, wired as its page's edges were; without, as it was opened.
         */
        bool CloseFunction(bool keep, std::string* error = nullptr);
        [[nodiscard]] const std::optional<OpenedFunction>& Opened() const { return _opened; }
        /** @brief Call @p call written out into its nodes, in its place: no longer a call of its function. */
        bool WriteOut(std::string_view call, std::string* error = nullptr);
        /** @brief Reads the project's functions again: one changed outside, every call of it follows. */
        void ReloadFunctions();

        // ---- prefabs ---------------------------------------------------------------------------------------------
        //
        // A prefab is a file of entities (`prefabs/<name>.prefab`, in the project) that instances are made of. Each
        // instance is made again from the prefab whenever the scene is opened, or the prefab changes (ApplyPrefab):
        // all of it but what the instance has made its own — a field set, a component added or taken off, an
        // entity under it taken away. Those are its overrides, kept with it, and Revert drops them. Where the
        // instance is, what it is called and what it is under are always its own. Entities put under an instance's
        // entities are its own as well. A prefab's instance under another's is part of the outer prefab once that is
        // applied: the outer prefab names the inner one, and what the inner instance made its own.

        /**
         * @brief Writes @p entity and everything under it to @p file as a prefab, and makes it the prefab's first
         *        instance. False, with @p error saying why, when the file cannot be written.
         */
        bool SaveAsPrefab(kecs::Entity entity, const std::filesystem::path& file, std::string* error = nullptr);
        /**
         * @brief The prefab a model's things placed in a scene are an instance of: made of the model, in the project's
         *        cache (`.koral/cache/prefabs/<the model's ID>.prefab`), and made again whenever the model is read again,
         *        so every place it was put follows the file — keeping what each made its own. Empty without a project.
         */
        [[nodiscard]] std::filesystem::path ModelPrefabOf(const std::filesystem::path& model) const;
        /** @brief Makes an instance of the prefab at @p file — a path from the project's folder, or a whole one — last under @p parent. */
        kecs::Entity Instantiate(const std::filesystem::path& file, kecs::Entity parent = kecs::NullEntity, std::string* error = nullptr);
        /** @brief The entity over the instance @p entity is of: itself, when it is that. NullEntity for one of no instance. */
        [[nodiscard]] kecs::Entity PrefabRootOf(kecs::Entity entity) const;
        /** @brief Which prefab an instance is of, as it names it: `prefabs/Crate.prefab`. Empty for no instance. */
        [[nodiscard]] std::string PrefabOf(kecs::Entity root) const;
        /** @brief What an instance has made its own, each as `<part>:<what>` — `3:position`, `0:lit.albedo`, `2:+Spin`, `4:-Mesh`, `5:gone`. */
        [[nodiscard]] std::vector<std::string> OverridesOf(kecs::Entity root) const;
        /** @brief Writes the instance as it is now to its prefab — overrides and all — and every other instance of it follows. */
        bool ApplyPrefab(kecs::Entity root, std::string* error = nullptr);
        /** @brief Drops what the instance made its own, and the entities put under it: it is as the prefab is again. */
        bool RevertPrefab(kecs::Entity root, std::string* error = nullptr);
        /** @brief Makes the instance an entity like any other: no prefab's, and nothing more made of one. */
        bool Unlink(kecs::Entity root);
        /**
         * @brief Makes every instance of the prefab at @p file — of every prefab, when none is given — again as the
         *        prefab is now, keeping what each has made its own. Load does it for the whole scene.
         */
        void UpdatePrefabs(const std::filesystem::path& file = {});
        /**
         * @brief Says that @p what of @p entity — `position`, `rotation`, `scale` — was changed by whoever changed it on
         *        the world itself (the gizmo): a change to it, and the instance's own when it is part of one.
         */
        void Moved(kecs::Entity entity, std::string_view what);
        /** @brief The project's folder: what a prefab's file is named from. Empty for a scene that is in none. */
        [[nodiscard]] std::filesystem::path ProjectDirectory() const;

        /** @brief Every entity as it is now: what RestoreWorld puts back. The graph is no part of it. */
        [[nodiscard]] std::string WorldJson() const;
        /** @brief Takes every entity away and makes those @p json holds: a scene put back as it was before it played. */
        bool RestoreWorld(std::string_view json, std::string& error);

        // ---- the file -----------------------------------------------------------------------------------------

        /** @brief The scene as JSON: its graph and its entities. */
        [[nodiscard]] std::string ToJson() const;
        /** @brief Makes the scene what @p json holds. For a document with nothing in it yet. */
        bool FromJson(std::string_view json, std::string& error);
        /**
         * @brief Makes entities of what a model file holds — a glTF, an obj, an fbx: an entity for each thing in
         *        it, placed as the file places it under one named for the file, each drawn part with its Mesh, and
         *        the file's lights. Every component a pipeline of the graph makes that has an `albedo` is given to
         *        each drawn entity and filled from its material, a field by name where there is one: `albedo`,
         *        `metallic`, `roughness`, `emission`, `alphaCutoff`, and the pictures `albedoMap`, `normalMap`,
         *        `roughnessMap` and `emissiveMap`. One with a cut-out has the tag `Masked`, and one drawn over what is
         *        behind it — glass, a decal — the tag `Blended`.
         *        The file is read off the frame: the entity over them all is there at once, and the rest is made
         *        under it when the file is read (Reading() counts down to it).
         * @return The entity over them all; NullEntity, with @p error saying why, when there is no such file.
         */
        kecs::Entity ImportModel(const std::filesystem::path& file, std::string* error = nullptr);
        /**
         * @brief How many files are still being read: models whose entities are yet to be made, and the meshes and
         *        pictures entities already name. All of it is read off the frame; the scene runs meanwhile, and what
         *        is drawn fills in as each arrives.
         */
        [[nodiscard]] std::size_t Reading() const { return _importing.size() + _toFollow.size() + _runtime->Reading(); }
        /**
         * @brief Says that a script was handed the resource node @p node makes — to write into, as a graphics script
         *        does. Another view of the graph (the editor's Game panel) shares those buffers rather than having its own.
         */
        void ScriptUses(std::string_view node) { _scriptResources.emplace(node); _runtime->ReachedFromTheCpu(node); }
        [[nodiscard]] const std::set<std::string>& ScriptResources() const { return _scriptResources; }
        /** @brief Where files named without their whole path are: a project's `assets` folder. Load and Save say it from the scene's own place. */
        /// The project's material kinds' Slang modules are written into its `cache/shaders`, beside it.
        void SetAssetDirectory(const std::filesystem::path& directory);
        /**
         * @brief Every asset of the project the asset directory is of: null when that is no project's (no `project.json`
         *        beside it). Found — each given a meta when it has none — when the directory is said. How a model's
         *        meshes are read and how a picture is kept are its meta's: a scene is given them when it is loaded, and
         *        they are written back to the metas, not the scene, when it is saved.
         */
        [[nodiscard]] kasset::Database* Assets() { return _assets.get(); }
        [[nodiscard]] const kasset::Database* Assets() const { return _assets.get(); }
        /** @brief What was wrong of the project's assets when they were found: a sentence each. */
        [[nodiscard]] const std::vector<std::string>& AssetProblems() const { return _assetProblems; }

        /**
         * @brief Makes the components the scripts under @p folder — or the one script file it is — declare known, as their
         *        sources declare them, read with the modules' scripts — a class may derive from one of a module's: what a
         *        project's scripts are, whether or not anything runs them. Load does it for the
         *        scene's project.
         */
        void DeclareScripts(const std::filesystem::path& folder);
        /** @brief The scripts declared that take @p piece (`MeshGenerator`), by their components' names: what can make a mesh. */
        [[nodiscard]] std::vector<std::string> ScriptsTaking(std::string_view piece) const;
        /**
         * @brief Makes a ResourceScript one a Script node can name, as its source declares it: what it makes, its fields,
         *        and what they start as. One a host runs already stays as the host made it.
         */
        void DeclareResourceScript(kscript::ScriptType type);
        /**
         * @brief Makes a Material's script a material kind of the scene: a component whose pictures start as it says,
         *        and a Slang module of it for shaders to import. DeclareScripts does it for each it finds.
         */
        void DeclareMaterialKind(kscript::ScriptType type);

        // ---- materials ----------------------------------------------------------------------------------------------

        /**
         * @brief Which material kind a model file's material reported as @p model is imported as: for every file, or —
         *        with @p file — for that one, before what every file is. By default PbrMaterial and BlinnPhongMaterial.
         *        An empty @p kind for a file takes its own away.
         */
        void SetImportKind(kgraph::ModelMaterial::Model model, std::string kind, const std::filesystem::path& file = {});
        [[nodiscard]] std::string ImportKind(kgraph::ModelMaterial::Model model, const std::filesystem::path& file = {}) const;
        /** @brief Makes @p asset one entities can follow, or puts it in the place of the one with its id. */
        void AddMaterialAsset(MaterialAsset asset);
        /**
         * @brief The asset of @p id. A model's is there once the model is read — from what it reports, and what was
         *        changed of it since (kept in `<model>.materials.json` beside it); a project's file is read when it is
         *        first asked for. Null for none.
         */
        [[nodiscard]] const MaterialAsset* MaterialAssetOf(std::string_view id);
        /** @brief Every asset known now, by id. */
        [[nodiscard]] std::vector<std::string> MaterialAssetIds() const;
        /**
         * @brief Makes a model's material @p id (`@ship.gltf#material:2`) one of the project's own: written to @p file (a
         *        `.kmat`) as it is now, edits and all, and every entity that followed it follows that, keeping what it made
         *        its own. The model's file read again changes it no more. Its id, or empty — and why in @p error — when
         *        @p id is no model's material or the file cannot be written.
         */
        std::string ExtractMaterial(std::string_view id, const std::filesystem::path& file, std::string* error = nullptr);
        /**
         * @brief What each field of material kind @p kind takes of what model @p file reports of its materials, where it
         *        does not take the value of its own name: field to the report's value (`tint` ← `albedo`). In the model's
         *        meta, as `materialFields`. Empty for a model of no project, or that says nothing of it.
         */
        [[nodiscard]] std::map<std::string, std::string> MaterialFieldsOf(const std::filesystem::path& file, std::string_view kind) const;
        /**
         * @brief Says what field @p field of kind @p kind takes of model @p file 's report: @p reported, or — empty — the
         *        report's value of its own name. Its materials are made again so, and every entity that follows one has
         *        what it did not make its own as they are now.
         */
        bool SetMaterialField(const std::filesystem::path& file, std::string_view kind, const std::string& field, const std::string& reported, std::string* error = nullptr);
        /** @brief What model material @p id 's file reports that none of its kind's fields takes: what is dropped. */
        [[nodiscard]] std::vector<std::string> UnusedReported(std::string_view id);
        /**
         * @brief Gives @p entity the material @p asset is — its kind, every field as the asset has it — following it from
         *        now on: what it had of that kind made its own is forgotten. False, with @p error saying why, for no asset.
         */
        bool GiveMaterial(kecs::Entity entity, std::string_view asset, std::string* error = nullptr);
        /** @brief The asset @p entity 's material of @p kind follows; empty for none. */
        [[nodiscard]] std::string MaterialAssetOfEntity(kecs::Entity entity, std::string_view kind) const;
        /** @brief The fields of @p entity 's material of @p kind it has made its own: set on it, not on its asset. */
        [[nodiscard]] std::vector<std::string> MaterialOverridesOf(kecs::Entity entity, std::string_view kind) const;
        /**
         * @brief Sets a field of an asset, and so of every entity's material that follows it and has not made that field
         *        its own. A model's is kept beside the model; a project's file is written.
         */
        bool SetMaterialAssetField(std::string_view asset, std::string_view field, const kgraph::Value& value, std::string* error = nullptr);
        /** @brief Writes what @p entity made its own of its @p kind material to its asset: every entity that follows it has it. */
        bool ApplyMaterial(kecs::Entity entity, std::string_view kind);
        /** @brief Forgets what @p entity made its own of its @p kind material: it is as its asset is. */
        bool RevertMaterial(kecs::Entity entity, std::string_view kind);
        /** @brief Stops @p entity 's @p kind material following its asset: what it has is its own. */
        bool UnlinkMaterial(kecs::Entity entity, std::string_view kind);
        /**
         * @brief Has @p entity 's material follow the one its mesh has in its model file, once the model is read; what
         *        it has of the fields a scene set before materials were their own (its colour, pictures, alpha) that is
         *        not as the file says is kept as its own. Nothing for an entity not drawn with a model's mesh, or whose
         *        material follows an asset already.
         */
        void FollowModelMaterial(kecs::Entity entity);
        /**
         * @brief Runs a ResourceScript: @p fill fills the images and buffers each Script node that names it makes, given
         *        that node's settings as @p type lays them out. What it makes is what @p type says, or — when it says
         *        nothing — what its source declares. Whatever it made is made and filled again.
         */
        void RegisterResourceScript(kscript::ScriptType type, std::function<std::string(std::span<kgraph::Contents>, std::span<const std::byte>)> fill);
        /** @brief Stops running a resource script: its code is going. It is declared still, and what it makes is made empty. */
        void UnregisterResourceScript(std::string_view name);
        /** @brief The scripts that fill resources, running or declared: what a Script node can name. */
        [[nodiscard]] std::vector<kgraph::ResourceScript> ResourceScripts() const;

        bool Save(const std::filesystem::path& path, std::string& error);
        bool Load(const std::filesystem::path& path, std::string& error);

    private:
        void Changed() { ++_revision; }
        /** A change that is only where an entity is: a change, but not one of Version (see there). */
        void Shifted() { ++_revision; ++_moves; }
        void Apply(kgraph::Graph graph);
        bool _makeInOverview = false;
        /** An instance's entity was changed: what, as an override of the prefab, kept on the instance. */
        void Override(kecs::Entity entity, std::string_view what);
        /** The instance under @p root made again from @p prefab (its file's "root"), keeping its overrides. */
        void Rebuild(kecs::Entity root, const kgraph::Value& prefab);
        /** The prefab an instance's entity is the root of, as a value: its entities, each with the "part" it is. */
        [[nodiscard]] kgraph::Value WritePrefab(kecs::Entity root);
        /** An override of @p part's @p what, kept on the instance @p root. */
        void Keep(kecs::Entity root, int part, std::string_view what);
        /** The instance's own entities, by which part of its prefab each is: not those of an instance under it, but such an instance itself. */
        [[nodiscard]] std::map<int, kecs::Entity> PartsOf(kecs::Entity root) const;
        int _rebuilding = 0;                        // instances being made again from their prefabs: nothing done to them is their own
        /** Gives @p graph the functions its calls name, read from the project's files — those inside them as well. */
        void WithFunctions(kgraph::Graph& graph);
        std::map<std::string, std::pair<std::filesystem::file_time_type, std::shared_ptr<const kgraph::Graph>>> _functionFiles;
        std::optional<OpenedFunction> _opened;
        kgraph::Graph _beforeOpened;                // the graph as it was when the function was opened: what Close without keeping puts back
        std::set<std::string> _scriptResources;     // the nodes whose resource a script was handed

        kecs::World _world;                         // before the runtime, which is gone first
        kscript::Scripts _scripts { _world };
        const kor::FrameGraph* _frameGraph = nullptr;
        std::unique_ptr<kgraph::Runtime> _runtime;
        std::uint32_t _reportsAsked = 0;     // frames since the start: Update's digest of the reports, every few
        std::unique_ptr<kasset::Database> _assets;
        std::vector<std::filesystem::path> _modules;           ///< Modules(): the folders of what the project imports.
        std::vector<std::string> _assetProblems;
        /** The import settings the project's metas hold, into @p graph: over what a scene saved before they were the assets' said. */
        void ImportSettingsInto(kgraph::Graph& graph) const;
        /** Every entity that follows a `.kgen` given what the file says now, but for what it made its own: a scene read, the file may have changed. */
        void FollowGenerators();
        /** The graph's import settings, into the metas of the assets they are of. */
        void StoreImportSettings();
        /**
         * A model's meshes and materials, as @p model has them, given their parts' IDs by name: one the file now has in
         * another place is the same part, and what holds it — an entity's Mesh, a material it follows — is led there.
         */
        void RecordModelParts(const std::filesystem::path& file, const kgraph::Model& model);
        /**
         * What a component an object, a prefab or a scene names is here: itself — or, for one that is a kind now (a
         * Camera), the one of it that what it holds says (kecs::Codecs::Becomes). Empty when it is a kind and nothing says.
         */
        [[nodiscard]] std::string Becomes(const std::string& component, const kgraph::Value& fields) const;
        kecs::Codecs _codecs;
        std::vector<std::string> _tags;
        std::vector<std::string> _notes;
        // Materials: the assets entities follow, by id; which kind each reported model is imported as, for every file
        // ("") and for one; and whether a field is being set because an asset has it, which makes nothing an override.
        std::map<std::string, MaterialAsset, std::less<>> _materialAssets;
        std::map<std::string, std::array<std::string, 2>, std::less<>> _importKinds { { "", { "PbrMaterial", "BlinnPhongMaterial" } } };
        bool _following = false;
        /** The entity's MaterialLink lines of @p which ("assets", "overrides"), and back. */
        [[nodiscard]] std::vector<std::string> LinkLines(kecs::Entity entity, bool overrides) const;
        void SetLinkLines(kecs::Entity entity, bool overrides, const std::vector<std::string>& lines);
        /** A model's materials as assets, from what it reports and what was changed of it since. */
        void MakeModelMaterials(const std::filesystem::path& file, const kgraph::Model& model);
        /** Writes what was changed of a model's materials beside it, or a project's material's file. */
        void SaveMaterialAsset(const MaterialAsset& asset);
        /** Tags @p entity Masked or Blended as its materials' alphaMode says. */
        void SyncAlphaTags(kecs::Entity entity);
        /** What a scene saved before materials were their own had in its pipelines' components, made each entity's PbrMaterial. */
        void MigrateMaterials(const kecs::json::Value& world, const std::vector<kecs::Entity>& entities);
        /** FollowModelMaterial, its model read. */
        void FollowedModel(kecs::Entity entity, const std::filesystem::path& file, const kgraph::Model& model);
        void Imported(const std::filesystem::path& file, kecs::Entity root, const kgraph::Model& model);
        std::vector<std::pair<std::filesystem::path, kecs::Entity>> _importing;     // a model being read, and the entity its things go under
        /// Models placed as instances of the prefab made of each (ModelPrefabOf), and the model each prefab was last made
        /// of: read again — written again, read with other settings — the prefab is made again, and its instances follow.
        std::map<std::filesystem::path, const kgraph::Model*> _modelPrefabs;
        /** Makes the things of @p read under @p root, placed as the file places them, each part its mesh and material, and the file's lights and cameras. */
        void BuildModel(const std::filesystem::path& file, kecs::Entity root, const kgraph::Model& read);
        /** Writes model @p file 's prefab again, of @p model as it is read now; when that changed it, every instance follows. */
        void RemakeModelPrefab(const std::filesystem::path& file, const kgraph::Model& model);
        std::set<std::filesystem::path> _partsToRecord;
        std::map<std::string, std::vector<std::string>> _scriptPieces;      // each script declared, by name: the pieces it takes     // models a scene loaded has parts of, whose parts are given IDs once each is read
        std::vector<std::pair<std::filesystem::path, kecs::Entity>> _toFollow;      // a model being read, and an entity to follow its material
        std::uint64_t _revision = 0, _saved = 0;
        std::uint64_t _moves = 0;                       // of _revision, the changes that were only an entity moved
        std::uint64_t _reported = 0;
        std::size_t _reports = 0;       // a digest of every pass's report, as last seen
    };

    /** @brief Whether a component field of this type is one Document::Field can show and SetField can set. */
    [[nodiscard]] bool Editable(std::string_view fieldType);
    /** @brief Whether a component field of this type holds a resource. */
    [[nodiscard]] bool HoldsResource(std::string_view fieldType);
    /** @brief An entity as a field of the type "entity" is said to hold one: Document::Field gives it so, SetField takes it. Null for none. */
    [[nodiscard]] kgraph::Value EntityValue(kecs::Entity entity);
    /** @brief The entity such a value is; NullEntity for null, nothing for what is not one. */
    [[nodiscard]] std::optional<kecs::Entity> EntityOfValue(const kgraph::Value& value);
}

template <> struct kecs::Name<keditor::Label> { static constexpr std::string_view value = "Name"; };
template <> struct kecs::Name<keditor::PrefabLink> { static constexpr std::string_view value = "Prefab"; };
template <> struct kecs::Name<keditor::PrefabPart> { static constexpr std::string_view value = "PrefabPart"; };
template <> struct kecs::Fields<keditor::PrefabLink> {
    static void Describe(kecs::FieldList<keditor::PrefabLink>& list)
    {
        list.Field("prefab", &keditor::PrefabLink::prefab);
        list.Field("overrides", &keditor::PrefabLink::overrides);
        list.Field("outer", &keditor::PrefabLink::outer);
    }
};
template <> struct kecs::Fields<keditor::PrefabPart> {
    static void Describe(kecs::FieldList<keditor::PrefabPart>& list) { list.Field("part", &keditor::PrefabPart::part); }
};
template <> struct kecs::Fields<keditor::Label> {
    static void Describe(kecs::FieldList<keditor::Label>& list) { list.Field("text", &keditor::Label::text); }
};
template <> struct kecs::Name<keditor::MaterialLink> { static constexpr std::string_view value = "MaterialLink"; };
template <> struct kecs::Fields<keditor::MaterialLink> {
    static void Describe(kecs::FieldList<keditor::MaterialLink>& list)
    {
        list.Field("assets", &keditor::MaterialLink::assets);
        list.Field("overrides", &keditor::MaterialLink::overrides);
    }
};
