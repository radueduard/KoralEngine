#include "runtime_impl.h"

namespace kgraph
{


    Resource::~Resource()
    {
        if (handle) koral_resource_release(handle);
    }



    // A graph's pipeline may have tessellation and geometry shaders: the device has them enabled where the GPU has them
    // (Context::Supports), as mesh shaders and ray tracing are already.
    KORAL_REQUEST_FEATURES(kor::Feature::eTessellationShader | kor::Feature::eGeometryShader);

    // ---- the runtime ---------------------------------------------------------------------------------------------



    Runtime::Runtime(kor::FrameGraph& frameGraph, kecs::World* world) : _impl(std::make_unique<Impl>(frameGraph, world))
    {
    }
    Runtime::Runtime(kor::FrameGraph& frameGraph, kecs::World* world, Runtime& source) : _impl(std::make_unique<Impl>(frameGraph, world))
    {
        _impl->source = source._impl->source ? source._impl->source : source._impl.get();
    }
    Runtime::~Runtime() = default;

    // A node that cannot be built keeps why (Status), and an entity that cannot be run is reported (ReportOf): the
    // framework's own account of each failure is kept out of the log, which a graph being edited would fill with it.

    void Runtime::Set(Graph graph)
    {
        const kor::log::Quiet quiet;
        _impl->WriteMaterialsNow();
        _graph = std::move(graph);
        ++_revision;
        _impl->SetMeshImports(_graph.meshImports);
        _impl->SetTextureImports(_graph.textureImports, _graph.texturePacks);
        _impl->SetDefaultMeshTangents(_graph.defaultMeshTangents);
        Run();
    }

    TexelLayout TexelLayoutOf(const std::string_view format)
    {
        // `eRGBA16_SFLOAT`: its channels, how many bits each, and what the bits are.
        TexelLayout layout;
        std::size_t at = format.starts_with('e') ? 1 : 0;
        int channels = 0;
        while (at < format.size() && std::string_view("RGBAD").find(format[at]) != std::string_view::npos) { layout.depth = format[at] == 'D'; ++channels; ++at; }
        int bits = 0;
        while (at < format.size() && std::isdigit(static_cast<unsigned char>(format[at]))) bits = bits * 10 + (format[at++] - '0');
        const std::string_view kind = at < format.size() ? format.substr(at + 1) : std::string_view();
        layout.kind = kind == "UNORM" || kind == "SRGB" ? 'u' : kind == "SNORM" ? 's' : kind == "UINT" ? 'U' : kind == "SINT" ? 'I' : kind == "SFLOAT" ? 'f' : 0;
        if (channels == 0 || layout.kind == 0 || (bits != 8 && bits != 16 && bits != 32)) return {};
        layout.channels = channels;
        layout.bits = bits;
        return layout;
    }


    void Runtime::SetAssetDirectory(std::filesystem::path directory) { _impl->assetDirectory = std::move(directory); }
    const std::filesystem::path& Runtime::AssetDirectory() const { return _impl->Assets(); }
    std::string Runtime::AssetId(const std::filesystem::path& file, const int mesh) const { return _impl->AssetId(file, mesh); }
    std::shared_ptr<const Model> Runtime::ModelOf(const std::filesystem::path& path, std::string* error) const
    {
        Impl& files = _impl->source ? *_impl->source : *_impl;
        const std::filesystem::path file = files.AssetFile(path.generic_string());
        const std::string key = file.lexically_normal().generic_string();
        if (const auto it = files.models.find(key); it != files.models.end()) {
            if (!it->second && error) *error = files.unread[key];
            return it->second;
        }
        files.WantModel(file);
        return nullptr;
    }

    bool Runtime::MakeMesh(const std::string& id, const VertexData& data, std::string* error)
    {
        Impl& files = _impl->source ? *_impl->source : *_impl;
        auto made = files.MeshOf(data);
        if (!made) { if (error) *error = made.error(); return false; }
        (*made)->node = id;
        (*made)->kind = "Mesh";
        Held& kept = files.assets[id];
        if (kept && kept != *made) { kept->replacedBy = *made; ++Impl::replacements; }
        kept = std::move(*made);
        // A generator's, kept in the project's cache: there when the scene is opened again without its scripts.
        if (Impl::Generated(id))
            if (const auto cache = Impl::CacheBeside(files.Assets())) (void)cache->Write("meshes", Impl::GeneratedKey(id), ".kmesh", kasset::Serialize(data));
        return true;
    }

    bool Runtime::MakeImage(const std::string& id, const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba, std::string* error)
    {
        Impl& files = _impl->source ? *_impl->source : *_impl;
        auto made = Impl::PictureOf(width, height, rgba);
        if (!made) { if (error) *error = made.error(); return false; }
        (*made)->node = id;
        Held& kept = files.assets[id];
        if (kept && kept != *made) { kept->replacedBy = *made; ++Impl::replacements; }
        kept = std::move(*made);
        if (Impl::Generated(id))
            if (const auto cache = Impl::CacheBeside(files.Assets())) (void)cache->Write("images", Impl::GeneratedKey(id), ".kimage", Impl::PictureBytes(width, height, rgba));
        return true;
    }

    std::optional<VertexData> Runtime::ReadMesh(const std::string_view id, std::string* error) const
    {
        const auto fail = [&](std::string why) -> std::optional<VertexData> { if (error) *error = std::move(why); return std::nullopt; };
        Held held = ResourceOf(id);
        while (held && held->replacedBy) held = held->replacedBy;
        if (!held || held->kind != "Mesh") return fail(std::format("'{}' is no mesh", id));
        if (held->pending) return fail(std::format("'{}' is still being read", id));
        const Resource* whole = held->part ? held->source.get() : held.get();
        if (!whole || !whole->handle) return fail(std::format("'{}' has no mesh on the device", id));
        const auto mesh = kor::capi::MeshOf(whole->handle);
        const auto& layout = mesh->Layout();
        const auto buffers = mesh->VertexBuffers();
        const std::uint32_t first = held->part ? static_cast<std::uint32_t>(held->vertexOffset) : 0;
        const std::uint32_t count = held->part ? held->vertexCount : static_cast<std::uint32_t>(mesh->VertexCount());
        // Each buffer, and the indices, copied where they can be read, all at once.
        const auto readable = [](const kor::ResourceRef<const kor::Buffer>& from) {
            return kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(from->size())).SetUsage(kor::Buffer::Usage::eTransferDst)
                .SetType(kor::Buffer::Type::eReadback).Build();
        };
        std::vector<kor::Resource<kor::Buffer>> copies;
        for (const auto& buffer : buffers) copies.push_back(readable(buffer));
        std::optional<kor::Resource<kor::Buffer>> indexCopy;
        if (const auto indices = mesh->IndexBuffer()) indexCopy = readable(*indices);
        kor::CommandBuffer::SingleTimeCommand([&](kor::CommandBuffer& cb) {
            for (std::size_t b = 0; b < buffers.size(); ++b) cb.CopyBuffer(buffers[b], kor::ResourceRef<const kor::Buffer>(copies[b]));
            if (indexCopy) cb.CopyBuffer(*mesh->IndexBuffer(), kor::ResourceRef<const kor::Buffer>(*indexCopy));
        }, kor::CommandBuffer::Usage::eTransfer).Wait();

