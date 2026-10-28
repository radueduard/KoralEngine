#pragma once

/**
 * @file catalog.h
 * @brief Every kind of node there is, and the inputs and outputs each has: what an editor offers and draws ports for.
 *
 * Most come from the framework's builder descriptions, one node type a builder — Image, Sampler, Shader, the
 * pipelines — whose inputs are the builder's settings. The rest are the engine's own: values and what is worked
 * out from them, the screen, a reference to another node, and the passes that run.
 *
 * A pipeline node has more inputs than are listed here, which depend on its shaders: `param.<name>` for each
 * descriptor they declare and `constant.<name>` for each specialization constant. Runtime::PortsOf gives those.
 */

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "value.h"

namespace kor::builders { struct Description; struct Type; }

namespace kgraph
{
    /** @brief Where an input is set. */
    enum class Placement : std::uint8_t {
        eInspector, ///< In the inspector: what takes room to set — a list, text, a choice of many, a struct.
        eNode,      ///< On the node, a control in its row, while nothing is wired to it: a number, a colour, a box to tick.
        eWire,      ///< Wired only, never typed: from a node that sets it up (Port::companion), or from what works one out.
    };

    /** @brief The kind of number a value is made of, as a shader or a builder takes it. */
    enum class Scalar : std::uint8_t {
        eNone,      ///< Not said: what a Math node takes, it takes of any kind.
        eFloat,     ///< 32 bits, as a shader's float.
        eDouble,
        eInt,       ///< A whole number, as a 32-bit int.
        eUInt,      ///< A whole number of none or more, as a 32-bit uint.
        eBool,
    };

    /**
     * @brief Exactly what a value is: the kind of its numbers, and how many — a Float's one, a Vec3's three, a shader's
     *        float4 four. What an editor draws an input with (four drags of a float4, a whole number's dragged a unit at a
     *        time) and makes what is set of (Conform), whatever the value it holds now is.
     */
    struct Shape {
        Scalar scalar = Scalar::eNone;
        std::uint8_t components = 1;    ///< 1 to 4.

        [[nodiscard]] bool Known() const { return scalar != Scalar::eNone; }
        [[nodiscard]] bool Whole() const { return scalar == Scalar::eInt || scalar == Scalar::eUInt; }
        bool operator==(const Shape&) const = default;
    };
    /** @brief The shape a value node's type is, or a Random's `type` says: "Float", "UInt", "Vec3", "IVec2", "Bool"; none for another name. */
    [[nodiscard]] Shape ShapeNamed(std::string_view name);
    /** @brief What ShapeNamed takes for @p shape: "Float", "Vec3", "UVec4"; empty for one not known. */
    [[nodiscard]] std::string ShapeName(Shape shape);
    /**
     * @brief @p value as @p shape is: each number made its kind — a float's to a float's precision, a whole one rounded to the
     *        nearest and kept within what an int or a uint holds, a bool's 0 or 1 — and as many as it has, a number for every
     *        component (a vector cut short or made up with zeros). What is not a number or a list of them, and any value
     *        for a shape not known, is as it is.
     */
    [[nodiscard]] Value Conform(const Value& value, Shape shape);

    enum class PortKind : std::uint8_t {
        eValue,     ///< A Value: given on the node, or wired from a node that works one out.
        eResource,  ///< A GPU object: always wired, from the node that makes it.
    };

    struct Port {
        std::string name {};
        PortKind kind = PortKind::eValue;
        /// What it takes or gives. A value: "bool", "number", "string", "vector", "enum kor::Image::Format",
        /// "flags kor::Image::Usage", "struct KoralRasterizationState". A resource: "Image", "Shader", or
        /// "Resource" for any.
        std::string type {};
        bool repeatable = false;    ///< Given any number of times: `color[0].view`, `color[1].view`.
        std::string doc {};
        const kor::builders::Type* builder = nullptr;   ///< The builder argument it is, for a builder node's input.
        /// Not one of the few a node of this type cannot do without: an editor shows it once it is asked for, or given.
        bool optional = false;
        std::vector<std::string> choices {};   ///< When it is one of a few names: what they are.
        /// What kind of thing a value is, beyond its type, for an editor to offer the right control: "color",
        /// "module" (a shader module), "entry" (an entry point of the module beside it), "node" (a node's id),
        /// "components" (component names), "parameter" (one of a pipeline's image parameters), "attributes".
        std::string editor {};
        /// The setting of the node's `stages` input it belongs to ("eVertex", "eMesh"); empty when it is there whatever that is.
        std::string mode {};
        /// Where an input is set: on the node, in the inspector, or only by a wire. A node can move one of the first two
        /// to the other (Node::placed); one wired only stays so.
        Placement place = Placement::eInspector;
        /// The node type that sets a wired-only input up — a VertexLayout, for a pipeline's `vertex.layout` — for an
        /// editor to make one beside it, wired in. Empty when nothing in particular does.
        std::string companion {};
        /// What a node starts with for it, when it is one a node cannot do without; what it is when it is not given
        /// otherwise; and, for a struct, what the fields that are not given are. Null when there is nothing to say.
        Value initial {};
        /// The least and the most a number of it is — each of a vector's, each part of one given in parts — as an editor
        /// lets it be set: an extent at least 1, a count no less than none, a field of view between 1 and 179 degrees.
        float min = -std::numeric_limits<float>::infinity(), max = std::numeric_limits<float>::infinity();
        /// Dragged by so much of what it is, not by so much: a size, a distance, a count that may be 2 or 20 000.
        bool logarithmic = false;
        /// A whole number — a count, an index, a builder's integer, an Int's — dragged and typed a unit at a time.
        bool integer = false;
        /// Exactly what it takes or gives, when that is said: a Float's, a shader's float4, a builder's uint. Not known for
        /// what takes any kind (a Math node's `a`), for what is not numbers, and for a whole of parts (`extent`).
        Shape shape {};
        /// The part of an editor's inspector it is in: "Indirect", "Depth", "rasterizationState", a Script node's
        /// resource. Empty: among the few that are always there, at the top.
        std::string group {};
        /// When it does anything: while the node's input `input` is one of `values` — a Draw's `indirectOffset` for the
        /// indirect commands, a Sampler's `maxAnisotropy` while `anisotropyEnable` is true. What an editor need not
        /// show until it does. `what` says when, for an editor to say what it is not showing: "for indirect draws".
        struct Condition {
            std::string input {};
            std::vector<Value> values {};
            std::string what {};
        } when {};
    };

