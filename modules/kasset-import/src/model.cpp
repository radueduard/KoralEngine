#include <kasset/import.h>
#include <kasset/meta.h>

#include <kecs/json.h>
#include <koralModelImport.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

namespace kasset
{
    namespace {
        /** Every attribute a file's mesh has, in the order it is read, added to @p found when not there: what can be chosen to be read. */
        void ListAttributes(const kmdl::Importer::Mesh& from, std::vector<Model::Attribute>& found)
        {
            const std::size_t vertices = from.positions.size();
            const auto add = [&](std::string semantic, const char* type, const std::uint32_t count) {
                if (std::ranges::find(found, semantic, &Model::Attribute::semantic) == found.end()) found.push_back({ std::move(semantic), type, count, true });
            };
            add("POSITION", "float", 3);
            if (from.normals && from.normals->size() == vertices) add("NORMAL", "float", 3);
            if (from.tangents && from.tangents->size() == vertices) add("TANGENT", "float", 3);
            if (from.bitangents && from.bitangents->size() == vertices) add("BITANGENT", "float", 3);
            const auto sets = [&](const auto& byChannel, const char* name, const std::uint32_t count) {
                std::vector<kor::u32> channels;
                for (const auto& [channel, values] : byChannel) if (values.size() == vertices) channels.push_back(channel);
                std::ranges::sort(channels);
                for (const kor::u32 channel : channels) add(channel == 0 ? std::string(name) : std::format("{}{}", name, channel), "float", count);
            };
            sets(from.vertexUVs, "UV", 2);
            sets(from.vertexColors, "COLOR", 3);
            if (from.boneData && from.boneData->first.size() == vertices && from.boneData->second.size() == vertices) {
                add("BONE_IDS", "uint", 4);
                add("BONE_WEIGHTS", "float", 4);
            }
        }

        VertexData VertexDataOf(const kmdl::Importer::Mesh& from, const MeshImport& import, std::vector<Model::Attribute>* found = nullptr)
        {
            // What the file has, said whether it is read or not: what can be chosen to be read.
            if (found) {
                const std::size_t before = found->size();
                ListAttributes(from, *found);
                for (std::size_t i = before; i < found->size(); ++i)
                    (*found)[i].read = (*found)[i].semantic == "POSITION" || std::ranges::find(import.leaveOut, (*found)[i].semantic) == import.leaveOut.end();
            }
            VertexData data;
            data.vertexCount = static_cast<std::uint32_t>(from.positions.size());
            data.buffers = import.buffers;
            const auto wanted = [&](const std::string& semantic) { return semantic == "POSITION" || std::ranges::find(import.leaveOut, semantic) == import.leaveOut.end(); };
            const auto add = [&](std::string semantic, const AttributeType type, const std::uint32_t count, auto&& word) {
                if (!wanted(semantic)) return;
                VertexAttribute attribute { std::move(semantic), type, count, {} };
                attribute.words.reserve(static_cast<std::size_t>(count) * data.vertexCount);
                for (std::uint32_t v = 0; v < data.vertexCount; ++v)
                    for (std::uint32_t c = 0; c < count; ++c) attribute.words.push_back(word(v, c));
                data.attributes.push_back(std::move(attribute));
            };
            const auto floats = [](const auto& list) { return [&list](const std::uint32_t v, const std::uint32_t c) { return std::bit_cast<std::uint32_t>(static_cast<float>(list[v][static_cast<int>(c)])); }; };
            add("POSITION", AttributeType::eFloat, 3, floats(from.positions));
            if (from.normals && from.normals->size() == data.vertexCount) add("NORMAL", AttributeType::eFloat, 3, floats(*from.normals));
            if (from.tangents && from.tangents->size() == data.vertexCount) add("TANGENT", AttributeType::eFloat, 3, floats(*from.tangents));
            if (from.bitangents && from.bitangents->size() == data.vertexCount) add("BITANGENT", AttributeType::eFloat, 3, floats(*from.bitangents));
            const auto sets = [&](const auto& byChannel, const char* name, const std::uint32_t count) {
                std::vector<kor::u32> channels;
                for (const auto& [channel, values] : byChannel) if (values.size() == data.vertexCount) channels.push_back(channel);
                std::ranges::sort(channels);
                for (const kor::u32 channel : channels)
                    add(channel == 0 ? std::string(name) : std::format("{}{}", name, channel), AttributeType::eFloat, count, floats(byChannel.at(channel)));
            };
            sets(from.vertexUVs, "UV", 2);
            sets(from.vertexColors, "COLOR", 3);
            if (from.boneData && from.boneData->first.size() == data.vertexCount && from.boneData->second.size() == data.vertexCount) {
                const auto& [weights, bones] = *from.boneData;
                add("BONE_IDS", AttributeType::eUInt, 4, [&](const std::uint32_t v, const std::uint32_t c) { return static_cast<std::uint32_t>(bones[v][static_cast<int>(c)]); });
                add("BONE_WEIGHTS", AttributeType::eFloat, 4, floats(weights));
            }
            // A buffer of what the mesh has not is no buffer.
            for (auto& buffer : data.buffers) std::erase_if(buffer, [&](const std::string& semantic) { return !data.Find(semantic); });
            std::erase_if(data.buffers, [](const auto& buffer) { return buffer.empty(); });
            if (from.indices) data.indices = *from.indices;
            else for (std::uint32_t v = 0; v < data.vertexCount; ++v) data.indices.push_back(v);
            return data;
        }
    }

