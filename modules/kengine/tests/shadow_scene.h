// The gate's scene, made through the C interface as any language would make it: a ground, an occluder over it, and
// a light. The shadow pass draws what casts into a depth image from the light's camera; the lit pass draws the
// ground, dark where that image says the light was stopped. Nothing here says where the light's camera comes from:
// a graphics script writes it into the "light camera" buffer.

#pragma once

#include <cstdio>
#include <string>

#include <koral_engine.h>

namespace shadow
{
    inline bool s_ok = true;
    inline void Must(const bool ok, const char* what)
    {
        if (ok) return;
        s_ok = false;
        std::fprintf(stderr, "shadow scene: %s failed: %s\n", what, kengine_last_error());
    }
    inline void Node(KengineDocument* doc, const char* type, const char* id, const double x, const double y)
    {
        const char* made = kengine_node_add(doc, type, id, x, y);
        Must(made && std::string(made) == id, id);
    }
    inline void Set(KengineDocument* doc, const char* node, const char* input, const char* json) { Must(kengine_node_set_input_json(doc, node, input, json), input); }
    inline void Wire(KengineDocument* doc, const char* from, const char* node, const char* input, const char* output = nullptr)
    {
        Must(kengine_node_connect(doc, from, output, node, input), input);
    }

    /** The graph. True when every node built. */
    inline bool BuildGraph(KengineDocument* doc)
    {
        s_ok = true;
        // The light as a camera: one matrix, written by a graphics script.
        Node(doc, "Buffer", "light camera", 0, 0);
        Set(doc, "light camera", "instanceCount", "64");
        Set(doc, "light camera", "usage", "[\"eUniform\"]");

        // What the light sees: depth only.
        Node(doc, "Image", "shadow map", 0, 120);
        Set(doc, "shadow map", "format", "\"eD32_SFLOAT\"");
        Set(doc, "shadow map", "extent.x", "64");
        Set(doc, "shadow map", "extent.y", "64");
        Set(doc, "shadow map", "extent.z", "1");
        Set(doc, "shadow map", "usage", "[\"eDepthStencilAttachment\", \"eSampled\"]");
        Node(doc, "ImageView", "shadow view", 200, 120);
        Wire(doc, "shadow map", "shadow view", "image");
        Node(doc, "Framebuffer", "shadow target", 400, 120);
        Set(doc, "shadow target", "depthStencil.which", "\"Depth\"");
        Set(doc, "shadow target", "depthStencil.name", "\"depth\"");
        Wire(doc, "shadow view", "shadow target", "depthStencil.view");
        Set(doc, "shadow target", "depthStencil.depth", "1");

        Node(doc, "Primitive", "square", 0, 300);
        Set(doc, "square", "shape", "\"eQuad\"");
        // A pipeline names its shaders: the module each is in, and the entry point.
        Node(doc, "GraphicsPipeline", "shadow", 400, 300);
        Set(doc, "shadow", "vertex.module", "\"shadow\"");
        Set(doc, "shadow", "vertex.entry", "\"castVertex\"");
        Wire(doc, "square", "shadow", "vertex.layout", "layout");
        Set(doc, "shadow", "fragment.module", "\"shadow\"");
        Set(doc, "shadow", "fragment.entry", "\"castFragment\"");
        Wire(doc, "shadow target", "shadow", "framebuffer");
        Set(doc, "shadow", "depthStencilState", "{\"depthTestEnable\": true, \"depthWriteEnable\": true, \"depthCompareOp\": \"eLess\", \"maxDepth\": 1}");
        Wire(doc, "light camera", "shadow", "param.light");
        // A draw for each entity with the pipeline's component and a Mesh: its mesh, and its own scale and offset.
        Node(doc, "ForEach", "casters", 600, 400);
        Set(doc, "casters", "component", "\"shadow\"");
        Set(doc, "casters", "with", "[\"Mesh\"]");
        Node(doc, "Draw", "cast", 600, 300);
        Wire(doc, "shadow", "cast", "pipeline");
        Wire(doc, "casters", "cast", "mesh", "Mesh.mesh");
        Wire(doc, "casters", "cast", "push.scale", "field.scale");
        Wire(doc, "casters", "cast", "push.offset", "field.offset");

        // What is shown: the ground, lit or not.
        Node(doc, "Image", "canvas", 0, 600);
        Set(doc, "canvas", "format", "\"eRGBA8_UNORM\"");
        Set(doc, "canvas", "extent.x", "32");
        Set(doc, "canvas", "extent.y", "32");
        Set(doc, "canvas", "extent.z", "1");
        Set(doc, "canvas", "usage", "[\"eColorAttachment\", \"eTransferSrc\"]");
        Node(doc, "ImageView", "canvas view", 200, 600);
        Wire(doc, "canvas", "canvas view", "image");
        Node(doc, "Framebuffer", "target", 400, 600);
        Set(doc, "target", "color[0].name", "\"color\"");
        Wire(doc, "canvas view", "target", "color[0].view");
        Node(doc, "GraphicsPipeline", "lit", 400, 700);
        Set(doc, "lit", "vertex.module", "\"lit\"");
        Set(doc, "lit", "vertex.entry", "\"litVertex\"");
        Wire(doc, "square", "lit", "vertex.layout", "layout");
        Set(doc, "lit", "fragment.module", "\"lit\"");
        Set(doc, "lit", "fragment.entry", "\"litFragment\"");
        Wire(doc, "target", "lit", "framebuffer");
        Wire(doc, "light camera", "lit", "param.light");
        Wire(doc, "shadow view", "lit", "param.shadowMap");
        Node(doc, "ForEach", "grounds", 600, 800);
        Set(doc, "grounds", "component", "\"lit\"");
        Set(doc, "grounds", "with", "[\"Mesh\"]");
        Node(doc, "Draw", "draw", 600, 700);
        Wire(doc, "lit", "draw", "pipeline");
        Wire(doc, "grounds", "draw", "mesh", "Mesh.mesh");
        Wire(doc, "grounds", "draw", "push.scale", "field.scale");
        Wire(doc, "grounds", "draw", "push.offset", "field.offset");

        Node(doc, "Screen", "screen", 600, 600);
        Node(doc, "Blit", "show", 800, 600);
        Wire(doc, "canvas", "show", "source");
        Wire(doc, "screen", "show", "destination");

        for (const char* node : { "light camera", "shadow map", "shadow target", "shadow", "cast", "canvas", "target", "lit", "draw", "show" })
            Must(kengine_node_ok(doc, node), node);
        return s_ok;
    }