        VertexData data;
        data.vertexCount = count;
        std::map<kor::u32, std::vector<std::string>> byBuffer;
        for (const auto& attribute : layout.attributes) {
            if (attribute.channelType != kor::ChannelType::eFloat && attribute.channelType != kor::ChannelType::eInt && attribute.channelType != kor::ChannelType::eUInt)
                return fail(std::format("'{}' of '{}' is not of four-byte numbers, and is not read back", attribute.semantic, id));
            const AttributeType type = attribute.channelType == kor::ChannelType::eInt ? AttributeType::eInt
                                     : attribute.channelType == kor::ChannelType::eUInt ? AttributeType::eUInt : AttributeType::eFloat;
            if (attribute.binding >= copies.size() || attribute.locations != 1) return fail(std::format("'{}' of '{}' is not read back", attribute.semantic, id));
            const auto binding = std::ranges::find(layout.bindings, attribute.binding, &kor::VertexInputBindingDescription::binding);
            if (binding == layout.bindings.end()) return fail(std::format("'{}' has no buffer {}", id, attribute.binding));
            const auto words = copies[attribute.binding]->Read<std::uint32_t>();
            VertexAttribute out { attribute.semantic, type, attribute.channelCount, {} };
            out.words.reserve(static_cast<std::size_t>(count) * attribute.channelCount);
            for (std::uint32_t v = first; v < first + count; ++v) {
                const std::size_t at = (static_cast<std::size_t>(v) * binding->stride + attribute.offset) / 4;
                for (std::uint32_t c = 0; c < attribute.channelCount; ++c) out.words.push_back(at + c < words.size() ? words[at + c] : 0u);
            }
            data.attributes.push_back(std::move(out));
            byBuffer[attribute.binding].push_back(attribute.semantic);
        }
        if (byBuffer.size() > 1) for (auto& semantics : byBuffer | std::views::values) data.buffers.push_back(std::move(semantics));
        if (indexCopy) {
            const auto indices = (*indexCopy)->Read<std::uint32_t>();
            const std::size_t from = held->part ? held->firstIndex : 0, n = held->part ? held->indexCount : indices.size();
            data.indices.assign(indices.begin() + static_cast<std::ptrdiff_t>(std::min(from, indices.size())),
                                indices.begin() + static_cast<std::ptrdiff_t>(std::min(from + n, indices.size())));
        }
        return data;
    }

    std::optional<std::vector<Model::Attribute>> Runtime::ModelAttributes(const std::filesystem::path& path, std::string* error) const
    {
        const std::filesystem::path file = _impl->AssetFile(path.generic_string());
        const std::string key = file.lexically_normal().generic_string();
        // Read already: what it has is known.
        if (const auto it = _impl->models.find(key); it != _impl->models.end() && it->second) return it->second->attributes;
        if (const auto it = _impl->probed.find(key); it != _impl->probed.end()) {
            if (!it->second && error) *error = _impl->unread[key];
            return it->second;
        }
        _impl->probed[key] = std::nullopt;     // being probed: asked again, it is not probed twice
        _impl->reading.push_back(Impl::ProbeModelFile(_impl.get(), _impl->alive, key, file));
        return std::nullopt;
    }

    std::vector<std::string> Runtime::Assets() const
    {
        std::vector<std::string> ids;
        for (const auto& [id, held] : _impl->assets)
            if (held && id.starts_with('@')) ids.push_back(id);
        return ids;
    }

    std::vector<std::string> Runtime::Pictures() const
    {
        std::vector<std::string> ids;
        for (const auto& [id, held] : _impl->source ? _impl->source->assets : _impl->assets)
            if (held && id.starts_with('@') && id.find('#') == std::string::npos && held->kind != "Mesh") ids.push_back(id);
        return ids;
    }
    bool Runtime::Cooked(const std::string_view id) const { return (_impl->source ? *_impl->source : *_impl).Cooked(id); }
    void Runtime::Cook(const std::vector<std::string>& ids) { _impl->Cook(ids); }
    const Runtime::CookProgress& Runtime::Cooking() const { return (_impl->source ? _impl->source : _impl.get())->cooking; }
    std::filesystem::path Runtime::TextureCache() const { return _impl->TextureCache(); }

    std::size_t Runtime::Reading() const { return _impl->reading.size() + _impl->arrivedMeshes.size(); }
    std::vector<std::string> Runtime::Unread() const
    {
        std::vector<std::string> said;
        for (const auto& why : _impl->unread | std::views::values) said.push_back(why);
        return said;
    }

    void Runtime::Watch(const std::string_view id)
    {
        if (_impl->watched.try_emplace(std::string(id)).second) _impl->rewatch = true;
    }
    void Runtime::Watch(const std::string_view id, const std::uint32_t mip, const std::uint32_t layer)
    {
        auto [it, made] = _impl->watched.try_emplace(std::string(id));
        if (made || it->second.mip != mip || it->second.layer != layer) _impl->rewatch = true;
        it->second.mip = mip;
        it->second.layer = layer;
    }
    void Runtime::WatchEvery(const std::string_view id, const std::uint32_t frames)
    {
        auto [it, made] = _impl->watched.try_emplace(std::string(id));
        if (made) _impl->rewatch = true;
        it->second.every = std::max<std::uint32_t>(frames, 1);
    }

    void Runtime::WatchOutput(const std::string_view id, std::string output)
    {
        auto [it, made] = _impl->watched.try_emplace(std::string(id));
        if (made || it->second.output != output) _impl->rewatch = true;
        it->second.output = std::move(output);
    }

    std::string Runtime::ShownOutput(const std::string_view id) const
    {
        // A call's, as a node's: the first of its outputs that holds an image, else the first that holds anything.
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            if (const auto watch = _impl->watched.find(id); watch != _impl->watched.end() && !watch->second.output.empty())
                if (const auto gives = call->second.gives.find(watch->second.output); gives != call->second.gives.end() && !gives->second.node.empty()) return watch->second.output;
            std::string first;
            for (const auto& name : call->second.outputs) {
                const auto gives = call->second.gives.find(name);
                if (gives == call->second.gives.end() || gives->second.node.empty()) continue;
                const auto held = ResourceOf(gives->second.node, gives->second.output);
                if (!held) continue;
                if (ImageRefOf(held.get()).Get()) return name;
                if (first.empty()) first = name;
            }
            return first.empty() ? std::string("out") : first;
        }
        if (_impl->aliases.contains(id)) return "out";
        const auto* node = _impl->Find(id);
        if (!node) return "out";
        if (const auto watch = _impl->watched.find(id); watch != _impl->watched.end() && !watch->second.output.empty())
            if (const auto it = node->outputs.find(watch->second.output); it != node->outputs.end() && it->second.resource) return watch->second.output;
        if (node->outputs.contains("out")) return "out";
        // In the order the node gives them: a Script node's as its script declares them.
        std::string first;
        for (const auto& port : OutputsOf(id)) {
            const auto it = node->outputs.find(port.name);
            if (it == node->outputs.end() || !it->second.resource) continue;
            if (ImageRefOf(it->second.resource.get()).Get()) return port.name;
            if (first.empty()) first = port.name;
        }
        return first.empty() ? std::string("out") : first;
    }

    void Runtime::Unwatch(const std::string_view id)
    {
        if (const auto it = _impl->watched.find(id); it != _impl->watched.end()) { _impl->watched.erase(it); _impl->rewatch = true; }
    }

    // What a watched node is copied out with: a buffer the size of what it holds that can be read back, and the pass
    // that copies into it. Two nodes like any other, added to the graph that runs and to nothing that is saved.
    static std::string PeekId(const std::string_view id) { return std::format("$peek {}", id); }
    static std::string PeekBufferId(const std::string_view id) { return std::format("$peek buffer {}", id); }
    static std::string PeekSourceId(const std::string_view id) { return std::format("$peek source {}", id); }

    namespace {
        /** Which level and layer of an image held as @p held a watch copies: what was asked, within what there is. */
        struct Subresource {
            kor::u32 base = 0, levels = 1, level = 0, layer = 0;
            kor::UVec3 size {};
        };
        Subresource SubresourceOf(const Resource& held, const kor::Image& image, const std::uint32_t mip, const std::uint32_t layer)
        {
            Subresource at;
            at.base = held.kind == "ImageView" ? kor::capi::ImageViewOf(held.handle)->BaseMipLevel() : 0;
            at.levels = std::max<kor::u32>(image.MipLevels() - std::min(at.base, image.MipLevels() - 1), 1);
            at.level = at.base + std::min<kor::u32>(mip, at.levels - 1);
            at.layer = std::min<kor::u32>(layer, std::max<kor::u32>(image.ArrayLayers(), 1) - 1);
            const auto extent = image.Extent();
            at.size = { std::max<kor::u32>(extent.x >> at.level, 1), std::max<kor::u32>(extent.y >> at.level, 1), std::max<kor::u32>(extent.z >> at.level, 1) };
            return at;
        }

        std::vector<std::string> UsageNames(const kor::Flags<kor::Image::Usage> usage)
        {
            std::vector<std::string> names;
            for (const auto& [flag, name] : { std::pair { kor::Image::Usage::eTransferSrc, "eTransferSrc" }, { kor::Image::Usage::eTransferDst, "eTransferDst" },
                                              { kor::Image::Usage::eSampled, "eSampled" }, { kor::Image::Usage::eStorage, "eStorage" },
                                              { kor::Image::Usage::eColorAttachment, "eColorAttachment" }, { kor::Image::Usage::eDepthStencilAttachment, "eDepthStencilAttachment" } })
                if (usage & flag) names.emplace_back(name);
            return names;
        }
    }

    std::string Runtime::WatchProblem(const std::string_view id) const
    {
        if (!_impl->watched.contains(id)) return "it is not watched";
        const auto held = ResourceOf(id);
        if (held && held->pending) return "it is still being read";
        if (!held || (!ImageRefOf(held.get()).Get() && !BufferRefOf(held.get()).Get())) return "it makes no image and no buffer";
        if (const auto image = ImageRefOf(held.get()); image.Get() && TexelLayoutOf(FormatName(image->PixelFormat())).channels == 0)
            return std::format("an image of {} cannot be looked at", FormatName(image->PixelFormat()));
        for (const std::string& node : { PeekSourceId(id), PeekBufferId(id), PeekId(id) })
            if (const auto it = _impl->built.find(node); it != _impl->built.end() && !it->second.status.ok) return it->second.status.error;
        return {};
    }

    std::optional<Peek> Runtime::Watched(const std::string_view id) const
    {
        const auto watch = _impl->watched.find(id);
        if (watch == _impl->watched.end()) return std::nullopt;
        // A node, a call (by the node inside it that what it shows is), or a picture of a file's that no node is (`@wall.png`).
        const auto* node = _impl->Find(id);
        if (const auto call = _impl->calls.find(id); !node && call != _impl->calls.end())
            if (const auto gives = call->second.gives.find(ShownOutput(id)); gives != call->second.gives.end()) node = _impl->Find(gives->second.node);
        const auto held = ResourceOf(id);
        if ((!node && !id.starts_with('@')) || !held || held->pending) return std::nullopt;
        Peek peek;
        std::string now;
        if (const auto image = ImageRefOf(held.get()); image.Get()) {
            const Subresource at = SubresourceOf(*held, *image, watch->second.mip, watch->second.layer);
            const auto extent = image->Extent();
            peek.image = true;
            peek.format = FormatName(image->PixelFormat());
            peek.extent = at.size;
            peek.fullExtent = { std::max<kor::u32>(extent.x >> at.base, 1), std::max<kor::u32>(extent.y >> at.base, 1), std::max<kor::u32>(extent.z >> at.base, 1) };
            peek.mips = at.levels;
            peek.layers = std::max<kor::u32>(image->ArrayLayers(), 1);
            peek.samples = static_cast<std::uint32_t>(image->Samples());
            peek.mip = at.level - at.base;
            peek.layer = at.layer;
            peek.type = image->ImageType() == kor::Image::Type::e1D ? "1D" : image->ImageType() == kor::Image::Type::e3D ? "3D" : "2D";
            peek.usage = UsageNames(image->UsageFlags());
            const TexelLayout texel = TexelLayoutOf(peek.format);
            for (kor::u32 level = at.base; level < at.base + at.levels; ++level)
                peek.byteSize += static_cast<std::uint64_t>(std::max<kor::u32>(extent.x >> level, 1)) * std::max<kor::u32>(extent.y >> level, 1)
                               * std::max<kor::u32>(extent.z >> level, 1) * texel.Bytes() * peek.layers;
            if (texel.channels == 0) return std::nullopt;     // WatchProblem says why
            now = std::format("image {} {}x{}x{} {} {} of {}", peek.format, peek.extent.x, peek.extent.y, peek.extent.z, at.level, at.layer, ShownOutput(id));
        } else if (const auto buffer = BufferRefOf(held.get()); buffer.Get()) {
            now = std::format("buffer {} of {}", buffer->size(), ShownOutput(id));
            peek.byteSize = buffer->size();
            if (node && node->mapped) {
                peek.stride = node->mapped->layout.stride;
                peek.count = static_cast<std::uint32_t>(node->mapped->count);
                // Of 32 bits a number, as what looks at it reads them: one of 16 or 64 is the node's to say, not a table's.
                for (const auto& field : node->mapped->layout.fields)
                    if (field.bytes == 4) peek.fields.push_back({ field.name, field.offset, field.scalar, field.Numbers() });
            } else if (const Node* drawn = _impl->current ? _impl->CurrentNode(id) : nullptr; node && node->packed && drawn && drawn->type == "IndirectDraws") {
                // An IndirectDraws': a VkDrawIndexedIndirectCommand a mesh — how many of it are drawn, from which instance.
                peek.stride = 20;
                peek.count = static_cast<std::uint32_t>(node->packed->bytes.size() / 20);
                peek.fields = { { "indexCount", 0, 'u', 1 }, { "instanceCount", 4, 'u', 1 }, { "firstIndex", 8, 'u', 1 },
                                { "vertexOffset", 12, 'i', 1 }, { "firstInstance", 16, 'u', 1 } };
            }
        } else {
            return std::nullopt;
        }
        // Not what it was when the copy was set up — another size, or only now built: set up again, next frame.
        if (now != watch->second.setUp) { _impl->rewatch = true; return std::nullopt; }
        const auto copied = _impl->built.find(PeekBufferId(id));
        const auto copy = _impl->built.find(PeekId(id));
        if (copied == _impl->built.end() || copy == _impl->built.end() || !copied->second.status.ok || !copy->second.status.ok) return std::nullopt;
        const auto out = copied->second.outputs.find("out");
        if (out == copied->second.outputs.end() || !out->second.resource || !out->second.resource->handle) return std::nullopt;
        peek.bytes = kor::capi::BufferOf(out->second.resource->handle)->Read<std::byte>();
        return peek;
    }

    void Runtime::Run()
    {
        // What a ForEach's element holds: where it is, and its component's fields — of `with`'s too, by their own names,
        // its component's coming first — as the world describes them now.
        const ElementOf elementOf = [this](const Node& node) {
            std::vector<std::pair<std::string, std::string>> members;
            for (const char* place : { "entityId", "matrix", "normalMatrix", "previousMatrix", "worldPosition", "forward", "up", "right", "position", "rotation", "orientation", "scale" })
                members.emplace_back(place, place);
            kecs::World* world = _impl->world;
            if (!world) return members;
            const auto add = [&](const std::string& component, const bool own) {
                const kecs::ComponentId id = world->Find(component);
                if (id == kecs::NoComponent) return;
                const kecs::ComponentDescription& description = world->Describe(id);
                for (const auto& field : description.fields) {
                    if (field.type == "string") continue;   // nothing a buffer holds
                    const std::string output = own ? "field." + field.name : description.name + "." + field.name;
                    if (const auto had = std::ranges::find(members, field.name, &std::pair<std::string, std::string>::first); had != members.end()) {
                        if (own) had->second = output;     // its own component's, before where it is
                    } else {
                        members.emplace_back(field.name, output);
                    }
                    // A mesh's buffers and layout, as a MeshBuffers gives them: what taking a Mesh apart gives besides it.
                    if (field.type == "Mesh")
                        for (const auto& part : MeshParts()) {
                            const std::string name = MeshPartName(field.name, part.name);
                            if (!std::ranges::contains(members, name, &std::pair<std::string, std::string>::first))
                                members.emplace_back(name, own ? "field." + name : description.name + "." + name);
                        }
                }
            };
            if (const auto component = node.inputs.find("component"); component != node.inputs.end() && component->second.value.String()) add(*component->second.value.String(), true);
            for (const char* listed : { "with" })
                if (const auto with = node.inputs.find(listed); with != node.inputs.end() && with->second.value.Items())
                    for (const auto& name : *with->second.value.Items())
                        if (name.String()) add(*name.String(), false);
            return members;
        };
        // What of a component a ComponentBuffer can hold: each field that is a number, a vector or a matrix.
        const FieldsOf fieldsOf = [this](const std::string_view component) -> std::optional<std::vector<std::pair<std::string, std::string>>> {
            kecs::World* world = _impl->world;
            const kecs::ComponentId id = world ? world->Find(component) : kecs::NoComponent;
            if (id == kecs::NoComponent) return std::nullopt;
            std::vector<std::pair<std::string, std::string>> fields;
            for (const auto& field : world->Describe(id).fields)
                if (const std::string_view type = LayoutTypeOf(field.type); !type.empty()) fields.emplace_back(field.name, type);
            return fields;
        };
        Expansion expansion = Expand(_graph, elementOf, fieldsOf, _impl->loopCounts);
        _impl->before.clear();
        // What is watched is copied out by two nodes more, sized by what the node made when last it was built.
        for (auto& [id, watching] : _impl->watched) {
            std::string& setUp = watching.setUp;
            setUp.clear();
            const auto held = ResourceOf(id);
            if (!held || held->pending || expansion.graph.Find(PeekId(id))) continue;
            std::size_t bytes = 0;
            Node copy;
            copy.id = PeekId(id);
            // The node itself, or — for one of a loop — what it is on its first turn; a file's picture, by a node of its own;
            // a call's output, by the node inside it that it is.
            std::string source = expansion.graph.Find(id) ? std::string(id) : TurnId(id, 0);
            std::string output = ShownOutput(id);
            if (const auto call = expansion.calls.find(id); call != expansion.calls.end()) {
                const auto gives = call->second.gives.find(output);
                if (gives == call->second.gives.end() || gives->second.node.empty()) continue;
                source = gives->second.node;
                output = gives->second.output;
            }
            if (!expansion.graph.Find(source) && id.starts_with('@') && !id.contains('#')) {
                source = PeekSourceId(id);
                expansion.graph.Add(source, "ImageFile").Set("file", id.substr(1));
            }
            if (!expansion.graph.Find(source)) continue;
            if (const auto image = ImageRefOf(held.get()); image.Get()) {
                const std::string format = FormatName(image->PixelFormat());
                const TexelLayout texel = TexelLayoutOf(format);
                if (texel.channels == 0) continue;
                const Subresource at = SubresourceOf(*held, *image, watching.mip, watching.layer);
                bytes = static_cast<std::size_t>(at.size.x) * at.size.y * at.size.z * texel.Bytes();
                copy.type = "CopyImageToBuffer";
                copy.Wire("image", source, output).Wire("buffer", PeekBufferId(id));
                if (at.level > 0) copy.Set("mip", at.level);
                if (at.layer > 0) copy.Set("layer", at.layer);
                setUp = std::format("image {} {}x{}x{} {} {} of {}", format, at.size.x, at.size.y, at.size.z, at.level, at.layer, ShownOutput(id));
            } else if (const auto buffer = BufferRefOf(held.get()); buffer.Get()) {
                bytes = static_cast<std::size_t>(buffer->size());
                copy.type = "CopyBuffer";
                copy.Wire("source", source, output).Wire("destination", PeekBufferId(id));
                setUp = std::format("buffer {} of {}", bytes, ShownOutput(id));
            } else {
                continue;
            }
            Node& into = expansion.graph.Add(PeekBufferId(id), "Buffer");
            into.Set("instanceCount", static_cast<double>(bytes)).Set("usage", Value::List { "eTransferDst" }).Set("type", "eReadback");
            // A Reference's is of what it stands for as it is where the Reference is: when its page begins, first of it.
            if (const Node* reference = expansion.graph.Find(id); reference && reference->type == "Reference" && !reference->pass.empty()) {
                copy.pass = reference->pass;
                _impl->before.insert(copy.id);
                const auto first = std::ranges::find(expansion.graph.nodes, copy.pass, &Node::pass);
                expansion.graph.nodes.insert(first, std::move(copy));
            } else {
                expansion.graph.nodes.push_back(std::move(copy));
            }
        }
        _impl->rewatch = false;
        _impl->unwritten.clear();
        for (auto& [id, why] : expansion.errors) _impl->unwritten[id] = NodeStatus { false, std::move(why), 0, {} };
        _impl->turns = std::move(expansion.turns);
        _impl->calls = std::move(expansion.calls);
        _impl->aliases = std::move(expansion.aliases);
        _impl->bundles = std::move(expansion.bundles);
        _impl->fromBundle = std::move(expansion.fromBundle);
        _impl->bundleNotes = std::move(expansion.notes);
        _impl->batched = std::move(expansion.batched);
        _impl->unbatched = std::move(expansion.unbatched);
        _impl->wiredCounts = std::move(expansion.wiredCounts);
        // What is found by id is the running graph's ids, seen, not kept: forgotten before that graph goes, as even
        // clearing it reads its keys.
        _impl->currentById.clear();
        _running = std::move(expansion.graph);
        _impl->Evaluate(_running);
    }

    void Runtime::Retire(std::shared_ptr<Resource> resource)
    {
        if (resource) _impl->retiredResources.push_back(std::move(resource));
    }

    void Runtime::Update()
    {
        const kor::log::Quiet quiet;
        _impl->WriteMaterialsNow();
        const Impl::Using using_(*_impl, std::string(Impl::EntitiesUse));
        // A frame, counted before anything can end it early: a buffer of instances this runtime did not fill in it — a
        // frame it was built again in — is filled by a runtime that follows it.
        ++*_impl->frames;
        _impl->LetGo();
        if (_impl->reloaded.exchange(false)) {
            _impl->ResetStale(_graph);
            // Which shader it was is not said: every shader node counts as built again, and what is wired to one follows.
            // A pipeline that names its shaders is itself one of them: it is built again, whatever its inputs say.
            for (auto& node : _impl->built | std::views::values) {
                if (node.watches.empty()) continue;
                node.stamp = ++_impl->counter;
                if (node.pipeline || node.mapped) node.signature.clear();
            }
            Run();
            return;
        }
        if (_impl->shaderFailed && std::chrono::steady_clock::now() - _impl->lastRetry > std::chrono::milliseconds(500)) {
            _impl->lastRetry = std::chrono::steady_clock::now();
            _impl->shaderFailed = false;    // set again by whichever still fails
            for (auto& node : _impl->built | std::views::values)
                if (!node.status.ok) node.signature.clear();
            _impl->Evaluate(_running);
            return;
        }
        if (_impl->rewatch) { Run(); return; }
        if (_impl->editorViewAsked != _impl->editorView || _impl->debugViewAsked != _impl->debugView || _impl->shaderDebugAsked != _impl->shaderDebug) {
            _impl->editorView = _impl->editorViewAsked;
            _impl->debugView = _impl->debugViewAsked;
            _impl->shaderDebug = _impl->shaderDebugAsked;
            _impl->Evaluate(_running);
            return;
        }
        // A resource script set again: what it fills is made and filled again — here, and in a runtime that follows this one.
        if (const std::uint64_t version = _impl->ScriptsVersion(); version != _impl->scriptsSeen) {
            _impl->scriptsSeen = version;
            _impl->Evaluate(_running);
            return;
        }
        _impl->Arrive();
        _impl->Watch();
        WriteBack();
        _impl->EachFrame();
        if (std::exchange(_impl->stale, false)) { _impl->Evaluate(_running); return; }
        // A loop whose count is wired, and what it is wired from gives another now: written out again with that many turns.
        if (_impl->Recount()) { Run(); return; }
        if (_impl->PrepareAll()) _impl->Resolve();
    }

    void Runtime::WriteBack()
    {
        // A view that follows another writes nothing back: the one it follows does, of the same world.
        if (_impl->source || !_impl->world || !_impl->current) return;
        kecs::World& world = *_impl->world;
        // Setup still being recorded (once for each copy of what it makes): it is made from the world as it is, and what is
        // read back is of what it is about to make again — written back, setup would be made from that instead.
        const bool remaking = _impl->setupAgain || std::ranges::any_of(_impl->built | std::views::values, [](const auto& built) { return built.setup && built.runsLeft > 0; });
        std::set<std::string> seen;
        for (const Node& node : _impl->current->nodes) {
            if (node.type != "WriteBack") continue;
            seen.insert(node.id);
            const NodeStatus* status = Status(node.id);
            const auto buffer = node.inputs.find("buffer");
            const auto each = node.inputs.find("forEach");
            if (!status || !status->ok || buffer == node.inputs.end() || !buffer->second.link || each == node.inputs.end() || !each->second.value.String()) continue;
            const std::string& from = buffer->second.link->node;
            // Idle: what it watches is let be — and what it would take as moved by something else is not looked at.
            if (const auto idle = node.inputs.find("idle"); idle != node.inputs.end()) {
                Value said = idle->second.value;
                if (idle->second.link)
                    if (const auto source = _impl->built.find(idle->second.link->node); source != _impl->built.end())
                        if (const auto output = source->second.outputs.find(idle->second.link->output); output != source->second.outputs.end()) said = output->second.value;
                if (said.Bool() ? *said.Bool() : said.Number() && *said.Number() != 0.0) { _impl->writingBack[node.id] = {}; seen.insert(node.id); continue; }
            }
            Watch(from);        // asked again each frame: what someone else stopped watching is watched again
            auto& state = _impl->writingBack[node.id];
            if (remaking) { state.bytes.clear(); continue; }
            const auto peek = Watched(from);
            if (!peek || peek->bytes.empty()) continue;
            const auto listed = _impl->built.find(*each->second.value.String());
            if (listed == _impl->built.end() || !listed->second.listed) continue;
            const std::vector<kecs::Entity>& entities = listed->second.listed->entities;
            const auto number = [&](const char* name) -> std::optional<std::size_t> {
                const auto it = node.inputs.find(name);
                const double* n = it == node.inputs.end() ? nullptr : it->second.value.Number();
                return n ? std::optional(static_cast<std::size_t>(*n)) : std::nullopt;
            };
            const std::size_t stride = number("stride").value_or(0);
            const auto position = number("position"), rotation = number("rotation"), follows = number("follows");
            const auto velocity = number("velocity"), spin = number("spin");
            // What is read back is of the steps before this frame's, which go on before it is drawn (and before what a script
            // moves this frame — a kinematic blade — is drawn where it was put): each element sent on by as long, as it goes.
            const float ahead = static_cast<float>(std::min(_impl->fixedPending, static_cast<std::uint32_t>(std::max(_impl->current->fixedStepsAtMost, 1)))
                                                   * _impl->fixedSeconds);
            if (stride == 0) continue;
            // An element that follows its entity: the entity says where it is, and is neither written nor taken as moved.
            const auto following = [&](const std::size_t i) {
                if (!follows || (i + 1) * stride > peek->bytes.size() || *follows + 4 > stride) return false;
                float flag;
                std::memcpy(&flag, peek->bytes.data() + i * stride + *follows, sizeof flag);
                return flag != 0.f;
            };
            const auto key = [](const kecs::Entity e) { return (std::uint64_t { e.index } << 32) | e.generation; };
            // One moved by something else since it was last written — the gizmo, a script, the inspector: setup is made
            // again, from the world as it is, and what is read back from before that is let pass.
            bool moved = false;
            for (std::size_t i = 0; i < entities.size(); ++i) {
                const kecs::Entity e = entities[i];
                if (following(i)) continue;
                const auto put = state.put.find(key(e));
                if (put == state.put.end() || !world.Alive(e)) continue;
                const kecs::Vec3 at = world.PositionOf(e);
                const kecs::Quat turn = world.OrientationOf(e);
                const auto far = [](const float a, const float b) { return std::abs(a - b) > 1e-4f; };
                if ((position && (far(at.x, put->second.first.x) || far(at.y, put->second.first.y) || far(at.z, put->second.first.z)))
                    || (rotation && (far(turn.x, put->second.second.x) || far(turn.y, put->second.second.y) || far(turn.z, put->second.second.z) || far(turn.w, put->second.second.w)))) {
                    moved = true;
                    break;
                }
            }
            if (moved) {
                RunSetupAgain();
                state.put.clear();
                state.skip = static_cast<int>(kor::Context::HasScheduler() ? kor::Context::Scheduler().ImageCount() : 1) + 2;
                continue;
            }
            if (state.skip > 0) { --state.skip; state.bytes.clear(); continue; }   // from before: not written, and not taken as written
            if (peek->bytes == state.bytes) continue;       // nothing new: nothing stepped — and nothing written over an edit
            state.bytes = peek->bytes;
            const auto floats = [&](const std::size_t at, float* into, const int n) { std::memcpy(into, peek->bytes.data() + at, sizeof(float) * n); };
            std::vector<std::tuple<kecs::ComponentId, const kecs::FieldDescription*, std::size_t>> fields;
            if (const auto given = node.inputs.find("fields"); given != node.inputs.end())
                if (const auto* members = given->second.value.Named())
                    for (const auto& [name, offset] : *members) {
                        const auto dot = name.find('.');
                        const kecs::ComponentId id = world.Find(name.substr(0, dot));
                        const kecs::FieldDescription* field = id == kecs::NoComponent ? nullptr : world.Describe(id).FindField(name.substr(dot + 1));
                        if (field && offset.Number()) fields.emplace_back(id, field, static_cast<std::size_t>(*offset.Number()));
                    }
            const std::size_t count = std::min(entities.size(), peek->bytes.size() / stride);
            for (std::size_t i = 0; i < count; ++i) {
                const kecs::Entity e = entities[i];
                if (!world.Alive(e) || following(i)) continue;
                const std::size_t base = i * stride;
                if (position) {
                    float p[3];
                    floats(base + *position, p, 3);
                    if (velocity && ahead > 0.f && base + *velocity + 12 <= peek->bytes.size()) {
                        float v[3];
                        floats(base + *velocity, v, 3);
                        for (int k = 0; k < 3; ++k) p[k] += v[k] * ahead;
                    }
                    world.SetPosition(e, { p[0], p[1], p[2] });
                }
                if (rotation) {
                    float q[4];
                    floats(base + *rotation, q, 4);
                    if (spin && ahead > 0.f && base + *spin + 12 <= peek->bytes.size()) {
                        // Turned on by the spin (about the world's axes) for as long: q + ½ (w, 0) q t, made length 1 again.
                        float w[3];
                        floats(base + *spin, w, 3);
                        const float h = 0.5f * ahead;
                        const float d[4] = { h * (w[0] * q[3] + w[1] * q[2] - w[2] * q[1]), h * (w[1] * q[3] + w[2] * q[0] - w[0] * q[2]),
                                             h * (w[2] * q[3] + w[0] * q[1] - w[1] * q[0]), h * (-w[0] * q[0] - w[1] * q[1] - w[2] * q[2]) };
                        float l = 0.f;
                        for (int k = 0; k < 4; ++k) { q[k] += d[k]; l += q[k] * q[k]; }
                        l = std::sqrt(l);
                        if (l > 1e-12f) for (float& c : q) c /= l;
                    }
                    world.SetOrientation(e, { q[0], q[1], q[2], q[3] });
                }
                for (const auto& [id, field, offset] : fields) {
                    if (!world.Has(e, id) || offset + field->size > stride) continue;
                    auto* bytes = static_cast<std::byte*>(const_cast<void*>(std::as_const(world).Get(e, id)));
                    std::memcpy(bytes + field->offset, peek->bytes.data() + base + offset, field->size);
                }
                state.put[key(e)] = { world.PositionOf(e), world.OrientationOf(e) };
            }
        }
        std::erase_if(_impl->writingBack, [&](const auto& entry) { return !seen.contains(entry.first); });
    }

    void Runtime::SetView(std::optional<View> view) { _impl->viewOverride = std::move(view); }
    void Runtime::SetHidden(std::set<kecs::Entity> hidden)
    {
        if (hidden == _impl->hidden) return;
        _impl->hidden = std::move(hidden);
        ++_impl->hiddenVersion;     // each list of what is drawn found again
    }
    void Runtime::Share(const std::set<std::string>& nodes)
    {
        if (!_impl->source || std::ranges::equal(nodes, _impl->shared)) return;
        _impl->shared = std::set<std::string, std::less<>>(nodes.begin(), nodes.end());
        _impl->stale = true;    // built again, next frame: those taken from the source, and the rest as their own
    }
    void Runtime::SetSelected(const kecs::Entity entity) { _impl->selectedId->store(entity.IsNull() ? 0 : entity.index + 1); }
    void Runtime::SetEditorView(const bool editor) { _impl->editorViewAsked = editor; }
    bool Runtime::EditorView() const { return _impl->editorViewAsked; }
    void Runtime::ShowDebugView(std::string id) { _impl->debugViewAsked = std::move(id); }
    const std::string& Runtime::ShownDebugView() const { return _impl->debugViewAsked; }
    void Runtime::SetShaderDebugView(const int channel) { _impl->shaderDebugAsked = channel; }
    int Runtime::ShaderDebugView() const { return _impl->shaderDebugAsked; }

    std::vector<Runtime::DebugViewOf> Runtime::DebugViews() const
    {
        std::vector<DebugViewOf> views;
        for (const auto& node : _running.nodes) {
            if (node.type != "DebugView") continue;
            const auto name = node.inputs.find("name");
            const std::string* named = name != node.inputs.end() ? name->second.value.String() : nullptr;
            views.push_back({ node.id, named && !named->empty() ? *named : node.id });
        }
        return views;
    }

    std::vector<std::string> Runtime::WithoutShaderDebugView() const
    {
        std::vector<std::string> without;
        const std::string constant = std::format("constant.{}", ShaderDebugConstant);
        for (const auto& [id, built] : _impl->built) {
            if (!built.pipeline || !built.status.ok || built.pipeline->numThreads[0] != 0) continue;     // a graphics pipeline, built
            // One that draws no colour — a depth prepass, a shadow — has nothing of a material to show.
            const Held& target = built.pipeline->framebuffer;
            if (!target || !target->handle || kor::capi::FramebufferOf(target->handle)->ColorAttachmentCount() == 0) continue;
            if (std::ranges::find(built.pipeline->ports, constant, &Port::name) == built.pipeline->ports.end()) without.push_back(id);
        }
        return without;
    }
    void Runtime::FixedStep(const float seconds)
    {
        ++_impl->fixedPending;
        if (seconds > 0.f) _impl->fixedSeconds = seconds;
    }
    std::uint32_t Runtime::FixedSteps() const { return _impl->fixedSteps; }
    void Runtime::SetFixedFraction(const float fraction) { _impl->fixedFraction = std::clamp(static_cast<double>(fraction), 0.0, 1.0); }
    std::map<std::string, Runtime::Timing, std::less<>> Runtime::Timings() const
    {
        std::map<std::string, Timing, std::less<>> timings;
        for (const auto& pass : _impl->frameGraph.PassTimings()) {
            if (!_impl->passes.contains(pass.name)) continue;      // another's: not one of this graph's passes
            Timing& of = timings[Impl::StepOf(pass.name).first];
            of.gpuMs += pass.gpuMs;
            of.recordMs += pass.recordMs;
            of.prepareMs += pass.prepareMs;
            of.gpuMeasured = of.gpuMeasured || pass.gpuMeasured;
            ++of.runs;
        }
        return timings;
    }
    Runtime::FrameReport Runtime::Frame() const
    {
        FrameReport report;
        const kor::FrameGraph& graph = _impl->frameGraph;
        // What each resource the passes use is, by the node that made it: the frame graph knows it by where it is.
        std::unordered_map<const void*, std::string> madeBy;
        for (const auto& [id, made] : _impl->built)
            for (const auto& output : made.outputs | std::views::values) {
                if (!output.resource || output.resource->node != id) continue;
                if (const auto image = ImageRefOf(output.resource.get()); image.Get()) madeBy.try_emplace(image.Get(), id);
                else if (const auto buffer = BufferRefOf(output.resource.get()); buffer.Get()) madeBy.try_emplace(buffer.Get(), id);
            }
        const auto named = [&](const std::string& name) -> std::string {
            // (Their runtime's in the name: every runtime of a frame graph has a pass of some name, and they share its names.)
            if (name.starts_with("kgraph.set.")) return std::format("the entities' {}", name.substr(name.find('.', 11) + 1));
            if (name.starts_with("kgraph.transient.")) return name.substr(name.find('.', 17) + 1);     // its node
            if (name.starts_with("kgraph.")) {
                const auto at = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(std::stoull(name.substr(7), nullptr, 16)));
                if (const auto found = madeBy.find(at); found != madeBy.end()) return found->second;
            }
            return name;
        };
        std::map<std::string, kor::FrameGraph::PassTiming, std::less<>> timings;
        for (auto& timing : graph.PassTimings()) timings.emplace(timing.name, timing);
        for (const auto& scheduled : graph.Schedule()) {
            const auto ours = _impl->passes.find(scheduled.name);
            if (ours == _impl->passes.end()) continue;     // another's: not one of this graph's passes
            FrameReport::Pass pass { scheduled.name, Impl::StepOf(scheduled.name).first, scheduled.level, scheduled.async };
            if (const auto timing = timings.find(scheduled.name); timing != timings.end()) {
                pass.gpuMs = timing->second.gpuMs;
                pass.recordMs = timing->second.recordMs;
                pass.gpuMeasured = timing->second.gpuMeasured;
                pass.cpuPrepareStartMs = timing->second.cpuPrepareStartMs;
                pass.cpuPrepareMs = timing->second.cpuPrepareLastMs;
                pass.cpuRecordStartMs = timing->second.cpuRecordStartMs;
                pass.cpuRecordMs = timing->second.cpuRecordLastMs;
                pass.recordThread = timing->second.recordThread;
                pass.gpuStartMs = timing->second.gpuStartMs;
                pass.gpuLastMs = timing->second.gpuLastMs;
                pass.gpuOnCpuClock = timing->second.gpuOnCpuClock;
                pass.work = timing->second.work;
            }
            for (const auto& use : ours->second->declared) pass.uses.emplace_back(named(use.name), use.read && use.write ? 'b' : use.write ? 'w' : 'r');
            if (ours->second->plan->readsScreen || ours->second->plan->writesScreen)
                pass.uses.emplace_back("the screen", ours->second->plan->readsScreen && ours->second->plan->writesScreen ? 'b' : ours->second->plan->writesScreen ? 'w' : 'r');
            report.passes.push_back(std::move(pass));
        }
        for (const auto& skipped : graph.SkippedPasses())
            if (_impl->passes.contains(skipped.name))
                report.leftOut.emplace_back(Impl::StepOf(skipped.name).first, std::format("skipped: what it needs, {}, is made by {}, which is left out", named(skipped.resource), skipped.source));
        for (const auto& culled : graph.CulledPasses())
            if (_impl->passes.contains(culled)) report.leftOut.emplace_back(Impl::StepOf(culled).first, "culled: nothing needs what it makes");
        for (const auto& id : _impl->restingUnread) report.leftOut.emplace_back(id, "resting: nothing that runs this frame reads what it makes");
        report.bytes = graph.Memory().bytes;
        report.unsharedBytes = graph.Memory().unsharedBytes;
        return report;
    }

    Runtime::FrameTimes Runtime::Times() const
    {
        auto times = _impl->frameGraph.Times();
        return { std::move(times.frameMs), std::move(times.gpuMs) };
    }

    void Runtime::RunSetupAgain() { _impl->setupAgain = true; }
    void Runtime::SetDebugDraw(kor::DebugDraw* debug) { _impl->debug = debug; }
    kor::Mat4 Runtime::ViewProjection() const { return _impl->viewProjection; }
    View Runtime::CurrentView() const { return _impl->currentView; }
    float Runtime::Aspect() const { return _impl->currentAspect; }

    void Runtime::SetScreenExtent(const kor::UVec2 extent)
    {
        if (extent == _impl->screenExtent || extent.x == 0 || extent.y == 0) return;
        _impl->screenExtent = extent;
        const kor::log::Quiet quiet;
        _impl->Evaluate(_running);    // only a Screen node, and what is sized by one, is built again
    }

    void Runtime::SetScreenLook(std::string format, const float scale)
    {
        if (format == _impl->screenFormat && scale == _impl->screenScale) return;
        _impl->screenFormat = std::move(format);
        _impl->screenScale = scale;
        const kor::log::Quiet quiet;
        _impl->Evaluate(_running);
    }

    bool Runtime::Pick(const float u, const float v, std::function<void(kecs::Entity)> then)
    {
        const bool there = std::ranges::any_of(_graph.nodes, [&](const Node& node) { return node.type == "Pick" && Status(node.id) && Status(node.id)->ok; });
        if (!there) return false;
        _impl->pick->asked = std::pair { std::clamp(u, 0.f, 1.f), std::clamp(v, 0.f, 1.f) };
        _impl->pick->then = [world = _impl->world, then = std::move(then)](const kor::u32 id, const PickHit&) {
            then(world && id != 0 ? world->EntityAt(id - 1) : kecs::NullEntity);
        };
        return true;
    }

    bool Runtime::PickAt(const float u, const float v, std::function<void(const PickHit&)> then)
    {
        const bool there = std::ranges::any_of(_graph.nodes, [&](const Node& node) { return node.type == "Pick" && Status(node.id) && Status(node.id)->ok; });
        if (!there) return false;
        _impl->pick->asked = std::pair { std::clamp(u, 0.f, 1.f), std::clamp(v, 0.f, 1.f) };
        _impl->pick->then = [world = _impl->world, then = std::move(then)](const kor::u32 id, const PickHit& hit) {
            PickHit found = hit;
            found.entity = world && id != 0 ? world->EntityAt(id - 1) : kecs::NullEntity;
            then(found);
        };
        return true;
    }

    const NodeStatus* Runtime::Status(const std::string_view id) const
    {
        if (const auto it = _impl->built.find(id); it != _impl->built.end()) return &it->second.status;
        if (const auto it = _impl->unwritten.find(id); it != _impl->unwritten.end()) return &it->second;
        // A node of a loop fared as its turns did: as the first of them that failed, or as its first.
        if (const auto of = _impl->turns.find(id); of != _impl->turns.end()) {
            const NodeStatus* first = nullptr;
            for (const auto& turn : of->second) {
                const auto it = _impl->built.find(turn);
                if (it == _impl->built.end()) continue;
                if (!it->second.status.ok) return &it->second.status;
                if (!first) first = &it->second.status;
            }
            return first;
        }
        // A call fared as its nodes did: as the first of them that failed, said with which.
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            NodeStatus fared { true, {}, 1 };
            for (const auto& inner : call->second.nodes)
                if (const NodeStatus* status = Status(inner); status && !status->ok) {
                    fared = NodeStatus { false, std::format("{}: {}", inner.substr(call->second.called.size() + 1), status->error), status->builds, {} };
                    break;
                }
            return &(_impl->callStatus[std::string(id)] = std::move(fared));
        }
        // An edge of a function opened in place stands for what it is given: there is nothing of it to fail.
        static const NodeStatus edge { true, {}, 1 };
        if (_impl->aliases.contains(id)) return &edge;
        // A Repeat is a count and nothing more: there is nothing of it to fail once it is one.
        static const NodeStatus counted { true, {}, 1 };
        const Node* node = _graph.Find(id);
        return node && node->type == "Repeat" ? &counted : nullptr;
    }

    bool Runtime::Ok() const
    {
        // (What copies a watched node out is no part of the graph: that it cannot is WatchProblem's to say.)
        return _impl->unwritten.empty() && std::ranges::all_of(_impl->built, [](const auto& node) { return node.second.status.ok || node.first.starts_with("$peek"); });
    }

    std::vector<std::string> Runtime::Problems() const
    {
        std::vector<std::string> problems;
        for (const auto& node : _graph.nodes)
            if (const auto* status = Status(node.id); status && !status->ok) problems.push_back(std::format("{}: {}", node.id, status->error));
        return problems;
    }

    std::vector<std::string> Runtime::TurnsOf(const std::string_view id) const
    {
        const auto of = _impl->turns.find(id);
        return of == _impl->turns.end() ? std::vector<std::string> {} : of->second;
    }

    std::vector<Preset> Runtime::Presets() const
    {
        std::vector<Preset> presets;
        // A Script node of each of the project's ResourceScripts: what it makes, at once.
        for (const auto& script : ResourceScripts()) {
            std::string makes;
            for (const auto& resource : script.resources) makes += std::format("{}{} ({})", makes.empty() ? "" : ", ", resource.name, resource.kind);
            presets.push_back({ script.name, "Script", {}, makes.empty() ? std::format("{}: a ResourceScript that makes nothing yet.", script.name)
                                                                          : std::format("What {} makes, filled by it: {}.", script.name, makes),
                                { { "script", Value(script.name) } } });
        }
        const kecs::World* world = _impl->world;
        if (!world) { std::ranges::sort(presets, {}, &Preset::name); return presets; }
        // Every component of the world, as a ForEach of it: its buffer is one ComponentBuffer node, told which.
        for (kecs::ComponentId id = 0; id < world->ComponentCount(); ++id) {
            const kecs::ComponentDescription& type = world->Describe(id);
            if (type.IsTag() || type.name == "Name" || type.name.empty()) continue;
            std::string fields;
            for (const auto& field : type.fields)
                if (!LayoutTypeOf(field.type).empty()) fields += std::format("{}{}", fields.empty() ? "" : ", ", field.name);
            if (fields.empty()) continue;
            presets.push_back({ type.name, "ForEach", type.name,
                                std::format("Every entity with a {}, an element at a time: {}, and where it is.", type.name, fields),
                                { { "component", Value(type.name) } } });
        }
        std::ranges::sort(presets, {}, &Preset::name);
        return presets;
    }

    std::string Runtime::EachOf(const std::string_view id) const
    {
        std::string source;
        const auto found = _impl->built.find(std::string(id));
        if (const auto* made = _impl->Find(id); made && made->pass && made->pass->entities) {
            source = made->pass->entities->each;     // a Draw's or a Dispatch's — of a loop, its first turn's: what it draws an element of
        } else if (found != _impl->built.end() && found->second.mapped) {
            source = found->second.mapped->source;
        } else {
            std::set<std::string> sources;
            std::vector<std::string> chain;
            std::map<std::string, bool> seen;
            if (!_impl->Follows(std::string(id), sources, chain, seen) || sources.size() != 1) return {};
            source = *sources.begin();
        }
        // One that chooses one of them is no loop.
        if (const Node* each = _impl->current && !source.empty() ? _impl->CurrentNode(source) : nullptr)
            if (each->type == "Entity") return {};
        return source;
    }

    std::shared_ptr<Resource> Runtime::ResourceOf(const std::string_view id, const std::string_view given) const
    {
        const std::string shown = given.empty() ? ShownOutput(id) : std::string();
        const std::string_view output = given.empty() ? std::string_view(shown) : given;
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            const auto gives = call->second.gives.find(output);
            return gives == call->second.gives.end() || gives->second.node.empty() ? nullptr : ResourceOf(gives->second.node, gives->second.output);
        }
        if (const auto edge = _impl->aliases.find(id); edge != _impl->aliases.end())
            return edge->second.link ? ResourceOf(edge->second.link->node, edge->second.link->output) : nullptr;
        const auto* node = _impl->Find(id);
        // A procedural mesh is there without a node of its own: `$cube`, `$sphere?rings=24`.
        if (!node) {
            // A file's, and a generator's: made this run, or kept from one before.
            if (id.starts_with('@') || Impl::Generated(id)) return _impl->Asset(id);
            if (!id.starts_with('$')) return nullptr;
            if (auto mesh = _impl->DefaultMesh(id.substr(1))) return mesh;
            return _impl->DefaultImage(id.substr(1));
        }
        const auto out = node->outputs.find(output);
        return out == node->outputs.end() ? nullptr : out->second.resource;
    }

    kor::ResourceRef<const kor::Image> Runtime::ImageOf(const std::string_view id, const std::string_view output) const
    {
        return ImageRefOf(ResourceOf(id, output).get());
    }

    Value Runtime::ValueOf(const std::string_view id, const std::string_view output) const
    {
        // A call's output is what it is written out to; an edge of a function opened in place, what it stands for.
        if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) {
            const auto gives = call->second.gives.find(output);
            return gives == call->second.gives.end() || gives->second.node.empty() ? Value {} : ValueOf(gives->second.node, gives->second.output);
        }
        if (const auto edge = _impl->aliases.find(id); edge != _impl->aliases.end())
            return edge->second.link ? ValueOf(edge->second.link->node, edge->second.link->output) : edge->second.value;
        const auto* node = _impl->Find(id);
        if (!node) return {};
        const auto out = node->outputs.find(output);
        return out == node->outputs.end() ? Value {} : out->second.value;
    }

    namespace {
        std::string KindOf(const Node& node)
        {
            const auto kind = node.inputs.find("kind");
            return kind != node.inputs.end() && kind->second.value.String() ? *kind->second.value.String() : std::string("eValue");
        }
    }

    std::vector<Port> Runtime::PortsOf(const std::string_view id) const
    {
        const Node* node = _graph.Find(id);
        if (!node) return {};
        std::vector<Port> ports;
        if (const NodeType* type = FindType(node->type)) ports = PortsFor(*type, *node);
        // A call takes what its function's inputs are; an edge, a `value` of its kind.
        if (node->type == "Function") {
            if (const auto call = _impl->calls.find(id); call != _impl->calls.end()) ports.insert(ports.end(), call->second.inputs.begin(), call->second.inputs.end());
            return ports;
        }
        if (node->type == "FunctionInput" || node->type == "FunctionOutput") {
            for (auto& port : ports)
                if (port.name == "value") {
                    Port typed = FunctionPort("value", KindOf(*node), port.doc);
                    typed.optional = port.optional;
                    port = std::move(typed);
                }
            return ports;
        }
        const auto* made = _impl->Find(id);
        if (made && made->pipeline) ports.insert(ports.end(), made->pipeline->ports.begin(), made->pipeline->ports.end());
        if (made) ports.insert(ports.end(), made->ports.begin(), made->ports.end());
        // A ComputePass has its pipeline's besides: the parameters and constants it gives the pipeline written out of it.
        if (node->type == "ComputePass")
            if (const auto* pipeline = _impl->Find(std::string(id) + "/pipeline")) {
                std::vector<Port> its = pipeline->ports;
                if (pipeline->pipeline) its.insert(its.end(), pipeline->pipeline->ports.begin(), pipeline->pipeline->ports.end());
                for (const Port& port : its)
                    if ((port.name.starts_with("param.") || port.name.starts_with("constant.")) && !std::ranges::contains(ports, port.name, &Port::name)) ports.push_back(port);
            }
        return ports;
    }

    std::vector<Port> Runtime::OutputsOf(const std::string_view id) const
    {
        const Node* node = _graph.Find(id);
        if (!node) node = _running.Find(id);     // a node a call was written out into: `<call>/<node>`
        if (!node) return {};
        std::vector<Port> ports;
        // A call gives what its function's outputs are, each as the node inside gives it — and a Reference to a call, the
        // same; an edge, an `out` of its kind.
        if (node->type == "Function" || _impl->calls.contains(id)) {
            const auto call = _impl->calls.find(id);
            if (call == _impl->calls.end()) return ports;
            for (const auto& name : call->second.outputs) {
                Port port { name, PortKind::eValue, "value", false, {}, nullptr };
                if (const auto gives = call->second.gives.find(name); gives != call->second.gives.end() && !gives->second.node.empty())
                    for (const auto& inner : OutputsOf(gives->second.node))
                        if (inner.name == gives->second.output) { port = inner; port.name = name; break; }
                ports.push_back(std::move(port));
            }
            return ports;
        }
        if (node->type == "FunctionInput" || node->type == "FunctionOutput") return { FunctionPort("out", KindOf(*node), "What it stands for: what it is given.") };
        // An Unbundle gives each member of what it takes apart.
        if (node->type == "Unbundle") {
            // Each as what it is taken from gives it — a vector, a buffer, a mesh — and of a ForEach's component or `transform`,
            // only those shown by default on the node (Port::shown): the rest there to be asked for.
            const auto found = _impl->bundles.find(std::format("{}.of", id));
            if (found != _impl->bundles.end())
                for (const auto& [name, input] : found->second) {
                    Port port { name, PortKind::eValue, "value", false, std::format("The member '{}'.", name), nullptr };
                    if (input.link && input.link->node != id)
                        for (const Port& given : OutputsOf(input.link->node))
                            if (given.name == input.link->output) {
                                port = given;
                                port.name = name;
                                port.optional = !given.group.empty() && !given.shown;
                                port.group.clear();
                                break;
                            }
                    ports.push_back(std::move(port));
                }
            // Of a struct only as the graph runs: its members as it last was.
            if (ports.empty())
                if (const auto* made = _impl->Find(id))
                    for (const auto& name : made->outputs | std::views::keys) ports.push_back({ name, PortKind::eValue, "value", false, std::format("The member '{}'.", name), nullptr });
            return ports;
        }
        // What a node says of itself comes first: an Entities node is its component's fields before anything else.
        if (const auto* made = _impl->Find(id)) ports = made->gives;
        if (const NodeType* type = FindType(node->type)) ports.insert(ports.end(), type->outputs.begin(), type->outputs.end());
        return ports;
    }

    std::vector<std::string> Runtime::BundleOf(const std::string_view id, const std::string_view output) const
    {
        std::vector<std::string> names;
        const auto found = _impl->bundles.find(std::format("{}.{}", id, output));
        if (found != _impl->bundles.end())
            for (const auto& name : found->second | std::views::keys) names.push_back(name);
        return names;
    }

    std::vector<std::string> Runtime::PerEntityParameters(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        return node && node->pipeline ? node->pipeline->perEntity : std::vector<std::string> {};
    }

    kecs::ComponentId Runtime::ComponentOf(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        return node && node->status.ok && node->pipeline ? node->pipeline->component : kecs::NoComponent;
    }

    std::vector<std::string> Runtime::MaterialsOf(const std::string_view id) const
    {
        std::vector<std::string> kinds;
        const auto* node = _impl->Find(id);
        if (!node || !node->status.ok || !node->pipeline) return kinds;
        for (const auto& slot : node->pipeline->slots)
            if (slot.instancing)
                for (const auto& field : slot.instancing->fields)
                    if (field.Material() && !std::ranges::contains(kinds, field.kind)) kinds.push_back(field.kind);
        return kinds;
    }

    const kecs::Migration* Runtime::MigrationOf(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        return node && node->migration ? &*node->migration : nullptr;
    }

    std::vector<std::pair<kecs::Entity, std::string>> Runtime::PassedBy() const
    {
        std::vector<std::pair<kecs::Entity, std::string>> all;
        for (const auto& id : _impl->wanted) {
            const auto it = _impl->built.find(id);
            if (it == _impl->built.end() || !it->second.pass || !it->second.pass->entities) continue;
            for (const auto& [entity, why] : it->second.pass->entities->report.passedBy) all.emplace_back(entity, std::format("{}: {}", id, why));
        }
        return all;
    }

    void Runtime::SetResourceScript(ResourceScript script)
    {
        Impl::Scripted& at = _impl->scripts[script.name];
        if (script.size == 0)
            for (const auto& field : script.fields) script.size = std::max(script.size, field.offset + field.size);
        at.script = std::move(script);
        at.generation = ++_impl->scriptsVersion;
    }

    void Runtime::RemoveResourceScript(const std::string_view name)
    {
        if (const auto it = _impl->scripts.find(name); it != _impl->scripts.end()) {
            _impl->scripts.erase(it);
            ++_impl->scriptsVersion;
        }
    }

    std::vector<ResourceScript> Runtime::ResourceScripts() const
    {
        std::vector<ResourceScript> out;
        const Impl* from = _impl.get();
        while (from->source) from = from->source;
        for (const auto& each : from->scripts | std::views::values) out.push_back(each.script);
        return out;
    }

    kecs::ComponentId Runtime::DeclareMaterialKind(MaterialKind kind, std::string* error)
    {
        kecs::World* world = _impl->world;
        if (!world) { if (error) *error = "there is no world to make the kind a component of"; return kecs::NoComponent; }
        const kecs::ComponentId existing = world->Find(kind.name);
        const auto known = std::ranges::find(_impl->materialKinds, kind.name, &MaterialKind::name);
        if (existing != kecs::NoComponent && known == _impl->materialKinds.end()) {
            if (error) *error = std::format("there is a component named '{}' already, which is no material kind: the kind needs another name", kind.name);
            return kecs::NoComponent;
        }
        // What a new one's pictures start as: found now, as a stand-in until a file is read.
        std::vector<Held> starting;
        for (const auto& [field, picture] : kind.textures) starting.push_back(ResourceOf(picture));
        kecs::ComponentDescription description = MaterialComponentOf(kind, std::move(starting));
        kecs::ComponentId id = existing;
        if (existing == kecs::NoComponent) id = world->Register(std::move(description));
        else {
            try { (void)world->Reshape(existing, std::move(description)); }
            catch (const std::exception& e) { if (error) *error = e.what(); return kecs::NoComponent; }
        }
        if (known != _impl->materialKinds.end()) *known = std::move(kind);
        else {
            _impl->materialKinds.push_back(std::move(kind));
            std::ranges::sort(_impl->materialKinds, {}, &MaterialKind::name);
        }
        _impl->WriteMaterials();
        // What a shader takes of a kind is worked out as its pipeline is built — a pipeline node's, or a pass's own: the
        // graph is built again, the next frame. A kind is declared seldom: when a project's scripts are read.
        for (auto& built : _impl->built | std::views::values) built.signature.clear();
        _impl->stale = true;
        return id;
    }

    const std::vector<MaterialKind>& Runtime::MaterialKinds() const { return _impl->materialKinds; }

    const MaterialKind* Runtime::MaterialKindOf(const std::string_view name) const
    {
        const auto it = std::ranges::find(_impl->materialKinds, name, &MaterialKind::name);
        return it == _impl->materialKinds.end() ? nullptr : &*it;
    }

    void Runtime::SetGeneratedShaderDirectory(std::filesystem::path directory, const bool own)
    {
        if (directory == _impl->generatedShaders) return;
        kor::Shader::AddSearchPath(directory);
        _impl->generatedShaders = std::move(directory);
        _impl->generatedOwned = own;
        _impl->WriteMaterials();
    }

    const std::filesystem::path& Runtime::GeneratedShaderDirectory() const { return _impl->generatedShaders; }

    const std::vector<std::string>& Runtime::MaterialNotes() const
    {
        _impl->WriteMaterialsNow();
        return _impl->materialNotes;
    }

    std::vector<PassUse> Runtime::UsesOf(const std::string_view id, const std::string_view output) const
    {
        std::vector<PassUse> uses;
        const auto resource = ResourceOf(id, output);
        const void* of = resource ? MemberOf(resource.get()).id : nullptr;
        if (!of) return uses;
        for (const auto& pass : _impl->wanted) {
            if (pass.starts_with('$')) continue;    // a watch's copy, which is no node of the graph's
            const auto& plan = _impl->built.at(pass).pass;
            PassUse use { pass, {}, false, false };
            for (const auto& each : plan->uses)
                if (each.what.id == of) { use.read = use.read || each.read; use.write = use.write || each.write; }
            if (!use.read && !use.write) continue;
            if (const Node* node = _impl->current ? _impl->CurrentNode(pass) : nullptr) use.pass = node->pass;
            uses.push_back(std::move(use));
        }
        return uses;
    }

    const PassReport* Runtime::ReportOf(const std::string_view id) const
    {
        const auto* node = _impl->Find(id);
        if (!node || !node->status.ok || !node->pass || !node->pass->entities) return nullptr;
        // One that draws once is no report of entities — unless it could not draw, and says why.
        const auto& run = *node->pass->entities;
        return run.each.empty() && run.report.skipped == 0 ? nullptr : &run.report;
    }

    void Runtime::ReachedFromTheCpu(const std::string_view id)
    {
        if (!_impl->reachedFromTheCpu.emplace(id).second) return;
        // Made where only passes reach it: made again where the CPU does.
        if (const auto it = _impl->built.find(std::string(id)); it != _impl->built.end() && it->second.automaticType == "eDeviceLocal") {
            it->second.signature.clear();
            _impl->stale = true;
        }
    }

    std::vector<std::string> Runtime::UsageOf(const std::string_view id) const
    {
        const Node* node = _running.Find(id);
        const auto it = _impl->built.find(std::string(id));
        const kb::Enumeration* usages = node && Impl::MadeForItsUse(node->type) ? Impl::UsagesOf(node->type) : nullptr;
        if (!usages || it == _impl->built.end() || !it->second.status.ok) return {};
        std::vector<std::string> names;
        for (const auto& entry : usages->values)
            if (entry.value != 0 && (it->second.madeFor & entry.value) == entry.value) names.emplace_back(entry.name);
        return names;
    }

    std::optional<Runtime::Batching> Runtime::BatchingOf(const std::string_view id) const
    {
        const Node* node = _running.Find(id);
        if (!node || (node->type != "Draw" && node->type != "Dispatch")) return std::nullopt;
        const auto rowsOf = [&](const std::string& filter) -> std::size_t {
            const auto* built = _impl->Find(filter);
            return built && built->listed ? built->listed->Rows() : 0;
        };
        const auto* built = _impl->Find(id);
        const EntityRun* run = built && built->status.ok && built->pass ? built->pass->entities.get() : nullptr;
        Batching batching;
        if (node->type == "Dispatch") {
            if (run && !run->batchOf.empty()) {
                batching.batched = true;
                batching.elements = rowsOf(run->batchOf);
                return batching;
            }
            if (!run || run->each.empty()) return std::nullopt;
            batching.elements = run->report.ran + run->report.skipped;
            if (batching.elements < 2) return std::nullopt;
            batching.why = std::format("wired to a Filter's entities ('entities'), it would run once for all of them, one after another along z, each "
                                       "element SV_GroupID.z: what it is wired of each of '{}' its shader would read from its [entity(\"PER_INSTANCE\")] buffer "
                                       "instead, and the images each holds by their bindless indices (TEXTURE, STORAGE)", run->each);
            return batching;
        }
        if (_impl->batched.contains(id)) {
            batching.batched = true;
            batching.elements = rowsOf(std::string(id) + "/batch");
            return batching;
        }
        // Of how many: what it drew one at a time; or, one that could not draw them so, the Filter or ForEach its mesh is of.
        const auto mesh = node->inputs.find("mesh");
        const Node* source = mesh != node->inputs.end() && mesh->second.link ? _impl->Origin(mesh->second.link->node) : nullptr;
        batching.elements = run && !run->each.empty() ? run->report.ran + run->report.culled + run->report.skipped : source ? rowsOf(source->id) : 0;
        if (batching.elements < 2 || !source || (source->type != "Filter" && source->type != "ForEach")) return std::nullopt;
        // Whether its shader reads each instance's from a buffer of them: then `batched` is all it takes.
        bool perInstance = false;
        if (const auto pipeline = node->inputs.find("pipeline"); pipeline != node->inputs.end() && pipeline->second.link)
            if (const Node* origin = _impl->Origin(pipeline->second.link->node))
                if (const auto* made = _impl->Find(origin->id); made && made->pipeline)
                    perInstance = std::ranges::any_of(made->pipeline->slots, [](const Slot& slot) { return slot.instancing != nullptr; });
        std::vector<std::string> each;
        const std::string why = WhyNotBatched(_running, *node, &each);
        if (const auto asked = _impl->unbatched.find(id); asked != _impl->unbatched.end()) {
            batching.why = asked->second;
        } else if (why.empty() && perInstance) {
            batching.offered = true;
        } else if (!each.empty() || why.empty()) {
            std::string list;
            for (const auto& name : each) list += std::format("{}'{}'", list.empty() ? "" : ", ", name);
            batching.why = std::format("batched, it would be one draw: {}its shader would read what each element is from a [entity(\"PER_INSTANCE\")] buffer — "
                                       "where it is as [entity(\"MATRIX\")], its own fields as they are", list.empty() ? std::string() : list + (each.size() == 1 ? ", which is each element's now — " : ", which are each element's now — "));
        } else {
            batching.why = why;
        }
        return batching;
    }

    std::vector<std::string> Runtime::TakeReset() { return std::exchange(_impl->reset, {}); }
}