    std::expected<ModelArtifact, std::string> ReadModel(const std::filesystem::path& file, const MeshImport& import)
    {
        const auto importer = kmdl::Importer::Load(file);
        if (!importer) return std::unexpected(std::format("'{}' could not be read as a model", file.generic_string()));
        auto scene = importer->LoadScene();
        ModelArtifact read;
        Model& model = read.model;
        // A picture as the file names it: from the model's folder, or whole.
        const auto picture = [](const std::optional<std::filesystem::path>& path) {
            if (!path || path->empty()) return std::string();
            return path->generic_string();
        };
        for (const auto& from : scene.materials) {
            ModelMaterial material;
            material.name = from.name;
            for (int i = 0; i < 4; ++i) material.albedo[i] = from.baseColorFactor[i];
            material.roughness = from.roughness;
            material.metallic = from.metallic;
            // The importer says how a material's opacity is used by its cutoff: 0 blended, 1 not used, between them cut out.
            material.blended = from.alphaCutoff <= 0.f;
            material.alphaCutoff = from.alphaCutoff > 0.f && from.alphaCutoff < 1.f ? from.alphaCutoff : 0.f;
            material.doubleSided = from.doubleSided != 0;
            material.albedoMap = picture(from.albedoTexturePath ? from.albedoTexturePath : from.diffuseTexturePath);
            material.normalMap = picture(from.normalTexturePath);
            material.roughnessMap = picture(from.roughnessTexturePath ? from.roughnessTexturePath : from.metallicTexturePath);
            // glTF's metallic factor is 1 when a file says none, and so is its roughness: a material that says neither and
            // has no picture of them — a decal, a plain colour — is taken as what was meant, something not metal, rather
            // than as a mirror. One that gives either, or a picture, is as it says.
            if (material.roughnessMap.empty() && material.metallic >= 1.f && material.roughness >= 1.f) material.metallic = 0.f;
            material.emissiveMap = picture(from.emissiveTexturePath);
            // A file's emissive factor is what its emissive picture is multiplied by: with no picture, there is none.
            if (!material.emissiveMap.empty()) for (int i = 0; i < 3; ++i) material.emission[i] = from.emissiveFactor[i];
            material.occlusionMap = picture(from.ambientOcclusionTexturePath);
            // Described the classic way: its own colours, and what is not physical of it.
            if (from.model == kmdl::Importer::Material::Model::eClassic) {
                material.model = ModelMaterial::Model::eClassic;
                for (int i = 0; i < 4; ++i) material.diffuse[i] = from.diffuseColor[i];
                for (int i = 0; i < 3; ++i) material.specular[i] = from.specularColor[i];
                material.shininess = from.shininess;
                material.specularMap = picture(from.specularTexturePath);
                material.shininessMap = picture(from.shininessTexturePath);
                material.alphaMap = picture(from.alphaTexturePath);
            }
            model.materials.push_back(std::move(material));
        }
        kor::Vec3 lower(1e30f), upper(-1e30f);
        model.meshNames.resize(scene.meshes.size());
        for (std::size_t i = 0; i < scene.meshes.size(); ++i) {
            auto& from = scene.meshes[i];
            model.meshNames[i] = from.name;
            for (const auto& position : from.positions) { lower = kor::Min(lower, position); upper = kor::Max(upper, position); }
            VertexData data = VertexDataOf(from, import, &model.attributes);
            const std::string called = from.name;
            from = {};
            if (data.vertexCount == 0 || data.indices.empty()) continue;
            read.meshes.push_back({ static_cast<int>(i), called, std::move(data) });
        }
        for (int axis = 0; axis < 3; ++axis) { model.boundsMin[axis] = lower.x <= upper.x ? lower[axis] : 0.f; model.boundsMax[axis] = lower.x <= upper.x ? upper[axis] : 0.f; }
        // The things in it, parents before their children.
        std::vector<int> nodeOf(scene.nodes.size(), -1);   // the file's node's index among model.nodes
        if (!scene.nodes.empty()) {
            std::vector<std::pair<kor::u32, int>> next { { 0, -1 } };
            std::vector<bool> seen(scene.nodes.size(), false);
            for (std::size_t at = 0; at < next.size(); ++at) {
                const auto [index, parent] = next[at];
                if (index >= scene.nodes.size() || seen[index]) continue;
                seen[index] = true;
                const auto& from = scene.nodes[index];
                ModelNode node;
                node.name = from.name;
                node.parent = parent;
                for (int axis = 0; axis < 3; ++axis) { node.position[axis] = from.position[axis]; node.rotation[axis] = from.rotation[axis]; node.scale[axis] = from.scale[axis]; }
                for (std::size_t part = 0; part < from.meshIndices.size(); ++part)
                    node.parts.push_back({ std::format("#{}", from.meshIndices[part]), part < from.materialIndices.size() ? static_cast<int>(from.materialIndices[part]) : -1 });
                const int self = static_cast<int>(model.nodes.size());
                nodeOf[index] = self;
                model.nodes.push_back(std::move(node));
                for (const kor::u32 child : from.childIndices) next.emplace_back(child, self);
            }
        }
        static constexpr float Degrees = 180.f / 3.14159265358979323846f;
        for (const auto& from : scene.lights) {
            ModelLight light;
            light.name = from.name;
            light.kind = from.type == kmdl::Importer::Light::Type::eDirectional ? ModelLight::Kind::eDirectional
                       : from.type == kmdl::Importer::Light::Type::eSpot ? ModelLight::Kind::eSpot : ModelLight::Kind::ePoint;
            for (int axis = 0; axis < 3; ++axis) { light.position[axis] = from.position[axis]; light.direction[axis] = from.direction[axis]; light.color[axis] = from.color[axis]; }
            light.intensity = from.intensity;
            light.range = from.range;
            light.innerAngle = from.innerConeAngle * Degrees;
            light.outerAngle = from.outerConeAngle * Degrees;
            light.node = from.node >= 0 && static_cast<std::size_t>(from.node) < nodeOf.size() ? nodeOf[static_cast<std::size_t>(from.node)] : -1;
            light.atNode = from.atNode && light.node >= 0;
            model.lights.push_back(std::move(light));
        }
        for (const auto& from : scene.cameras) {
            ModelCamera camera;
            camera.name = from.name;
            for (int axis = 0; axis < 3; ++axis) { camera.position[axis] = from.position[axis]; camera.direction[axis] = from.direction[axis]; camera.up[axis] = from.up[axis]; }
            camera.orthographic = from.orthographic;
            camera.fieldOfView = from.verticalFieldOfView * Degrees;
            camera.height = from.orthographicHeight;
            camera.aspect = from.aspect;
            camera.zNear = from.zNear;
            camera.zFar = from.zFar;
            camera.node = from.node >= 0 && static_cast<std::size_t>(from.node) < nodeOf.size() ? nodeOf[static_cast<std::size_t>(from.node)] : -1;
            camera.atNode = from.atNode && camera.node >= 0;
            model.cameras.push_back(std::move(camera));
        }
        return read;
    }