    struct Entities {
        KecsEntity ground, occluder, light;
    };

    /** The entities: the ground is drawn, the occluder casts, and the light is only a name for scripts to be put on. */
    inline Entities BuildEntities(KengineDocument* doc)
    {
        KecsWorld* world = kengine_document_world(doc);
        Entities made {};
        made.ground = kengine_entity_create(doc, "Ground");
        made.occluder = kengine_entity_create(doc, "Occluder");
        made.light = kengine_entity_create(doc, "Light");
        const auto give = [&](const KecsEntity entity, const char* component) {
            Must(kecs_add(world, entity, kecs_component_find(world, component), nullptr), component);
        };
        const auto field = [&](const KecsEntity entity, const char* component, const char* name, const char* json) {
            Must(kengine_field_set_json(doc, entity, component, name, json), name);
        };
        // The ground: the whole picture, at height 0. Drawn by the lit pass; it casts nothing.
        give(made.ground, "Mesh");
        field(made.ground, "Mesh", "mesh", "\"square\"");
        give(made.ground, "lit");
        field(made.ground, "lit", "scale", "2");
        field(made.ground, "lit", "offset", "[0, 0, 0]");
        // The occluder: a quarter the width, half way up. It casts, and is not itself drawn.
        give(made.occluder, "Mesh");
        field(made.occluder, "Mesh", "mesh", "\"square\"");
        give(made.occluder, "shadow");
        field(made.occluder, "shadow", "scale", "0.5");
        field(made.occluder, "shadow", "offset", "[0, 0, 0.5]");
        return made;
    }

    /**
     * The light as a camera, as a graphics script writes it: sixteen floats, a column after another. The light leans
     * by @p slant, so a point at height z is seen where the ground point (x + slant.x * z, y + slant.y * z) is, and
     * nearer the higher it is.
     */
    inline void LightCamera(const float slantX, const float slantY, float out[16])
    {
        const float matrix[16] { 1, 0, 0, 0,   0, 1, 0, 0,   slantX, slantY, -1, 0,   0, 0, 1, 1 };
        for (int i = 0; i < 16; ++i) out[i] = matrix[i];
    }
}