    /**
     * @brief How abstract a node is: what one works at, placing it — from a number to a whole way of drawing. Each
     *        level is made of those below it, as a technique is of passes, a pass of resources, a resource of values.
     */
    enum class Level : std::uint8_t {
        eValue = 0,         ///< A value worked out on the CPU: a number, a vector, a matrix, a camera's lens; and what routes them (Switch, Bundle).
        eResource = 1,      ///< A GPU object: an image, a buffer, a sampler, a view, a pipeline, a mesh.
        eCommand = 2,       ///< One thing done on the GPU: a draw, a dispatch, rays traced, a copy, a clear.
        eScene = 3,         ///< What is read of the scene's entities, and assembled of them: who, where, their meshes packed, a scene for rays.
        eTechnique = 4,     ///< A way of drawing or working, whole: a function's call — bloom, a sort, the sun's shadows.
    };
    /** @brief What a level is called, for an editor: "value", "resource", "command", "scene", "technique". */
    [[nodiscard]] std::string_view LevelName(Level level);

    struct NodeType {
        std::string name {};
        std::string category {};       ///< "Resources", "Pipelines", "Values", "Passes".
        std::vector<Port> inputs {};
        std::vector<Port> outputs {};
        std::string doc {};
        const kor::builders::Description* builder = nullptr;    ///< For a builder node.
        /// Whether an editor offers it. A Shader is not: a pipeline names its shaders itself. One still loads and runs.
        bool listed = true;
        /// Other words someone looking for it might type: "texture" for an Image, "mix" for a Lerp.
        std::string keywords {};
        /// What an editor draws in its title bar: a Material icon, by name ("image", "functions").
        std::string icon {};
        /// How abstract it is (Level).
        Level level = Level::eValue;
        /// Made of other nodes: a function's call, written out as the function's nodes before the graph runs.
        bool composite = false;
    };

    struct Node;
    /**
     * @brief The inputs of @p type that a node set up as @p node is has: all of them, but for those that belong to
     *        another choice of its `stages` (a graphics pipeline drawn with mesh shaders has no vertex shader).
     */
    [[nodiscard]] std::vector<Port> PortsFor(const NodeType& type, const Node& node);
    /**
     * @brief The inputs @p whole stands for when it is one given in parts (`groups` → `groups.x`, `.y`, `.z`); none
     *        when it is not such an input. A node gives the whole or its parts, not both.
     */
    [[nodiscard]] std::vector<std::string> PartsOf(const NodeType& type, std::string_view whole);
    /** @brief Where @p node 's input @p port is set: its type's placement, or the node's own, where it may say otherwise. */
    [[nodiscard]] Placement PlacementOf(const Port& port, const Node& node);
    /** @brief What a graphics pipeline node's `stages` is: "eVertex" unless it says "eMesh". */
    [[nodiscard]] std::string StagesOf(const Node& node);

    /**
     * @brief Whether @p port of @p node does anything as the node is set: always, but for one whose Port::when is not so
     *        — of what the node is given for that input, or what it starts as. One whose input is neither given nor
     *        starts as anything might: it does.
     */
    [[nodiscard]] bool Applies(const Port& port, const Node& node);
    /** @brief Whether a pass node of @p type may run on the async compute queue: what records compute dispatches and copies only. */
    [[nodiscard]] bool RunsAsync(std::string_view type);

    /** @brief What an editor keeps a number within, and how it is dragged. */
    struct NumberLimits {
        float min = -std::numeric_limits<float>::infinity(), max = std::numeric_limits<float>::infinity();
        bool logarithmic = false;
    };
    /**
     * @brief What a number of a field of one of the builders' structs is kept within — a stencil's masks 0 to 255, a
     *        depth 0 to 1, a vertex attribute of 1 to 4 numbers — by the struct's name (`KoralStencilOpState`) and the
     *        field's (`compareMask`). Unbounded, for one nothing is said of.
     */
    [[nodiscard]] NumberLimits LimitsOf(std::string_view structName, std::string_view field);

    /**
     * @brief How a field of a component of type @p fieldType (`f32`, `vec3`, `quat`...) is laid out in a buffer, as a
     *        BufferBuilder's layout names it (`eFloat`, `eVec3`, `eVec4`...). Empty for what no buffer holds: a string, an asset.
     */
    [[nodiscard]] std::string_view LayoutTypeOf(std::string_view fieldType);
    /** @brief Where an entity is, as a ComponentBuffer holds it besides its component's fields: each name, and its layout type. */
    [[nodiscard]] const std::vector<std::pair<std::string_view, std::string_view>>& PlaceFields();
    /** @brief Which of PlaceFields a ComponentBuffer told nothing of its `fields` holds: worldPosition, forward and matrix. */
    [[nodiscard]] bool PlacedByDefault(std::string_view name);

    /** @brief Every node type. */
    [[nodiscard]] const std::vector<NodeType>& Catalog();
    /** @brief The node type so named, or null. */
    [[nodiscard]] const NodeType* FindType(std::string_view name);
}