    Key ModelKey(const std::filesystem::path& file, const MeshImport& import)
    {
        Key key(ModelProducer);
        key.AddFile(file);
        // What it names beside it that its meshes and materials are read from — a glTF's buffers, an OBJ's materials. Its
        // pictures are not read, only named: a picture changed is the same model.
        std::error_code failed;
        for (const auto& beside : NamedBeside(file)) {
            const std::filesystem::path path = file.parent_path() / beside;
            const auto kind = KindOfFile(path);
            if (kind == Kind::eTexture) continue;
            key.Add(beside.generic_string());
            if (std::filesystem::exists(path, failed)) key.AddFile(path);
            else key.Add("missing");
        }
        for (const auto& semantic : import.leaveOut) key.Add("leaveOut").Add(semantic);
        for (const auto& buffer : import.buffers) {
            key.Add("buffer");
            for (const auto& semantic : buffer) key.Add(semantic);
        }
        return key;
    }

    std::expected<ModelArtifact, std::string> ImportModel(const std::filesystem::path& file, const MeshImport& import, const Cache* cache)
    {
        if (!cache) return ReadModel(file, import);
        const Key key = ModelKey(file, import);
        if (const auto bytes = cache->Read("models", key, ".kmodel"))
            if (auto kept = DeserializeModel(*bytes)) return kept;     // one not read is made again, in its place
        auto read = ReadModel(file, import);
        // Not kept is only made again next time: what was read is as good.
        if (read) (void)cache->Write("models", key, ".kmodel", Serialize(*read));
        return read;
    }
}
