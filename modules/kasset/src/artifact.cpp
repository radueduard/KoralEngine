#include <kasset/artifact.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace kasset
{
    namespace {
        constexpr char Magic[4] { 'K', 'M', 'D', 'L' };
        constexpr char MeshMagic[4] { 'K', 'M', 'S', 'H' };
        /// Goes up whenever what is written changes: one of another version is not read, and is made again.
        constexpr std::uint32_t Version = 2;

        struct Writer {
            std::vector<std::byte> out;

            template <typename T> requires std::is_trivially_copyable_v<T>
            void Raw(const T& value)
            {
                const auto* bytes = reinterpret_cast<const std::byte*>(&value);
                out.insert(out.end(), bytes, bytes + sizeof(T));
            }
            void Text(const std::string& text) { Raw(static_cast<std::uint32_t>(text.size())); out.insert(out.end(), reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()); }
            template <typename T> requires std::is_trivially_copyable_v<T>
            void List(const std::vector<T>& list)
            {
                Raw(static_cast<std::uint32_t>(list.size()));
                const auto* bytes = reinterpret_cast<const std::byte*>(list.data());
                out.insert(out.end(), bytes, bytes + list.size() * sizeof(T));
            }
            void Texts(const std::vector<std::string>& texts) { Raw(static_cast<std::uint32_t>(texts.size())); for (const auto& text : texts) Text(text); }
        };

        struct Reader {
            std::span<const std::byte> in;
            std::size_t at = 0;
            bool failed = false;

            bool Has(const std::size_t size) { if (failed || in.size() - at < size) failed = true; return !failed; }
            template <typename T> requires std::is_trivially_copyable_v<T>
            void Raw(T& value) { if (Has(sizeof(T))) { std::memcpy(&value, in.data() + at, sizeof(T)); at += sizeof(T); } }
            std::uint32_t Count() { std::uint32_t n = 0; Raw(n); return n; }
            void Text(std::string& text)
            {
                const std::uint32_t n = Count();
                if (!Has(n)) return;
                text.assign(reinterpret_cast<const char*>(in.data() + at), n);
                at += n;
            }
            template <typename T> requires std::is_trivially_copyable_v<T>
            void List(std::vector<T>& list)
            {
                const std::uint32_t n = Count();
                if (!Has(static_cast<std::size_t>(n) * sizeof(T))) return;
                list.resize(n);
                std::memcpy(list.data(), in.data() + at, n * sizeof(T));
                at += n * sizeof(T);
            }
            void Texts(std::vector<std::string>& texts) { const std::uint32_t n = Count(); for (std::uint32_t i = 0; i < n && !failed; ++i) Text(texts.emplace_back()); }
        };

        // Each of the model's parts, the same way both ways: `io` is a Writer or a Reader, and `T` const or not with it.
        template <typename IO, typename T> void Material(IO& io, T& m)
        {
            io.Text(m.name); io.Raw(m.model); io.Raw(m.albedo); io.Raw(m.emission); io.Raw(m.roughness); io.Raw(m.metallic);
            io.Raw(m.diffuse); io.Raw(m.specular); io.Raw(m.shininess); io.Raw(m.alphaCutoff); io.Raw(m.blended); io.Raw(m.doubleSided);
            io.Text(m.albedoMap); io.Text(m.normalMap); io.Text(m.roughnessMap); io.Text(m.emissiveMap); io.Text(m.occlusionMap);
            io.Text(m.specularMap); io.Text(m.shininessMap); io.Text(m.alphaMap);
        }
        template <typename IO, typename T> void Light(IO& io, T& l)
        {
            io.Raw(l.kind); io.Text(l.name); io.Raw(l.position); io.Raw(l.direction); io.Raw(l.color);
            io.Raw(l.intensity); io.Raw(l.range); io.Raw(l.innerAngle); io.Raw(l.outerAngle); io.Raw(l.node); io.Raw(l.atNode);
        }
        template <typename IO, typename T> void Camera(IO& io, T& c)
        {
            io.Text(c.name); io.Raw(c.position); io.Raw(c.direction); io.Raw(c.up); io.Raw(c.orthographic); io.Raw(c.fieldOfView);
            io.Raw(c.height); io.Raw(c.aspect); io.Raw(c.zNear); io.Raw(c.zFar); io.Raw(c.node); io.Raw(c.atNode);
        }
        template <typename IO, typename T> void Attribute(IO& io, T& a) { io.Text(a.semantic); io.Text(a.type); io.Raw(a.count); io.Raw(a.read); }
        template <typename IO, typename T> void Data(IO& io, T& d)
        {
            io.Raw(d.vertexCount);
            if constexpr (std::is_const_v<T>) {
                io.Raw(static_cast<std::uint32_t>(d.attributes.size()));
                for (const auto& a : d.attributes) { io.Text(a.semantic); io.Raw(a.type); io.Raw(a.count); io.List(a.words); }
                io.List(d.indices);
                io.Raw(static_cast<std::uint32_t>(d.buffers.size()));
                for (const auto& buffer : d.buffers) io.Texts(buffer);
            } else {
                for (std::uint32_t i = 0, n = io.Count(); i < n && !io.failed; ++i) {
                    VertexAttribute& a = d.attributes.emplace_back();
                    io.Text(a.semantic); io.Raw(a.type); io.Raw(a.count); io.List(a.words);
                }
                io.List(d.indices);
                for (std::uint32_t i = 0, n = io.Count(); i < n && !io.failed; ++i) io.Texts(d.buffers.emplace_back());
            }
        }
    }

    std::vector<std::byte> Serialize(const ModelArtifact& artifact)
    {
        Writer w;
        w.Raw(Magic);
        w.Raw(Version);
        const Model& model = artifact.model;
        w.Raw(static_cast<std::uint32_t>(model.nodes.size()));
        for (const auto& node : model.nodes) {
            w.Text(node.name); w.Raw(node.parent); w.Raw(node.position); w.Raw(node.rotation); w.Raw(node.scale);
            w.Raw(static_cast<std::uint32_t>(node.parts.size()));
            for (const auto& part : node.parts) { w.Text(part.mesh); w.Raw(part.material); }
        }
        w.Raw(static_cast<std::uint32_t>(model.materials.size()));
        for (const auto& material : model.materials) Material(w, material);
        w.Raw(static_cast<std::uint32_t>(model.lights.size()));
        for (const auto& light : model.lights) Light(w, light);
        w.Raw(static_cast<std::uint32_t>(model.cameras.size()));
        for (const auto& camera : model.cameras) Camera(w, camera);
        w.Texts(model.meshNames);
        w.Raw(model.boundsMin); w.Raw(model.boundsMax);
        w.Raw(static_cast<std::uint32_t>(model.attributes.size()));
        for (const auto& attribute : model.attributes) Attribute(w, attribute);
        w.Raw(static_cast<std::uint32_t>(artifact.meshes.size()));
        for (const auto& mesh : artifact.meshes) { w.Raw(mesh.index); w.Text(mesh.name); Data(w, mesh.data); }
        return std::move(w.out);
    }

    std::expected<ModelArtifact, std::string> DeserializeModel(const std::span<const std::byte> bytes)
    {
        Reader r { bytes };
        char magic[4] {};
        std::uint32_t version = 0;
        r.Raw(magic);
        r.Raw(version);
        if (r.failed || std::memcmp(magic, Magic, 4) != 0) return std::unexpected("it is no model artifact");
        if (version != Version) return std::unexpected("it is a model artifact of another version");
        ModelArtifact artifact;
        Model& model = artifact.model;
        for (std::uint32_t i = 0, n = r.Count(); i < n && !r.failed; ++i) {
            ModelNode& node = model.nodes.emplace_back();
            r.Text(node.name); r.Raw(node.parent); r.Raw(node.position); r.Raw(node.rotation); r.Raw(node.scale);
            for (std::uint32_t p = 0, parts = r.Count(); p < parts && !r.failed; ++p) { auto& part = node.parts.emplace_back(); r.Text(part.mesh); r.Raw(part.material); }
        }
        for (std::uint32_t i = 0, n = r.Count(); i < n && !r.failed; ++i) Material(r, model.materials.emplace_back());
        for (std::uint32_t i = 0, n = r.Count(); i < n && !r.failed; ++i) Light(r, model.lights.emplace_back());
        for (std::uint32_t i = 0, n = r.Count(); i < n && !r.failed; ++i) Camera(r, model.cameras.emplace_back());
        r.Texts(model.meshNames);
        r.Raw(model.boundsMin); r.Raw(model.boundsMax);
        for (std::uint32_t i = 0, n = r.Count(); i < n && !r.failed; ++i) Attribute(r, model.attributes.emplace_back());
        for (std::uint32_t i = 0, n = r.Count(); i < n && !r.failed; ++i) {
            auto& mesh = artifact.meshes.emplace_back();
            r.Raw(mesh.index); r.Text(mesh.name); Data(r, mesh.data);
        }
        if (r.failed) return std::unexpected("it is cut short");
        if (r.at != bytes.size()) return std::unexpected("it has more than a model artifact");
        return artifact;
    }

    std::vector<std::byte> Serialize(const VertexData& mesh)
    {
        Writer w;
        w.Raw(MeshMagic);
        w.Raw(Version);
        Data(w, mesh);
        return std::move(w.out);
    }

    std::expected<VertexData, std::string> DeserializeMesh(const std::span<const std::byte> bytes)
    {
        Reader r { bytes };
        char magic[4] {};
        std::uint32_t version = 0;
        r.Raw(magic);
        r.Raw(version);
        if (r.failed || std::memcmp(magic, MeshMagic, 4) != 0) return std::unexpected("it is no mesh artifact");
        if (version != Version) return std::unexpected("it is a mesh artifact of another version");
        VertexData mesh;
        Data(r, mesh);
        if (r.failed) return std::unexpected("it is cut short");
        if (r.at != bytes.size()) return std::unexpected("it has more than a mesh artifact");
        return mesh;
    }
}
