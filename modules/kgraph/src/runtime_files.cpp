// kgraph: the runtime's own, what is read from files and from the world: models, pictures, entities, buffers of them.

#include "runtime_impl.h"

namespace kgraph
{
    auto Runtime::Impl::Assets() const -> const std::filesystem::path&
    { return source ? source->Assets() : assetDirectory; }

    auto Runtime::Impl::FileNamed(const std::string_view name, const std::filesystem::path& directory) -> std::filesystem::path
    {
            std::filesystem::path path(name);
#ifdef _WIN32
            // A file named as WSL names a Windows drive, /mnt/w/..., is W:/... here: the same project opens on both.
            if (name.size() >= 6 && name.starts_with("/mnt/") && std::isalpha(static_cast<unsigned char>(name[5])) && (name.size() == 6 || name[6] == '/'))
                path = std::format("{}:/{}", static_cast<char>(std::toupper(static_cast<unsigned char>(name[5]))), name.substr(std::min<std::size_t>(name.size(), 7)));
#endif
            return path.is_absolute() || directory.empty() ? path : directory / path;
        }

    auto Runtime::Impl::AssetFile(const std::string_view name) const -> std::filesystem::path
    { return FileNamed(name, Assets()); }

    auto Runtime::Impl::AssetId(const std::filesystem::path& file, const int mesh) const -> std::string
    { return AssetName(file, Assets(), mesh); }

    auto Runtime::Impl::DefaultImage(const std::string_view name) -> Held
    {
            if (source) return source->DefaultImage(name);
            const std::string id = std::format("${}", name);
            if (const auto it = assets.find(id); it != assets.end()) return it->second;
            if (name != "white" && name != "black" && name != "normal") return nullptr;
            const std::array<kor::U8Vec4, 1> texel { name == "white" ? kor::U8Vec4(255, 255, 255, 255) : name == "black" ? kor::U8Vec4(0, 0, 0, 255) : kor::U8Vec4(128, 128, 255, 255) };
            auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(1, 1))
                .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                .SetData(std::span<const kor::U8Vec4>(texel)).Build();
            if (!image.Valid()) return nullptr;
            Held held = MakeResource(std::move(image));
            held->node = id;
            return assets.emplace(id, std::move(held)).first->second;
        }

    auto Runtime::Impl::Together(std::vector<std::pair<std::string, VertexData>> meshes, const std::map<std::string, std::string>& names) -> Arrived
    {
            std::map<std::string, Whole> wholes;
            for (auto& [id, data] : meshes) {
                Whole& whole = wholes[LayoutKey(data)];
                if (whole.parts.empty()) {
                    whole.all.buffers = data.buffers;
                    for (const auto& attribute : data.attributes) whole.all.attributes.push_back({ attribute.semantic, attribute.type, attribute.count, {} });
                }
                Held part = std::make_shared<Resource>();
                part->kind = "Mesh";
                part->node = id;
                part->part = true;
                if (const auto called = names.find(id); called != names.end()) part->label = called->second;
                part->firstIndex = static_cast<std::uint32_t>(whole.all.indices.size());
                part->indexCount = static_cast<std::uint32_t>(data.indices.size());
                part->vertexOffset = static_cast<std::int32_t>(whole.all.vertexCount);
                part->vertexCount = data.vertexCount;
                Bound(*part, data, 0, data.vertexCount);
                for (std::size_t a = 0; a < data.attributes.size(); ++a) {
                    auto& words = whole.all.attributes[a].words;
                    words.insert(words.end(), data.attributes[a].words.begin(), data.attributes[a].words.end());
                }
                whole.all.indices.insert(whole.all.indices.end(), data.indices.begin(), data.indices.end());
                whole.all.vertexCount += data.vertexCount;
                data = {};
                whole.parts.push_back(std::move(part));
            }
            Arrived arrived;
            // Laid out as the device will hold it here, off the frame: the frame it arrives in only makes the buffers.
            for (auto& whole : wholes | std::views::values) {
                whole.ready = Interleave(whole.all);
                whole.all = {};
                arrived.wholes.push_back(std::move(whole));
            }
            return arrived;
        }

    auto Runtime::Impl::CacheBeside(const std::filesystem::path& assetDirectory) -> std::optional<kasset::Cache>
    {
            if (assetDirectory.empty()) return std::nullopt;
            return kasset::Cache(kasset::CacheDirectoryOf(assetDirectory.parent_path()));
        }

    auto Runtime::Impl::Read(const std::filesystem::path& file, const std::filesystem::path& assetDirectory, const MeshImport& import) -> std::optional<ReadModel>
    {
            const auto named = [&](const std::filesystem::path& path, const int mesh) { return AssetName(path, assetDirectory, mesh); };
            const auto cache = CacheBeside(assetDirectory);
            auto artifact = kasset::ImportModel(file, import, cache ? &*cache : nullptr);
            if (!artifact) return std::nullopt;
            ReadModel read;
            read.model = std::make_shared<Model>(std::move(artifact->model));
            const std::filesystem::path beside = file.parent_path();
            for (ModelMaterial& material : read.model->materials)
                for (std::string* picture : { &material.albedoMap, &material.normalMap, &material.roughnessMap, &material.emissiveMap, &material.occlusionMap,
                                              &material.specularMap, &material.shininessMap, &material.alphaMap })
                    if (!picture->empty()) {
                        const std::filesystem::path path(*picture);
                        *picture = named(path.is_absolute() ? path : beside / path, -1);
                    }
            for (ModelNode& node : read.model->nodes)
                for (auto& part : node.parts)
                    if (part.mesh.starts_with('#')) part.mesh = named(file, std::stoi(part.mesh.substr(1)));
            for (auto& mesh : artifact->meshes) {
                read.names[named(file, mesh.index)] = mesh.name;
                read.meshes.emplace_back(named(file, mesh.index), std::move(mesh.data));
            }
            return read;
        }

    auto Runtime::Impl::ReadModelFile(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string key, std::filesystem::path file,
                                             std::filesystem::path assetDirectory, MeshImport import) -> kor::Task<void>
    {
            co_await kor::Context::SwitchToBackgroundThread();
            auto read = Read(file, assetDirectory, import);
            std::optional<Arrived> together;
            if (read) together = Together(std::move(read->meshes), read->names);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            self->readingModels.erase(key);
            // How it is to be read changed while it was being read: what was read is not that, and it is read again.
            if (self->rereadModels.erase(key)) { self->WantModel(file); co_return; }
            if (!read) { self->models[key] = nullptr; self->unread[key] = std::format("'{}' could not be read as a model", key); co_return; }
            self->arrivedMeshes.push_back(std::move(*together));
            self->models[key] = std::move(read->model);
        }

    auto Runtime::Impl::ProbeModelFile(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string key, std::filesystem::path file) -> kor::Task<void>
    {
            const auto cache = CacheBeside(self->Assets());
            co_await kor::Context::SwitchToBackgroundThread();
            // Read as it is by default — every attribute — which is what it is most likely imported as next.
            auto artifact = kasset::ImportModel(file, {}, cache ? &*cache : nullptr);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            if (!artifact) {
                self->probed[key] = std::nullopt;
                self->unread[key] = artifact.error();
                co_return;
            }
            self->probed[key] = std::move(artifact->model.attributes);
        }

    auto Runtime::Impl::ReadPicture(Impl* self, std::shared_ptr<std::atomic<bool>> alive, std::string id, std::filesystem::path file, Held standIn,
                                           const TextureFormat format) -> kor::Task<void>
    {
            kor::Resource<kor::Image> image = co_await kimg::LoadImageAsync(file, true);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            if (!image.Valid()) { self->unread[id] = std::format("'{}' could not be read as a picture", file.generic_string()); co_return; }
            self->unread.erase(id);
            self->PictureArrived(id, standIn, Spread(MakeResource(std::move(image)), format));
        }

    auto Runtime::Impl::TextureCache() const -> std::filesystem::path
    { return Assets().empty() ? std::filesystem::path() : kasset::CacheDirectoryOf(Assets().parent_path()) / "textures"; }

    auto Runtime::Impl::FileOf() const -> cook::FileOf
    {
            return [directory = Assets()](const std::string_view id) { return FileNamed(id.starts_with('@') ? id.substr(1) : id, directory); };
        }

    auto Runtime::Impl::RecipeOf(const std::string_view id) const -> cook::Recipe
    {
            cook::Recipe recipe { std::string(id) };
            if (const auto pack = texturePacks.find(id); pack != texturePacks.end()) {
                recipe.format = pack->second.format;
                recipe.quality = pack->second.quality;
                recipe.pack = pack->second;
            } else if (const auto import = textureImports.find(id); import != textureImports.end() && TextureCompressible(id)) {
                recipe.format = import->second.format;
                recipe.quality = import->second.quality;
            }
            return recipe;
        }

    auto Runtime::Impl::Cooked(const std::string_view id) const -> bool
    {
            const cook::Recipe recipe = RecipeOf(id);
            if (recipe.format == TextureFormat::eAsRead) return false;
            const auto cooked = cook::CookedPath(TextureCache(), recipe, FileOf());
            std::error_code ec;
            return cooked && std::filesystem::exists(*cooked, ec);
        }

    auto Runtime::Impl::ViewOf(const Held& image, const kor::ImageView::ComponentMapping& mapping) -> Held
    {
            if (!image || image->kind != "Image" || !image->handle) return image;
            auto view = kor::ImageView::Builder(kor::ResourceRef<const kor::Image>(kor::capi::ImageOf(image->handle))).SetComponentMapping(mapping).Build();
            if (!view.Valid()) return image;
            Held held = MakeResource(std::move(view));
            held->source = image;
            return held;
        }

    auto Runtime::Impl::Spread(Held image, const TextureFormat format) -> Held
    {
            using S = kor::ImageView::Swizzle;
            if (format == TextureFormat::eBC4) return ViewOf(image, kor::ImageView::ComponentMapping { S::eR, S::eR, S::eR, S::eOne });
            if (format == TextureFormat::eBC5) return ViewOf(image, kor::ImageView::ComponentMapping { S::eR, S::eG, S::eOne, S::eOne });
            return image;
        }

    auto Runtime::Impl::Channel(const Held& pack, const int channel) -> Held
    {
            using S = kor::ImageView::Swizzle;
            // The pack may itself be a view (a BC4 or BC5 one): its picture's channels are what is spread.
            const Held& image = pack && pack->kind == "ImageView" && pack->source ? pack->source : pack;
            const S from = channel == 1 ? S::eG : channel == 2 ? S::eB : channel == 3 ? S::eA : S::eR;
            return ViewOf(image, kor::ImageView::ComponentMapping { from, from, from, S::eOne });
        }

    auto Runtime::Impl::Lead(const std::string& id, const Held& from, Held to) -> void
    {
            to->node = id;
            if (from && from != to) { from->replacedBy = to; ++replacements; arrivedPictures.push_back(from); }
            assets[id] = std::move(to);
        }

    auto Runtime::Impl::PictureArrived(const std::string& id, const Held& standIn, Held real) -> void
    {
            Lead(id, standIn, real);
            for (const auto& [member, in] : packedIn)
                if (in.first == id)
                    if (const auto held = assets.find(member); held != assets.end() && held->second) Lead(member, held->second, Channel(real, in.second));
        }

    auto Runtime::Impl::StartPicture(const std::string& id, const Held& standIn) -> void
    {
            if (const auto member = packedIn.find(id); member != packedIn.end()) {
                const Held pack = Asset(member->second.first);
                if (pack && !pack->pending) Lead(id, standIn, Channel(pack, member->second.second));
                return;     // the pack, when it arrives, leads it on
            }
            // Watched, whether it is read as it is or from what was cooked of it: written again, it is read again.
            if (!IsTexturePack(id)) Remember(AssetFile(id.substr(1)));
            const cook::Recipe recipe = RecipeOf(id);
            if (recipe.format != TextureFormat::eAsRead)
                if (const auto cooked = cook::CookedPath(TextureCache(), recipe, FileOf()); cooked && std::filesystem::exists(*cooked)) {
                    reading.push_back(ReadPicture(this, alive, id, *cooked, standIn, recipe.format));
                    return;
                }
            if (recipe.pack) { reading.push_back(ReadPack(this, alive, recipe, FileOf(), standIn)); return; }
            reading.push_back(ReadPicture(this, alive, id, AssetFile(id.substr(1)), standIn));
        }

    auto Runtime::Impl::ReadPack(Impl* self, std::shared_ptr<std::atomic<bool>> alive, cook::Recipe recipe, cook::FileOf fileOf, Held standIn) -> kor::Task<void>
    {
            co_await kor::Context::SwitchToBackgroundThread();
            auto pixels = cook::Pixels(recipe, fileOf);
            co_await kor::Context::SwitchToMainThread();
            if (!*alive) co_return;
            if (!pixels) { self->unread[recipe.id] = std::format("'{}': {}", recipe.id, pixels.error()); co_return; }
            kor::u32 levels = 1;
            for (kor::u32 size = std::max(pixels->extent.x, pixels->extent.y); size > 1; size /= 2) ++levels;
            auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(pixels->extent.x, pixels->extent.y))
                .SetMipLevels(levels).SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled))
                .SetData(pixels->pixels.data(), pixels->pixels.size()).Build();
            if (!image.Valid()) { self->unread[recipe.id] = std::format("'{}' could not be made a picture", recipe.id); co_return; }
            self->unread.erase(recipe.id);
            self->PictureArrived(recipe.id, standIn, MakeResource(std::move(image)));
        }

    auto Runtime::Impl::SetTextureImports(const std::map<std::string, TextureImport>& imports, const std::map<std::string, TexturePack>& packs) -> void
    {
            if (source) return;     // the source reads the files, as its own graph says
            std::set<std::string> changed;
            for (const auto& [id, import] : imports) if (const auto it = textureImports.find(id); it == textureImports.end() || it->second != import) changed.insert(id);
            for (const auto& id : textureImports | std::views::keys) if (!imports.contains(id)) changed.insert(id);
            const auto members = [&](const TexturePack& pack) { for (const auto& channel : pack.channels) if (!channel.from.empty()) changed.insert(channel.from); };
            for (const auto& [id, pack] : packs)
                if (const auto it = texturePacks.find(id); it == texturePacks.end() || it->second != pack) { changed.insert(id); members(pack); if (it != texturePacks.end()) members(it->second); }
            for (const auto& [id, pack] : texturePacks) if (!packs.contains(id)) { changed.insert(id); members(pack); }
            if (changed.empty()) return;
            textureImports = std::map<std::string, TextureImport, std::less<>>(imports.begin(), imports.end());
            texturePacks = std::map<std::string, TexturePack, std::less<>>(packs.begin(), packs.end());
            packedIn.clear();
            for (const auto& [id, pack] : texturePacks)
                for (int c = 0; c < 4; ++c)
                    if (const auto& from = pack.channels[static_cast<std::size_t>(c)].from; !from.empty()) packedIn.try_emplace(from, id, c);
            // Packs first: a file of one is its channel, and the pack read again leads it on.
            std::vector<std::string> order(changed.begin(), changed.end());
            std::ranges::stable_partition(order, [&](const std::string& id) { return texturePacks.contains(id); });
            for (const auto& id : order)
                if (const auto it = assets.find(id); it != assets.end() && it->second) StartPicture(id, it->second);
        }

    auto Runtime::Impl::CookAll(Impl* self, std::shared_ptr<std::atomic<bool>> alive) -> kor::Task<void>
    {
            while (*alive && !self->toCook.empty()) {
                cook::Recipe recipe = std::move(self->toCook.front());
                self->toCook.pop_front();
                self->cooking.current = recipe.id;
                const std::filesystem::path cache = self->TextureCache();
                const cook::FileOf fileOf = self->FileOf();
                co_await kor::Context::SwitchToBackgroundThread();
                auto cooked = cook::Cook(cache, recipe, fileOf);
                co_await kor::Context::SwitchToMainThread();
                if (!*alive) co_return;
                ++self->cooking.done;
                if (!cooked) { self->cooking.failed.push_back(cooked.error()); continue; }
                // Read again, from what was cooked — while it is still to be kept as it was cooked.
                if (const auto it = self->assets.find(recipe.id); it != self->assets.end() && it->second && self->RecipeOf(recipe.id) == recipe)
                    self->StartPicture(recipe.id, it->second);
            }
            self->cooking.current.clear();
        }

    auto Runtime::Impl::Cook(const std::vector<std::string>& ids) -> void
    {
            if (source) { source->Cook(ids); return; }
            if (cooking.done == cooking.total) cooking = {};
            for (const auto& id : ids) {
                cook::Recipe recipe = RecipeOf(id);
                if (recipe.format == TextureFormat::eAsRead || std::ranges::find(toCook, recipe) != toCook.end()) continue;
                toCook.push_back(std::move(recipe));
                ++cooking.total;
            }
            std::erase_if(cookingTasks, [](const kor::Task<void>& task) { return task.Done(); });
            if (cookingTasks.empty() && !toCook.empty()) cookingTasks.push_back(CookAll(this, alive));
        }

    auto Runtime::Impl::WrittenAt(const std::filesystem::path& file) -> std::optional<std::filesystem::file_time_type>
    {
            std::error_code failed;
            const auto written = std::filesystem::last_write_time(file, failed);
            if (failed) return std::nullopt;
            return written;
        }

    auto Runtime::Impl::Remember(const std::filesystem::path& file, const std::string& model) -> void
    {
            if (const auto written = WrittenAt(file)) stamps[file.lexically_normal().generic_string()] = { *written, model };
        }

    auto Runtime::Impl::Watch() -> void
    {
            if (source || stamps.empty()) return;
            const auto now = std::chrono::steady_clock::now();
            if (now - lookedAt < std::chrono::seconds(1)) return;
            lookedAt = now;
            std::set<std::string> models_, pictures;
            for (auto& [path, stamp] : stamps) {
                const auto written = WrittenAt(path);
                if (!written || *written == stamp.written) continue;
                stamp.written = *written;
                if (stamp.model.empty()) pictures.insert(path);
                else models_.insert(stamp.model);
            }
            for (const auto& key : models_) {
                if (readingModels.contains(key)) { rereadModels.insert(key); continue; }
                if (models.erase(key)) WantModel(key);
            }
            if (pictures.empty()) return;
            std::vector<std::pair<std::string, Held>> again;
            for (const auto& [id, held] : assets)
                if (held && id.starts_with('@') && !id.contains('#') && pictures.contains(AssetFile(id.substr(1)).lexically_normal().generic_string())) again.emplace_back(id, held);
            for (const auto& [id, held] : again) StartPicture(id, held);
        }

    auto Runtime::Impl::WantModel(const std::filesystem::path& file) -> void
    {
            if (source) { source->WantModel(file); return; }
            const std::string key = file.lexically_normal().generic_string();
            if (models.contains(key) || !readingModels.insert(key).second) return;
            // Watched, with what its meshes and materials are read from beside it.
            Remember(file, key);
            for (const auto& beside : kasset::NamedBeside(file))
                if (kasset::KindOfFile(beside) != kasset::Kind::eTexture) Remember(file.parent_path() / beside, key);
            const auto import = meshImports.find(AssetId(file, -1).substr(1));
            reading.push_back(ReadModelFile(this, alive, key, file, assetDirectory, import == meshImports.end() ? MeshImport {} : import->second));
        }

    auto Runtime::Impl::SetDefaultMeshTangents(const Tangents wanted) -> void
    {
            if (source || wanted == defaultMeshTangents) return;
            defaultMeshTangents = wanted;
            for (auto& [name, kept] : defaultMeshes) {
                const auto made = MeshOf(ToVertexData(*ShapeData(name), wanted));
                if (!made) continue;
                (*made)->node = kept->node;
                { kept->replacedBy = *made; ++replacements; }
                kept = *made;
            }
            for (auto& node : built | std::views::values)
                if (node.outputs.contains("layout") && node.outputs.contains("out")) { node.signature.clear(); stale = true; }
        }

    auto Runtime::Impl::SetMeshImports(const std::map<std::string, MeshImport>& wanted) -> void
    {
            if (source) return;     // the source reads the files, as its own graph says
            std::set<std::string> changed;
            for (const auto& [name, import] : wanted) if (const auto it = meshImports.find(name); it == meshImports.end() || it->second != import) changed.insert(name);
            for (const auto& name : meshImports | std::views::keys) if (!wanted.contains(name)) changed.insert(name);
            meshImports = std::map<std::string, MeshImport, std::less<>>(wanted.begin(), wanted.end());
            for (const auto& name : changed) {
                const std::filesystem::path file = AssetFile(name);
                const std::string key = file.lexically_normal().generic_string();
                if (readingModels.contains(key)) { rereadModels.insert(key); continue; }
                if (models.erase(key) == 0) continue;     // not read yet: it is read as it is now asked to be, when it is
                WantModel(file);
            }
        }

    auto Runtime::Impl::PictureOf(const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba) -> std::expected<Held, std::string>
    {
            if (width == 0 || height == 0) return std::unexpected("a picture has at least one texel");
            if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return std::unexpected(std::format("{} × {} texels are {} bytes, and {} were given", width, height, static_cast<std::size_t>(width) * height * 4, rgba.size()));
            kor::u32 levels = 1;
            for (kor::u32 size = std::max(width, height); size > 1; size /= 2) ++levels;
            auto image = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(width, height))
                .SetMipLevels(levels).SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled))
                .SetData(reinterpret_cast<const unsigned char*>(rgba.data()), rgba.size()).Build();
            if (!image.Valid()) return std::unexpected("the picture could not be made");
            return MakeResource(std::move(image));
        }

    auto Runtime::Impl::PictureBytes(const std::uint32_t width, const std::uint32_t height, const std::span<const std::byte> rgba) -> std::vector<std::byte>
    {
            std::vector<std::byte> bytes(8 + rgba.size());
            std::memcpy(bytes.data(), &width, 4);
            std::memcpy(bytes.data() + 4, &height, 4);
            std::memcpy(bytes.data() + 8, rgba.data(), rgba.size());
            return bytes;
        }

    auto Runtime::Impl::Generated(const std::string_view id) -> bool
    { return id.starts_with("$script/gen/"); }

    auto Runtime::Impl::GeneratedKey(const std::string_view id) -> kasset::Key
    { return kasset::Key("generated@1").Add(id); }

    auto Runtime::Impl::Asset(const std::string_view id) -> Held
    {
            if (source) return source->Asset(id);
            if (const auto it = assets.find(id); it != assets.end()) return it->second;
            // A generator's that nothing has made this run: as it was made before, from the cache.
            if (Generated(id)) {
                const auto cache = CacheBeside(Assets());
                const auto bytes = cache ? cache->Read("meshes", GeneratedKey(id), ".kmesh") : std::nullopt;
                if (!bytes) {
                    // Or a picture a generator made.
                    const auto picture = cache ? cache->Read("images", GeneratedKey(id), ".kimage") : std::nullopt;
                    if (!picture || picture->size() < 8) return nullptr;
                    std::uint32_t width = 0, height = 0;
                    std::memcpy(&width, picture->data(), 4);
                    std::memcpy(&height, picture->data() + 4, 4);
                    auto made = PictureOf(width, height, std::span(*picture).subspan(8));
                    if (!made) return nullptr;
                    (*made)->node = std::string(id);
                    return assets[std::string(id)] = std::move(*made);
                }
                auto data = kasset::DeserializeMesh(*bytes);
                if (!data) return nullptr;
                auto made = MeshOf(*data);
                if (!made) return nullptr;
                (*made)->node = std::string(id);
                (*made)->kind = "Mesh";
                return assets[std::string(id)] = std::move(*made);
            }
            if (!id.starts_with('@')) return nullptr;
            const std::string_view named = id.substr(1);
            Held standIn;
            if (const auto mesh = named.rfind('#'); mesh != std::string_view::npos) {
                standIn = std::make_shared<Resource>();
                standIn->kind = "Mesh";
                standIn->pending = true;
                WantModel(AssetFile(named.substr(0, mesh)));
            } else {
                const std::array<kor::U8Vec4, 1> white { kor::U8Vec4(255, 255, 255, 255) };
                auto texel = kor::Image::Builder().SetFormat(kor::Image::Format::eRGBA8_UNORM).SetExtent(kor::UVec2(1, 1))
                    .SetUsage(kor::Flags<kor::Image::Usage>(kor::Image::Usage::eSampled) | kor::Image::Usage::eTransferDst)
                    .SetData(std::span<const kor::U8Vec4>(white)).Build();
                if (!texel.Valid()) return nullptr;
                standIn = MakeResource(std::move(texel));
                standIn->pending = true;
            }
            standIn->node = std::string(id);
            assets.emplace(std::string(id), standIn);
            if (standIn->kind != "Mesh") StartPicture(std::string(id), standIn);
            return assets.find(id)->second;
        }

    auto Runtime::Impl::TransientsUpstream(const std::string& id, std::map<std::string, std::set<std::string>, std::less<>>& memo) const -> const std::set<std::string>&
    {
            if (const auto known = memo.find(id); known != memo.end()) return known->second;
            std::set<std::string>& found = memo[id];      // empty while it is looked into: a ring ends here
            const Node* node = current ? CurrentNode(id) : nullptr;
            if (!node) return found;
            std::set<std::string> all;
            const auto from = [&](const std::string& other) {
                if (transients.contains(other)) all.insert(other);
                const auto& further = TransientsUpstream(other, memo);
                all.insert(further.begin(), further.end());
            };
            for (const auto& input : node->inputs | std::views::values)
                if (input.link) from(input.link->node);
            if (node->type == "Reference")
                if (const auto target = node->inputs.find("node"); target != node->inputs.end() && target->second.value.String()) from(*target->second.value.String());
            found = std::move(all);
            return found;
        }

    auto Runtime::Impl::ArriveTransients() -> void
    {
            std::map<std::string, Member, std::less<>> made;
            {
                const std::scoped_lock lock(board->mutex);
                made = board->made;
            }
            for (auto it = transients.begin(); it != transients.end();) {
                const auto node = built.find(it->first);
                if (node == built.end() || !node->second.status.ok) {
                    { const std::scoped_lock lock(board->mutex); board->made.erase(it->first); }
                    it = transients.erase(it);
                    continue;
                }
                const auto fresh = made.find(it->first);
                if (fresh == made.end() || !fresh->second.id || fresh->second.id == it->second.made) { ++it; continue; }
                Held held = std::make_shared<Resource>();
                held->kind = fresh->second.image.Get() ? "Image" : "Buffer";
                held->handle = fresh->second.image.Get() ? kor::capi::BorrowImage(fresh->second.image) : kor::capi::BorrowBuffer(fresh->second.buffer);
                held->node = it->first;
                held->transient = true;
                Output& out = node->second.outputs["out"];
                if (out.resource) { out.resource->replacedBy = held; ++replacements; }
                out.resource = std::move(held);
                node->second.stamp = ++counter;      // what is wired from it is built again
                it->second.made = fresh->second.id;
                stale = true;
                ++it;
            }
        }

    auto Runtime::Impl::Recount() -> bool
    {
            bool changed = false;
            for (const auto& [repeat, link] : wiredCounts) {
                const auto source = built.find(link.node);
                if (source == built.end() || !source->second.status.ok) continue;
                const auto output = source->second.outputs.find(link.output);
                const double* number = output == source->second.outputs.end() ? nullptr : output->second.value.Number();
                if (!number || !std::isfinite(*number)) continue;
                const int count = static_cast<int>(std::clamp(std::llround(*number), -1ll, 257ll));
                const auto known = loopCounts.find(repeat);
                if ((known == loopCounts.end() ? 1 : known->second) == count) continue;
                loopCounts[repeat] = count;
                changed = true;
            }
            return changed;
        }

    auto Runtime::Impl::Arrive() -> void
    {
            ArriveTransients();
            std::erase_if(reading, [](const kor::Task<void>& task) { return task.Done(); });
            // A model's meshes are one mesh on the device, each a run of its indices: a pass that draws a hundred of
            // them binds one thing, where a mesh each would be bound a hundred times. Every model read since arrives
            // in the same frame: what packs their meshes (a MeshPack, its Meshlets, a RayScene) is built again once for
            // them all, where a model a frame built it again for each.
            // While other models are still being read, those read wait for them a moment — a model read alone at
            // first, the rest a moment after, was every pack built twice over.
            bool hold = false;
            if (!arrivedMeshes.empty() && !readingModels.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (!modelsWaiting) modelsWaiting = now;
                hold = now - *modelsWaiting < std::chrono::milliseconds(400);
            }
            if (!hold) modelsWaiting.reset();
            while (!hold && !arrivedMeshes.empty()) {
                Arrived arrived = std::move(arrivedMeshes.front());
                arrivedMeshes.pop_front();
                for (auto& whole : arrived.wholes) {
                    const auto made = MeshOf(std::move(whole.ready));
                    if (!made) {
                        for (const auto& part : whole.parts) unread[part->node] = std::format("'{}': {}", part->node, made.error());
                        continue;
                    }
                    for (auto& part : whole.parts) {
                        part->source = *made;
                        // What held the stand-in, or this mesh as the file was read before, holds this one from here on.
                        Held& kept = assets[part->node];
                        if (kept && kept != part) { kept->replacedBy = part; ++replacements; }
                        kept = std::move(part);
                    }
                }
            }
            // A node that made its output of a stand-in — an ImageFile — is built again, with the picture.
            if (arrivedPictures.empty()) return;
            for (auto& node : built | std::views::values)
                for (const auto& output : node.outputs | std::views::values)
                    if (std::ranges::find(arrivedPictures, output.resource) != arrivedPictures.end()) { node.signature.clear(); stale = true; }
            arrivedPictures.clear();
        }

    auto Runtime::Impl::IsEach(const std::string_view type) -> bool
    { return type == "ForEach" || type == "Entities" || type == "Filter" || type == "Entity"; }

    auto Runtime::Impl::IsMapped(const std::string_view type) -> bool
    { return type == "ShaderBuffer" || type == "BufferBuilder"; }

    auto Runtime::Impl::ReadsTheWorld(const std::string_view type) -> bool
    {
            return type == "Selected" || type == "Keep" || type == "LastFrame" || type == "SceneBounds" || type == "WriteComponent" || type == "DebugShapes" || type == "Readback" || type == "FixedStep" || type == "Time" || IsEach(type);
        }

    auto Runtime::Impl::Matrix(const kor::Mat4& m) -> Value
    {
            Value::List out;
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row) out.emplace_back(m[column][row]);
            return Value(std::move(out));
        }

    auto Runtime::Impl::Three(const kor::Vec3 v) -> Value
    { return Value(Value::List { v.x, v.y, v.z }); }

    auto Runtime::Impl::EntityFor(const Inputs& inputs, const kecs::ComponentId component) const -> kecs::Entity
    {
            if (!world) return kecs::NullEntity;
            if (const auto it = inputs.find("entity"); it != inputs.end() && it->second.value.String() && !it->second.value.String()->empty())
                return world->FindEntity(*it->second.value.String());
            if (component == kecs::NoComponent) return kecs::NullEntity;
            for (const kecs::Entity entity : world->All())
                if (world->Has(entity, component) && world->IsEnabled(entity, component)) return entity;
            return kecs::NullEntity;
        }

    auto Runtime::Impl::IsResourceField(const kecs::FieldDescription& field) -> bool
    {
            static const std::set<std::string, std::less<>> kinds { "resource", "Image", "Buffer", "Sampler", "BufferView", "AccelerationStructure", "Mesh" };
            return field.size == sizeof(Held) && kinds.contains(field.type);
        }

    auto Runtime::Impl::FieldValue(const kecs::FieldDescription& field, const std::byte* at) -> Value
    {
            const auto numbers = [&](const std::size_t count, const char kind) {
                Value::List out;
                for (std::size_t i = 0; i < count; ++i) {
                    if (kind == 'f') { float v; std::memcpy(&v, at + i * 4, 4); out.emplace_back(v); }
                    else if (kind == 'i') { std::int32_t v; std::memcpy(&v, at + i * 4, 4); out.emplace_back(v); }
                    else { std::uint32_t v; std::memcpy(&v, at + i * 4, 4); out.emplace_back(v); }
                }
                return count == 1 ? out.front() : Value(std::move(out));
            };
            const std::string& type = field.type;
            if (type == "f32") return numbers(1, 'f');
            if (type == "i32") return numbers(1, 'i');
            if (type == "u32" || type == "bool32") return numbers(1, 'u');
            if (type == "f64") { double v; std::memcpy(&v, at, 8); return Value(v); }
            if (type == "bool") return Value(static_cast<bool>(*at != std::byte { 0 }));
            if (type == "mat4") return numbers(16, 'f');
            if (type == "quat") return numbers(4, 'f');
            // Another entity, as a ForEach's entityId has it: its index and one; 0 for none.
            if (type == "entity") { kecs::Entity e; std::memcpy(&e, at, sizeof e); return Value(e.IsNull() ? 0.0 : static_cast<double>(e.index) + 1.0); }
            for (const auto& [prefix, kind] : { std::pair<std::string_view, char> { "vec", 'f' }, { "ivec", 'i' }, { "uvec", 'u' } })
                if (type.size() == prefix.size() + 1 && type.starts_with(prefix) && type.back() >= '2' && type.back() <= '4') return numbers(static_cast<std::size_t>(type.back() - '0'), kind);
            return {};
        }

    auto Runtime::Impl::ReadBack(Built& out) -> void
    {
            const auto state = out.readback;
            if (!state) return;
            const std::uint64_t copies = state->copies.load();
            if (copies < ReadState::Ring) { out.outputs["ready"].value = false; return; }
            // The oldest of them: the one the next copy goes into.
            const auto bytes = (*state->owners[copies % ReadState::Ring])->Read<std::byte>();
            if (bytes.size() < state->bytes) return;
            Value::List numbers;
            if (state->texel) {
                const TexelLayout& texel = *state->texel;
                const std::size_t size = static_cast<std::size_t>(texel.bits / 8);
                for (int channel = 0; channel < texel.channels; ++channel) {
                    const std::byte* at = bytes.data() + static_cast<std::size_t>(channel) * size;
                    std::uint32_t raw = 0;
                    std::memcpy(&raw, at, size);
                    double n = 0;
                    if (texel.kind == 'f') {
                        if (size == 4) { float f; std::memcpy(&f, at, 4); n = f; }
                        else {
                            // A half: its sign, five bits of exponent and ten of fraction.
                            const std::uint32_t sign = raw >> 15, exponent = (raw >> 10) & 31, fraction = raw & 1023;
                            n = exponent == 0 ? std::ldexp(fraction, -24) : exponent == 31 ? (fraction ? std::nan("") : INFINITY) : std::ldexp(fraction + 1024, static_cast<int>(exponent) - 25);
                            if (sign) n = -n;
                        }
                    } else if (texel.kind == 'u' || texel.kind == 's') {
                        const double most = std::ldexp(1.0, texel.bits) - 1.0;
                        if (texel.kind == 'u') n = raw / most;
                        else {
                            const std::int64_t signedRaw = raw >= (1u << (texel.bits - 1)) ? static_cast<std::int64_t>(raw) - (std::int64_t { 1 } << texel.bits) : raw;
                            n = std::max(static_cast<double>(signedRaw) / (most / 2.0 - 0.5), -1.0);
                        }
                    } else if (texel.kind == 'I') {
                        n = raw >= (1u << (texel.bits - 1)) && texel.bits < 32 ? static_cast<double>(static_cast<std::int64_t>(raw) - (std::int64_t { 1 } << texel.bits))
                                                                                 : static_cast<double>(static_cast<std::int32_t>(raw));
                    } else {
                        n = raw;
                    }
                    numbers.emplace_back(n);
                }
            } else {
                for (std::size_t i = 0; i < state->count; ++i) {
                    const std::byte* at = bytes.data() + i * 4;
                    if (state->kind == 'f') { float f; std::memcpy(&f, at, 4); numbers.emplace_back(f); }
                    else if (state->kind == 'i') { std::int32_t v; std::memcpy(&v, at, 4); numbers.emplace_back(v); }
                    else { std::uint32_t v; std::memcpy(&v, at, 4); numbers.emplace_back(v); }
                }
            }
            out.outputs["out"].value = numbers.size() == 1 ? numbers.front() : Value(std::move(numbers));
            out.outputs["ready"].value = true;
        }

    auto Runtime::Impl::BuildBounds(const Node& node, Built& out) -> Expected<void>
    {
            const auto wired = node.inputs.find("entities");
            const Node* origin = wired != node.inputs.end() && wired->second.link ? Origin(wired->second.link->node) : nullptr;
            const auto source = origin ? built.find(origin->id) : built.end();
            if (source == built.end() || !source->second.listed) return Fail("'entities' is what it is the box round: wire an Entities node's, or a Filter's");
            const EntityList& list = *source->second.listed;
            kor::Vec3 least(std::numeric_limits<float>::max()), most(-std::numeric_limits<float>::max());
            const kecs::ComponentId meshes = world ? world->Find("Mesh") : kecs::NoComponent;
            const kecs::FieldDescription* field = meshes != kecs::NoComponent ? world->Describe(meshes).FindField("mesh") : nullptr;
            std::size_t count = 0;
            for (const kecs::Entity entity : list.entities) {
                if (!world) break;
                ++count;
                const kor::Mat4 placed = ToKor(world->WorldMatrix(entity));
                const Resource* mesh = nullptr;
                if (field && IsResourceField(*field))
                    if (const auto* bytes = static_cast<const std::byte*>(world->Get(entity, meshes))) {
                        const Held* held = reinterpret_cast<const Held*>(bytes + field->offset);
                        while (*held && (*held)->replacedBy) held = &(*held)->replacedBy;
                        mesh = held->get();
                    }
                if (!mesh || !mesh->bounded) {
                    const kor::Vec3 at(placed[3][0], placed[3][1], placed[3][2]);
                    least = kor::Min(least, at);
                    most = kor::Max(most, at);
                    continue;
                }
                for (int corner = 0; corner < 8; ++corner) {
                    const kor::Vec4 local((corner & 1) ? mesh->bounds[3] : mesh->bounds[0], (corner & 2) ? mesh->bounds[4] : mesh->bounds[1],
                                          (corner & 4) ? mesh->bounds[5] : mesh->bounds[2], 1.f);
                    const kor::Vec4 at = placed * local;
                    least = kor::Min(least, kor::Vec3(at.x, at.y, at.z));
                    most = kor::Max(most, kor::Vec3(at.x, at.y, at.z));
                }
            }
            if (count == 0) least = most = kor::Vec3(0.f);
            const kor::Vec3 middle = (least + most) * 0.5f;
            out.outputs["min"].value = Three(least);
            out.outputs["max"].value = Three(most);
            out.outputs["center"].value = Three(middle);
            out.outputs["radius"].value = static_cast<double>(kor::Length(most - least) * 0.5f);
            out.outputs["count"].value = static_cast<double>(count);
            return {};
        }

    auto Runtime::Impl::WriteComponent(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto text = [&](const char* name) -> const std::string* {
                const auto it = inputs.find(name);
                return it == inputs.end() ? nullptr : it->second.value.String();
            };
            const std::string* component = text("component");
            const std::string* name = text("field");
            if (!component || component->empty()) return Fail("'component' is the component written");
            if (!name || name->empty()) return Fail("'field' is the field of it written, by name");
            out.outputs["written"].value = false;
            if (!world) return {};
            const kecs::ComponentId id = world->Find(*component);
            if (id == kecs::NoComponent) return Fail(std::format("there is no component '{}'", *component));
            const kecs::FieldDescription* field = world->Describe(id).FindField(*name);
            if (!field) return Fail(std::format("'{}' has no field '{}'", *component, *name));
            if (IsResourceField(*field) || field->type == "string" || field->type == "entity")
                return Fail(std::format("'{}.{}' is a {}: a number, a vector, a matrix or true or false is written", *component, *name, field->type));
            const auto given = inputs.find("value");
            if (given == inputs.end() || given->second.value.IsNull()) return {};
            const kecs::Entity entity = EntityFor(inputs, id);
            const auto* bytes = entity.IsNull() ? nullptr : static_cast<const std::byte*>(world->Get(entity, id));
            if (!bytes) return {};
            out.outputs["written"].value = true;
            // What it would hold, compared with what it holds: written only when they differ, so nothing else is told of a change that is none.
            std::vector<double> numbers;
            if (const bool* flag = given->second.value.Bool()) numbers = { *flag ? 1.0 : 0.0 };
            else if (auto read = NumbersOf(given->second.value, "value")) numbers = std::move(*read);
            else return Fail(read.error());
            std::vector<std::byte> wanted(bytes + field->offset, bytes + field->offset + field->size);
            const std::string& type = field->type;
            const auto put = [&](const std::size_t i, const char kind) {
                if (i >= numbers.size() || (i + 1) * 4 > wanted.size()) return;
                if (kind == 'f') { const auto v = static_cast<float>(numbers[i]); std::memcpy(wanted.data() + i * 4, &v, 4); }
                else if (kind == 'i') { const auto v = static_cast<std::int32_t>(std::llround(numbers[i])); std::memcpy(wanted.data() + i * 4, &v, 4); }
                else { const auto v = static_cast<std::uint32_t>(std::llround(std::max(numbers[i], 0.0))); std::memcpy(wanted.data() + i * 4, &v, 4); }
            };
            if (type == "f64") { if (!numbers.empty()) std::memcpy(wanted.data(), &numbers[0], 8); }
            else if (type == "bool") { if (!numbers.empty()) wanted[0] = std::byte { numbers[0] != 0.0 }; }
            else {
                const char kind = type.starts_with("ivec") || type == "i32" ? 'i' : type.starts_with("uvec") || type == "u32" || type == "bool32" ? 'u' : 'f';
                for (std::size_t i = 0; i < field->size / 4; ++i) put(i, kind);
            }
            if (std::memcmp(wanted.data(), bytes + field->offset, wanted.size()) == 0) return {};
            if (auto* writable = static_cast<std::byte*>(world->Mutable(entity, id))) std::memcpy(writable + field->offset, wanted.data(), wanted.size());
            return {};
        }

    auto Runtime::Impl::DrawShapes(const Inputs& inputs) -> Expected<void>
    {
            const auto numbers = [&](const char* name) -> std::vector<double> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || it->second.value.IsNull()) return {};
                if (const double* n = it->second.value.Number()) return { *n };
                auto read = NumbersOf(it->second.value, name);
                return read ? *read : std::vector<double> {};
            };
            const auto kindOf = inputs.find("kind");
            const std::string kind = kindOf != inputs.end() && kindOf->second.value.String() ? *kindOf->second.value.String() : "eFrustum";
            kor::DebugStyle style;
            if (const auto color = numbers("color"); color.size() >= 3)
                style.color = kor::Vec4(static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]), color.size() > 3 ? static_cast<float>(color[3]) : 1.f);
            const auto three = [](const std::vector<double>& v, const std::size_t i) {
                return kor::Vec3(static_cast<float>(v[i * 3]), static_cast<float>(v[i * 3 + 1]), static_cast<float>(v[i * 3 + 2]));
            };
            const auto matrix = [](const std::vector<double>& v, const std::size_t i) {
                kor::Mat4 m;
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) m[column][row] = static_cast<float>(v[i * 16 + static_cast<std::size_t>(column * 4 + row)]);
                return m;
            };
            if (!debug) return {};
            if (kind == "eLine" || kind == "eArrow") {
                const auto from = numbers("from"), to = numbers("to");
                if (from.size() % 3 != 0 || to.size() != from.size()) return Fail("'from' and 'to' are three numbers for each line, as many of one as of the other");
                for (std::size_t i = 0; i < from.size() / 3; ++i)
                    if (kind == "eLine") debug->Line(three(from, i), three(to, i), style); else debug->Arrow(three(from, i), three(to, i), style);
            } else if (kind == "eBox") {
                const auto least = numbers("min"), most = numbers("max");
                if (least.size() % 3 != 0 || most.size() != least.size()) return Fail("'min' and 'max' are three numbers for each box");
                for (std::size_t i = 0; i < least.size() / 3; ++i) debug->Box(three(least, i), three(most, i), style);
            } else if (kind == "eSphere") {
                const auto centers = numbers("center"), radii = numbers("radius");
                if (centers.size() % 3 != 0 || radii.empty()) return Fail("'center' is three numbers for each sphere, and 'radius' one for all or one for each");
                for (std::size_t i = 0; i < centers.size() / 3; ++i) debug->Sphere(three(centers, i), static_cast<float>(radii[std::min(i, radii.size() - 1)]), style);
            } else if (kind == "eFrustum" || kind == "eAxes" || kind == "eTransformedBox") {
                const auto matrices = numbers("matrix");
                if (matrices.empty() || matrices.size() % 16 != 0) return Fail("'matrix' is sixteen numbers for each");
                const auto size = numbers("size");
                for (std::size_t i = 0; i < matrices.size() / 16; ++i) {
                    if (kind == "eFrustum") debug->Frustum(matrix(matrices, i), style);
                    else if (kind == "eAxes") debug->Axes(matrix(matrices, i), size.empty() ? 1.f : static_cast<float>(size[0]));
                    else debug->Box(matrix(matrices, i), style);
                }
            } else {
                return Fail(std::format("'kind' is eLine, eArrow, eBox, eSphere, eFrustum, eAxes or eTransformedBox, not '{}'", kind));
            }
            return {};
        }

    auto Runtime::Impl::BuildLive(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto set = [&](const char* name, Value value) { out.outputs[name].value = std::move(value); };
            const std::string& type = node.type;

            if (type == "Selected") {
                // Which entity is picked, from 1; 0 for none — as whoever runs the graph says (SetSelected).
                const double picked = static_cast<double>(selectedId->load());
                set("out", picked);
                set("entityId", picked);
                set("present", picked > 0.0 ? 1 : 0);
                // A pick asked of this frame: what draws the ids it is read from has to draw them now.
                set("picking", pick->asked ? 1 : 0);
            } else if (type == "Time") {
                set("seconds", timeSeconds);
                set("dt", timeDelta);
                set("frame", static_cast<double>(timeFrame));
            } else if (type == "FixedStep") {
                set("dt", fixedSeconds);
                set("steps", static_cast<double>(fixedSteps));
                set("count", static_cast<double>(fixedCount));
                set("lost", static_cast<double>(fixedLost));
                set("alpha", fixedFraction);
            } else if (type == "LastFrame") {
                const auto given = inputs.find("value");
                const Value now = given == inputs.end() ? Value {} : given->second.value;
                auto [at, fresh] = lastSeen.try_emplace(node.id);
                LastSeen& seen = at->second;
                if (fresh) seen = { timeFrame, now, now, false };
                else if (seen.frame != timeFrame) {
                    // Another frame: what it was given on the last is what it was before, if the last was the frame before.
                    seen.valid = seen.frame + 1 == timeFrame;
                    seen.before = seen.valid ? seen.now : now;
                    seen.frame = timeFrame;
                }
                seen.now = now;
                set("out", seen.before);
                set("valid", seen.valid);
            } else if (type == "SceneBounds") {
                return BuildBounds(node, out);
            } else if (type == "WriteComponent") {
                return WriteComponent(inputs, out);
            } else if (type == "DebugShapes") {
                return DrawShapes(inputs);
            } else if (type == "Readback") {
                ReadBack(out);
            } else if (type == "Keep") {
                set("valid", out.recorded && out.recorded->load() > 0);
            } else if (IsEach(type)) {
                // A ForEach is of a component; an Entities node of every entity; a Filter of those of the node it is wired.
                const bool all = type == "Entities", filter = type == "Filter";
                const auto component = inputs.find("component");
                const std::string* name = component == inputs.end() ? nullptr : component->second.value.String();
                static const std::string none;
                if (all || filter || (type == "Entity" && (!name || name->empty()))) name = &none;
                else if (!name || name->empty()) return Fail("'component' is the name of the component every one of them has");
                const EntityList* from = nullptr;
                if (filter) {
                    const auto wired = node.inputs.find("entities");
                    // Through the References between: what another pass made is brought in by one.
                    const Node* origin = wired != node.inputs.end() && wired->second.link ? Origin(wired->second.link->node) : nullptr;
                    const auto upstream = origin ? built.find(origin->id) : built.end();
                    if (upstream == built.end() || !upstream->second.listed) return Fail("'entities' is what it keeps some of: wire an Entities node's, or another Filter's");
                    from = upstream->second.listed.get();
                }
                std::size_t repeat = 1;
                if (const auto it = inputs.find("repeat"); it != inputs.end() && !it->second.value.IsNull()) {
                    const double* given = it->second.value.Number();
                    if (!given || *given < 1 || *given > 256 || *given != std::floor(*given)) return Fail("'repeat' is how many elements each entity is: a whole number from 1 to 256");
                    repeat = static_cast<std::size_t>(*given);
                }
                // A ForEach is all of them; an Entity one of them: the first, or the one named.
                for (const char* gone : { "select", "which" })
                    if (inputs.contains(gone))
                        return Fail(std::format("'{}' is no ForEach's now: a ForEach is every one of them, and an Entity node is one — the first, or the one it names", gone));
                const bool one = type == "Entity";
                std::string named;
                if (const auto it = inputs.find("entity"); one && it != inputs.end() && !it->second.value.IsNull()) {
                    if (!it->second.value.String()) return Fail("'entity' is the entity's name");
                    named = *it->second.value.String();
                }
                if (one && name->empty() && named.empty()) return Fail("'component' is the component it has, or 'entity' the entity's name: one of them, at least");
                const std::string select = !one ? "eAll" : named.empty() ? "eFirst" : "eNamed";
                const std::size_t which = 0;
                std::vector<kecs::ComponentId> with, without;
                bool nobody = false;
                for (const std::string_view which : { "with", "without" }) {
                    const auto given = inputs.find(std::string(which));
                    if (given == inputs.end() || given->second.value.IsNull()) continue;
                    const auto* names = given->second.value.Items();
                    if (!names || !std::ranges::all_of(*names, [](const Value& v) { return v.String() != nullptr; })) return Fail(std::format("'{}' is a list of component names", which));
                    for (const auto& each : *names) {
                        const kecs::ComponentId id = world ? world->Find(*each.String()) : kecs::NoComponent;
                        if (id != kecs::NoComponent) (which == "with" ? with : without).push_back(id);
                        else if (which == "with") nobody = true;    // nothing has a component there is none of
                    }
                }
                const kecs::ComponentId id = world && !name->empty() ? world->Find(*name) : kecs::NoComponent;
                // A Filter grouped: by one of `with`'s components, each entity next to those whose component is as its own.
                kecs::ComponentId groupBy = kecs::NoComponent;
                if (const auto it = inputs.find("groupBy"); filter && it != inputs.end() && !it->second.value.IsNull()) {
                    const std::string* by = it->second.value.String();
                    if (!by) return Fail("'groupBy' is one of `with`'s components, by name");
                    if (!by->empty()) {
                        groupBy = world ? world->Find(*by) : kecs::NoComponent;
                        if (!nobody && (groupBy == kecs::NoComponent || std::ranges::find(with, groupBy) == with.end()))
                            return Fail(std::format("'groupBy' is one of `with`'s components, and '{}' is not among them", *by));
                    }
                }
                // A Filter of one entity alone, by its id from 1: what an editor's outline draws, of what is picked.
                double only = -1;
                if (const auto it = inputs.find("entityId"); filter && it != inputs.end() && !it->second.value.IsNull()) {
                    const double* given = it->second.value.Number();
                    if (!given || *given < 0 || *given != std::floor(*given)) return Fail("'entityId' is an entity's id from 1 (0 for none)");
                    only = *given;
                }
                // `where`: fields an entity's must hold — its component's as `field.<name>`, another's as `<Component>.<name>`.
                struct Holds { kecs::ComponentId component; kecs::FieldDescription field; Value value; };
                std::vector<Holds> holds;
                std::string whereGiven;
                std::vector<std::uint64_t> whereChanges;
                if (const auto it = inputs.find("where"); it != inputs.end() && !it->second.value.IsNull()) {
                    const auto* members = it->second.value.Named();
                    if (!members) return Fail("'where' is { \"field.<name>\": what it holds }: the fields an entity's must hold");
                    whereGiven = it->second.value.ToJson();
                    for (const auto& [output, value] : *members) {
                        if (!value.Number() && !value.Bool()) return Fail(std::format("'where' has '{}': what it holds is a number, or true or false", output));
                        const bool own = output.starts_with("field.");
                        const auto dot = output.find('.');
                        if (dot == std::string::npos) return Fail(std::format("'where' has '{}': a field is `field.<name>`, or `<Component>.<name>`", output));
                        const kecs::ComponentId of = own ? (world && !name->empty() ? world->Find(*name) : kecs::NoComponent) : world ? world->Find(output.substr(0, dot)) : kecs::NoComponent;
                        if (of == kecs::NoComponent) { nobody = true; continue; }     // nothing has a field of a component there is none of
                        const kecs::FieldDescription* field = world->Describe(of).FindField(output.substr(dot + 1));
                        if (!field) return Fail(std::format("'where' has '{}': {} has no such field", output, world->Describe(of).name));
                        holds.push_back({ of, *field, value });
                        whereChanges.push_back(world->ChangesOf(of));
                    }
                }
                // What it gives: its component's fields, and those of each of `with`'s, as they are now.
                std::string shape;
                std::vector<kecs::ComponentId> shaped;
                if (id != kecs::NoComponent) shaped.push_back(id);
                if (id != kecs::NoComponent || filter) shaped.insert(shaped.end(), with.begin(), with.end());
                for (const kecs::ComponentId each : shaped) {
                    shape += world->Describe(each).name + "{";
                    for (const auto& field : world->Describe(each).fields) shape += std::format("{}:{}@{};", field.name, field.type, field.offset);
                    shape += "}";
                }
                if (!out.listed) {
                    // Built: a number, a vector or a resource each — of the component as `field.<name>`, of another as `<Component>.<name>`.
                    out.listed = std::make_shared<EntityList>();
                    out.listed->component = id;
                    out.listed->shape = shape;
                    const auto gives = [&](const kecs::ComponentId each, const bool own) {
                        const kecs::ComponentDescription& description = world->Describe(each);
                        // What one is with nobody there: a new one, as its type makes it — plain data, as a script's is — or nothing.
                        std::vector<std::byte>& none = out.listed->nones[each];
                        none.assign(description.size, std::byte { 0 });
                        if (description.construct && !description.destroy && !description.IsTag()) description.construct(none.data(), description);
                        for (const auto& field : description.fields) {
                            if (field.offset + field.size > description.size) continue;
                            const std::string output = own ? "field." + field.name : description.name + "." + field.name;
                            const std::string doc = std::format("Each one's {}.{}.", description.name, field.name);
                            if (IsResourceField(field)) {
                                out.gives.push_back({ output, PortKind::eResource, field.type == "resource" ? std::string("Resource") : field.type, false, doc, nullptr });
                                out.listed->fields.push_back({ output, each, field, true });
                                continue;
                            }
                            const Value value = FieldValue(field, none.data() + field.offset);
                            if (value.IsNull()) continue;   // a string: nothing a buffer holds
                            const auto* items = value.Items();
                            out.gives.push_back({ output, PortKind::eValue, items ? (items->size() == 16 ? "matrix" : "vector") : value.Bool() ? "bool" : "number", false, doc, nullptr });
                            out.listed->fields.push_back({ output, each, field, false });
                        }
                    };
                    if (id != kecs::NoComponent) gives(id, true);
                    if (id != kecs::NoComponent || filter)
                        for (const kecs::ComponentId each : with) if (each != id) gives(each, false);
                }
                EntityList& list = *out.listed;
                list.repeat = repeat;
                // Found again only when what they are found from has changed since: see EntityList::Made.
                // A list of what is drawn leaves out what the runtime hides (an editor's view).
                const kecs::ComponentId meshes = world ? world->Find("Mesh") : kecs::NoComponent;
                const bool drawn = meshes != kecs::NoComponent && (id == meshes || std::ranges::contains(with, meshes));
                const EntityList::Made made { world ? world->StructureVersion() : 0, from ? from->version : 0,
                                              world && groupBy != kecs::NoComponent ? world->ChangesOf(groupBy) : 0,
                                              with, without, id, groupBy, select, named, whereGiven, whereChanges, repeat, nobody, only,
                                              drawn ? hiddenVersion : 0 };
                const bool again = !list.made || *list.made != made;
                // The component was made since, or is made of other fields now: what this gives is no longer what it has.
                if (list.component != id || list.shape != shape) {
                    list.entities.clear();
                    list.made.reset();
                    out.signature.clear();
                    stale = true;
                } else if (!again) {
                    // As they were: the entities, their groups, and what was said of the groups.
                } else if (list.entities.clear(); world && (id != kecs::NoComponent || all || filter || select == "eNamed") && !nobody) {
                    const auto has = [&](const kecs::Entity entity, const kecs::ComponentId each) { return world->Has(entity, each) && world->IsEnabled(entity, each); };
                    for (const kecs::Entity entity : from ? from->entities : world->All())
                        if ((!drawn || !hidden.contains(entity)) && (id == kecs::NoComponent || has(entity, id)) && std::ranges::all_of(with, [&](const auto each) { return has(entity, each); })
                            && std::ranges::none_of(without, [&](const auto each) { return has(entity, each); })
                            && std::ranges::all_of(holds, [&](const Holds& hold) {
                                   const auto* bytes = static_cast<const std::byte*>(world->Get(entity, hold.component));
                                   if (!bytes) return false;
                                   const Value now = FieldValue(hold.field, bytes + hold.field.offset);
                                   if (hold.value.Bool()) return now.Bool() ? *now.Bool() == *hold.value.Bool() : now.Number() && (*now.Number() != 0.0) == *hold.value.Bool();
                                   return now.Number() ? std::abs(*now.Number() - *hold.value.Number()) < 1e-6 : now.Bool() && (*now.Bool() ? 1.0 : 0.0) == *hold.value.Number();
                               }))
                            list.entities.push_back(entity);
                    if (select == "eFirst" && list.entities.size() > 1) list.entities.resize(1);
                    if (select == "eIndex") list.entities = which < list.entities.size() ? std::vector { list.entities[which] } : std::vector<kecs::Entity> {};
                    if (select == "eNamed") std::erase_if(list.entities, [&](const kecs::Entity entity) { return world->NameOf(entity) != named; });
                    if (select == "eNamed" && list.entities.size() > 1) list.entities.resize(1);
                    if (only >= 0) std::erase_if(list.entities, [&](const kecs::Entity entity) { return entity.index + 1.0 != only; });
                }
                // Grouped: the entities of each group one after another, each group where its first one was. Two are of one
                // group when their component holds the same: a resource the same one (the same mesh), anything else the same bytes.
                const bool regroup = again && list.component == id && list.shape == shape;
                if (regroup) {
                    list.made = made;
                    ++list.version;
                }
                list.grouped = groupBy != kecs::NoComponent;
                if (regroup) list.groupOf.clear();
                Value::List groups;
                if (regroup && list.grouped && world) {
                    const kecs::ComponentDescription& description = world->Describe(groupBy);
                    std::map<std::string, std::size_t> index;
                    std::vector<std::vector<kecs::Entity>> buckets;
                    for (const kecs::Entity entity : list.entities) {
                        std::string key;
                        if (const auto* bytes = static_cast<const std::byte*>(world->Get(entity, groupBy)))
                            for (const auto& field : description.fields) {
                                if (field.offset + field.size > description.size) continue;
                                if (IsResourceField(field)) {
                                    const Resource* which = reinterpret_cast<const Held*>(bytes + field.offset)->get();
                                    while (which && which->replacedBy) which = which->replacedBy.get();
                                    key.append(reinterpret_cast<const char*>(&which), sizeof which);
                                } else key.append(reinterpret_cast<const char*>(bytes + field.offset), field.size);
                            }
                        const auto [at, fresh] = index.try_emplace(std::move(key), buckets.size());
                        if (fresh) buckets.emplace_back();
                        buckets[at->second].push_back(entity);
                    }
                    list.entities.clear();
                    for (std::size_t group = 0; group < buckets.size(); ++group) {
                        groups.emplace_back(Value::Members { { "first", static_cast<double>(list.entities.size() * repeat) },
                                                             { "count", static_cast<double>(buckets[group].size() * repeat) } });
                        for (const kecs::Entity entity : buckets[group]) {
                            list.entities.push_back(entity);
                            list.groupOf.push_back(static_cast<std::uint32_t>(group));
                        }
                    }
                }
                if (filter && (regroup || !out.outputs.contains("groups"))) {
                    out.outputs["groupCount"].value = static_cast<double>(groups.size());
                    out.outputs["groups"].value = Value(std::move(groups));
                    if (!list.grouped) out.outputs["group"].value = 0;     // not grouped: every element is of the one
                }
                out.outputs["out"].value = static_cast<double>(list.Rows());     // them, for a Filter to keep some of
                SetRow(out, 0);
            }
            return {};
        }

    auto Runtime::Impl::SetRow(Built& out, const std::size_t index, const std::set<std::string, std::less<>>* only) const -> void
    {
            const EntityList& list = *out.listed;
            // Its element, a struct of all of it, asks for all of it.
            const bool whole = !only || only->contains("element");
            const auto wanted = [&](const std::string_view name) { return whole || only->contains(name); };
            const auto set = [&](const std::string_view name, Value value) {
                if (!wanted(name)) return;
                const auto it = out.outputs.find(name);
                if (it != out.outputs.end()) it->second.value = std::move(value);
                else out.outputs[std::string(name)].value = std::move(value);
            };
            const std::size_t repeat = std::max<std::size_t>(list.repeat, 1), which = index / repeat;
            const bool present = world && which < list.entities.size();
            const kecs::Entity entity = present ? list.entities[which] : kecs::NullEntity;
            for (const auto& read : list.fields) {
                if (!wanted(read.output)) continue;
                const auto* bytes = present ? static_cast<const std::byte*>(world->Get(entity, read.component)) : nullptr;
                if (!bytes) bytes = list.nones.at(read.component).data();
                if (!read.resource) { set(read.output, FieldValue(read.field, bytes + read.field.offset)); continue; }
                Held held = *reinterpret_cast<const Held*>(bytes + read.field.offset);
                while (held && held->replacedBy) held = held->replacedBy;
                out.outputs[read.output].resource = std::move(held);
            }
            set("count", static_cast<double>(list.Rows()));
            set("index", static_cast<double>(index));
            set("turn", static_cast<double>(index % repeat));
            set("entityIndex", static_cast<double>(which));
            set("entities", static_cast<double>(list.entities.size()));
            set("entityId", present ? static_cast<double>(entity.index) + 1.0 : 0.0);
            set("present", present ? 1.0 : 0.0);
            if (list.grouped) set("group", which < list.groupOf.size() ? static_cast<double>(list.groupOf[which]) : 0.0);
            if (only && std::ranges::none_of(std::array<std::string_view, 11> { "matrix", "normalMatrix", "previousMatrix", "worldPosition", "forward", "up", "right", "position", "rotation", "orientation", "scale" }, wanted)) return;
            kecs::Mat4 matrix;
            kecs::Vec3 position, rotation, scale { 1.f, 1.f, 1.f };
            if (present) {
                matrix = world->WorldMatrix(entity);
                position = world->PositionOf(entity);
                rotation = world->RotationOf(entity);
                scale = world->ScaleOf(entity);
            }
            const kor::Mat4 placed = ToKor(matrix);
            set("matrix", Matrix(placed));
            if (wanted("normalMatrix")) set("normalMatrix", Matrix(kor::Transpose(kor::Inverse(placed))));
            if (wanted("previousMatrix")) set("previousMatrix", Matrix(present ? MatrixBefore(entity, placed) : placed));
            set("worldPosition", Three({ placed[3][0], placed[3][1], placed[3][2] }));
            set("forward", Three(kor::Normalize(kor::Vec3(-placed[2][0], -placed[2][1], -placed[2][2]))));
            set("up", Three(kor::Normalize(kor::Vec3(placed[1][0], placed[1][1], placed[1][2]))));
            set("right", Three(kor::Normalize(kor::Vec3(placed[0][0], placed[0][1], placed[0][2]))));
            set("position", Value(Value::List { position.x, position.y, position.z }));
            set("rotation", Value(Value::List { rotation.x, rotation.y, rotation.z }));
            const kecs::Quat turned = present ? world->OrientationOf(entity) : kecs::Quat {};
            set("orientation", Value(Value::List { turned.x, turned.y, turned.z, turned.w }));
            set("scale", Value(Value::List { scale.x, scale.y, scale.z }));
            // The element as one value, a struct: where it is, then its fields by name — its own component's before any
            // other's of the same name — as Expand's element has them (a resource is wiring alone).
            if (!whole) return;
            Value::Members element;
            const auto value = [&](const std::string& output) -> const Value* {
                const auto it = out.outputs.find(output);
                return it == out.outputs.end() || it->second.resource ? nullptr : &it->second.value;
            };
            for (const char* place : { "entityId", "matrix", "normalMatrix", "previousMatrix", "worldPosition", "forward", "up", "right", "position", "rotation", "orientation", "scale" })
                if (const Value* got = value(place)) element.emplace_back(place, *got);
            for (const auto& read : list.fields) {
                if (read.resource || read.field.type == "string") continue;
                const Value* got = value(read.output);
                if (!got) continue;
                const bool own = read.output.starts_with("field.");
                const std::string name = read.field.name;
                if (const auto had = std::ranges::find(element, name, &Value::Members::value_type::first); had != element.end()) {
                    if (own) had->second = *got;
                } else {
                    element.emplace_back(name, *got);
                }
            }
            set("element", Value(std::move(element)));
        }

    auto Runtime::Impl::MatrixBefore(const kecs::Entity entity, const kor::Mat4& now) const -> kor::Mat4
    {
            auto [at, fresh] = moved.try_emplace(entity.index);
            Moved& seen = at->second;
            if (fresh || seen.entity != entity) seen = { entity, timeFrame, now, now };
            else if (seen.frame != timeFrame) {
                seen.before = seen.frame + 1 == timeFrame ? seen.now : now;
                seen.frame = timeFrame;
            }
            seen.now = now;
            return seen.before;
        }

    auto Runtime::Impl::WorksOut(const Node& node) -> bool
    {
            const NodeType* type = FindType(node.type);
            return type && !type->builder && (node.type == "Reference" || (type->category != "Passes" && type->category != "Resources" && type->category != "Scene"));
        }

    auto Runtime::Impl::Follows(const std::string& id, std::set<std::string>& sources, std::vector<std::string>& chain, std::map<std::string, bool>& seen) const -> bool
    {
            if (const auto known = seen.find(id); known != seen.end()) return known->second;
            seen[id] = false;
            const Node* node = current ? CurrentNode(id) : nullptr;
            if (!node) return false;
            if (IsEach(node->type)) {
                // One that chooses one of them is no loop: what is wired from it is that one's, like any value.
                if (node->type == "Entity") return false;
                sources.insert(id);
                return seen[id] = true;
            }
            if (!WorksOut(*node) || node->type == "Collect") return false;     // a Collect is of every element at once
            bool follows = false;
            for (const auto& input : node->inputs | std::views::values)
                if (input.link && Follows(input.link->node, sources, chain, seen)) follows = true;
            if (node->type == "Reference")
                if (const auto target = node->inputs.find("node"); target != node->inputs.end() && target->second.value.String())
                    if (Follows(*target->second.value.String(), sources, chain, seen)) follows = true;
            if (follows) chain.push_back(id);
            return seen[id] = follows;
        }

    auto Runtime::Impl::CollectAll(const Node& node, Built& out) -> void
    {
            const auto wired = node.inputs.find("value");
            Value::List all;
            std::size_t count = 0;
            if (wired != node.inputs.end() && wired->second.link) {
                std::set<std::string> sources;
                std::vector<std::string> chain;
                std::map<std::string, bool> seen;
                Follows(wired->second.link->node, sources, chain, seen);
                const auto from = built.find(wired->second.link->node);
                Built* source = nullptr;
                if (sources.size() == 1)
                    if (const auto found = built.find(*sources.begin()); found != built.end() && found->second.status.ok && found->second.listed) source = &found->second;
                const auto take = [&] {
                    if (from == built.end() || !from->second.status.ok) return;
                    const auto output = from->second.outputs.find(wired->second.link->output);
                    if (output == from->second.outputs.end()) return;
                    const Value& value = output->second.value;
                    if (const auto* items = value.Items()) all.insert(all.end(), items->begin(), items->end());
                    else if (const bool* flag = value.Bool()) all.emplace_back(*flag ? 1.0 : 0.0);
                    else if (!value.IsNull() && !value.Named()) all.push_back(value);
                    ++count;
                };
                if (!source) {
                    take();
                } else {
                    std::set<std::string, std::less<>> asked;
                    for (const std::string& id : chain)
                        if (const Node* between = current ? CurrentNode(id) : nullptr)
                            for (const auto& input : between->inputs | std::views::values)
                                if (input.link && input.link->node == *sources.begin()) asked.insert(input.link->output);
                    if (wired->second.link->node == *sources.begin()) asked.insert(wired->second.link->output);
                    for (std::size_t row = 0; row < source->listed->Rows(); ++row) {
                        SetRow(*source, row, &asked);
                        for (const auto& id : chain) Refresh(id);
                        take();
                    }
                    // Anything else wired to those elements has the first of them, as it does without a Collect.
                    SetRow(*source, 0);
                    for (const auto& id : chain) Refresh(id);
                }
            }
            out.outputs["out"].value = Value(std::move(all));
            out.outputs["count"].value = static_cast<double>(count);
        }

    auto Runtime::Impl::Put(const BlockField& field, const Value& value, std::byte* element) -> void
    {
            std::vector<double> numbers;
            if (const bool* flag = value.Bool()) numbers = { *flag ? 1.0 : 0.0 };
            else if (auto read = NumbersOf(value, "field")) numbers = std::move(*read);
            else return;
            std::size_t next = 0;
            for (std::uint32_t item = 0; item < field.count; ++item)
                for (std::uint32_t line = 0; line < field.lines; ++line)
                    for (std::uint32_t component = 0; component < field.components; ++component, ++next) {
                        const double number = next < numbers.size() ? numbers[next] : 0.0;
                        std::byte* at = element + field.offset + item * field.arrayStride + line * field.lineStride + component * field.bytes;
                        const auto put = [&](const auto v) { std::memcpy(at, &v, sizeof(v)); };
                        if (field.scalar == 'b') put(std::uint32_t { number != 0.0 ? 1u : 0u });
                        else if (field.scalar == 'f') {
                            if (field.bytes == 2) put(HalfOf(static_cast<float>(number)));
                            else if (field.bytes == 8) put(number);
                            else put(static_cast<float>(number));
                        } else if (field.scalar == 'i') {
                            const long long whole = std::llround(number);
                            if (field.bytes == 2) put(static_cast<std::int16_t>(std::clamp<long long>(whole, INT16_MIN, INT16_MAX)));
                            else if (field.bytes == 8) put(static_cast<std::int64_t>(whole));
                            else put(static_cast<std::int32_t>(whole));
                        } else {
                            const long long whole = std::llround(std::max(number, 0.0));
                            if (field.bytes == 2) put(static_cast<std::uint16_t>(std::min<long long>(whole, UINT16_MAX)));
                            else if (field.bytes == 8) put(static_cast<std::uint64_t>(whole));
                            else put(static_cast<std::uint32_t>(whole));
                        }
                    }
        }

    auto Runtime::Impl::RowKeySize(const Node& node, const Mapped& mapped, const Built& source, const std::set<std::string, std::less<>>& asked, std::vector<Value>& context) const -> std::size_t
    {
            const EntityList& list = *source.listed;
            if (asked.contains("previousMatrix") || asked.contains("element")) return 0;     // the element has the matrix of the frame before
            std::size_t size = sizeof(kecs::Entity);
            for (const auto& read : list.fields) {
                if (!asked.contains(read.output)) continue;
                if (read.resource) { size += sizeof(void*); continue; }
                if (read.field.assign || read.field.type == "string") return 0;
                size += read.field.size;
            }
            if (std::ranges::any_of(PlacedOutputs, [&](const std::string_view name) { return asked.contains(name); })) size += sizeof(kecs::Mat4);
            if (list.grouped) size += sizeof(std::uint32_t);

            context = { Value(static_cast<double>(list.Rows())), Value(static_cast<double>(list.entities.size())) };
            std::set<std::string_view> between(mapped.chain.begin(), mapped.chain.end());
            const auto read = [&](const Node& of) {
                for (const auto& [name, input] : of.inputs) {
                    if (!input.link) { context.push_back(input.value); continue; }
                    if (input.link->node == mapped.source || between.contains(input.link->node)) continue;
                    const auto from = built.find(input.link->node);
                    const auto output = from == built.end() ? decltype(from->second.outputs.end()) {} : from->second.outputs.find(input.link->output);
                    if (from == built.end() || output == from->second.outputs.end()) { context.emplace_back(); continue; }
                    context.push_back(output->second.value);
                    context.emplace_back(static_cast<double>(reinterpret_cast<std::uintptr_t>(output->second.resource.get())));
                }
            };
            read(node);
            for (const auto& id : mapped.chain) {
                const Node* each = current ? CurrentNode(id) : nullptr;
                if (!each || ReadsTheWorld(each->type)) return 0;
                read(*each);
            }
            return size;
        }

    auto Runtime::Impl::RowKey(const Built& source, const std::size_t index, const std::set<std::string, std::less<>>& asked, std::byte* key) const -> void
    {
            const EntityList& list = *source.listed;
            const std::size_t repeat = std::max<std::size_t>(list.repeat, 1), which = index / repeat;
            const bool present = world && which < list.entities.size();
            const kecs::Entity entity = present ? list.entities[which] : kecs::NullEntity;
            std::memcpy(key, &entity, sizeof(entity));
            key += sizeof(entity);
            for (const auto& read : list.fields) {
                if (!asked.contains(read.output)) continue;
                const auto* bytes = present ? static_cast<const std::byte*>(world->Get(entity, read.component)) : nullptr;
                if (!bytes) bytes = list.nones.at(read.component).data();
                if (read.resource) {
                    const Resource* held = reinterpret_cast<const Held*>(bytes + read.field.offset)->get();
                    while (held && held->replacedBy) held = held->replacedBy.get();
                    std::memcpy(key, &held, sizeof(held));
                    key += sizeof(held);
                    continue;
                }
                std::memcpy(key, bytes + read.field.offset, read.field.size);
                key += read.field.size;
            }
            if (std::ranges::any_of(PlacedOutputs, [&](const std::string_view name) { return asked.contains(name); })) {
                const kecs::Mat4 matrix = present ? world->WorldMatrix(entity) : kecs::Mat4 {};
                std::memcpy(key, &matrix, sizeof(matrix));
                key += sizeof(matrix);
            }
            if (list.grouped) {
                const std::uint32_t group = which < list.groupOf.size() ? list.groupOf[which] : 0;
                std::memcpy(key, &group, sizeof(group));
            }
        }

    auto Runtime::Impl::Fill(const Node& node, Built& out) -> void
    {
            Mapped& mapped = *out.mapped;
            const BlockLayout& layout = mapped.layout;
            const Mapped* first = nullptr;
            if (!mapped.after.empty()) {
                const auto found = built.find(mapped.after);
                if (found == built.end() || !found->second.status.ok || !found->second.mapped) return;
                first = found->second.mapped.get();
            }
            Built* source = nullptr;
            if (!mapped.source.empty()) {
                const auto found = built.find(mapped.source);
                if (found == built.end() || !found->second.status.ok || !found->second.listed) return;
                source = &found->second;
            }
            const std::size_t before = first ? first->count : 0;
            // A count given is how many elements there are: of a ForEach's, the first that many; without one, each the same.
            std::size_t rows = source ? source->listed->Rows() : mapped.most > 0 ? mapped.most : 1;
            if (mapped.most > 0) rows = std::min(rows, mapped.most);
            // More than there is room for is another buffer: it is built again, with whatever is made of it.
            if (before + rows > mapped.capacity) { out.signature.clear(); stale = true; return; }

            std::vector<std::byte> bytes(mapped.bytes.size());
            if (first) std::memcpy(bytes.data() + layout.first, first->bytes.data() + layout.first, before * layout.stride);
            // Where each field's value is: on the node, or in what another node gives — which changes where it is, a
            // row after another. And which of the entities' values anything asks for: only those are worked out.
            std::vector<std::pair<const BlockField*, const Value*>> fields, headers;
            // The whole element, when `fields` is a struct only as the graph runs: what it was wired from.
            const Value* whole = nullptr;
            if (const auto struct_ = node.inputs.find("fields"); struct_ != node.inputs.end() && struct_->second.link) {
                const auto from = built.find(struct_->second.link->node);
                if (from == built.end() || !from->second.status.ok) return;
                if (const auto output = from->second.outputs.find(struct_->second.link->output); output != from->second.outputs.end()) whole = &output->second.value;
            }
            std::set<std::string, std::less<>> asked;
            const auto asks = [&](const Node& of) {
                for (const auto& input : of.inputs | std::views::values)
                    if (input.link && input.link->node == mapped.source) asked.insert(input.link->output);
            };
            asks(node);
            for (const auto& id : mapped.chain)
                if (const Node* between = current ? CurrentNode(id) : nullptr) asks(*between);
            for (const auto& [name, input] : node.inputs) {
                if (!name.starts_with("field.")) continue;
                const BlockField* field = layout.Find(std::string_view(name).substr(6));
                // One of what is before the array: once for the buffer.
                auto* into = &fields;
                if (!field && (field = layout.FindHeader(std::string_view(name).substr(6)))) into = &headers;
                if (!field) continue;
                if (!input.link) { if (!input.value.IsNull()) into->emplace_back(field, &input.value); continue; }
                const auto from = built.find(input.link->node);
                if (from == built.end() || !from->second.status.ok) return;
                const auto output = from->second.outputs.find(input.link->output);
                if (output == from->second.outputs.end()) return;
                into->emplace_back(field, &output->second.value);
            }
            // Rows as they were are not worked out again: each is keyed by what it is worked out of, where that can be said.
            std::vector<Value> context;
            const std::size_t keySize = source ? RowKeySize(node, mapped, *source, asked, context) : 0;
            std::vector<std::byte> keys(keySize * rows);
            const bool reuse = keySize > 0 && mapped.keySize == keySize && mapped.keyedBefore == before && context == mapped.context
                               && mapped.bytes.size() == bytes.size();
            bool skipped = false;
            for (std::size_t row = 0; row < rows; ++row) {
                std::byte* element = bytes.data() + layout.first + (before + row) * layout.stride;
                if (keySize > 0) {
                    std::byte* key = keys.data() + row * keySize;
                    RowKey(*source, row, asked, key);
                    if (reuse && row < mapped.keyedRows && std::memcmp(key, mapped.rowKeys.data() + row * keySize, keySize) == 0) {
                        std::memcpy(element, mapped.bytes.data() + layout.first + (before + row) * layout.stride, layout.stride);
                        skipped = true;
                        continue;
                    }
                }
                if (source) {
                    SetRow(*source, row, &asked);
                    for (const auto& id : mapped.chain) Refresh(id);
                }
                for (const auto& [field, value] : fields)
                    if (!value->IsNull()) Put(*field, *value, element);
                // A struct as the graph runs (a Switch of two Bundles): each member where the field of its name is, unless given by itself.
                if (whole && whole->Named())
                    for (const auto& [name, member] : *whole->Named())
                        if (const BlockField* field = layout.Find(name); field && !node.inputs.contains("field." + name) && !member.IsNull()) Put(*field, member, element);
            }
            // Anything else wired to those entities has the first of them, as it does without a buffer.
            if (source && (rows > 1 || skipped)) {
                SetRow(*source, 0);
                for (const auto& id : mapped.chain) Refresh(id);
            }
            // What is before the array, once, where it is from the buffer's start.
            for (const auto& [field, value] : headers)
                if (!value->IsNull()) Put(*field, *value, bytes.data());
            mapped.rowKeys = std::move(keys);
            mapped.keySize = keySize;
            mapped.keyedRows = keySize > 0 ? rows : 0;
            mapped.keyedBefore = before;
            mapped.context = std::move(context);
            mapped.count = before + rows;
            out.outputs["count"].value = static_cast<double>(mapped.count);
            if (bytes == mapped.bytes) return;
            mapped.buffer->Write(bytes);
            mapped.bytes = std::move(bytes);
        }

    auto Runtime::Impl::FieldKind(const BlockField& field) -> std::string
    {
            static const std::map<std::pair<char, std::uint32_t>, const char*> scalars {
                { { 'f', 4 }, "float" }, { { 'i', 4 }, "int" }, { { 'u', 4 }, "uint" }, { { 'b', 4 }, "bool" },
                { { 'f', 2 }, "half" }, { { 'i', 2 }, "int16_t" }, { { 'u', 2 }, "uint16_t" },
                { { 'f', 8 }, "double" }, { { 'i', 8 }, "int64_t" }, { { 'u', 8 }, "uint64_t" } };
            const auto named = scalars.find({ field.scalar, field.bytes });
            std::string kind = named != scalars.end() ? named->second : "?";
            if (field.lines > 1) kind += std::format("{}x{}", field.lines, field.components);
            else if (field.components > 1) kind += std::to_string(field.components);
            if (field.count > 1) kind += std::format("[{}]", field.count);
            return kind;
        }

    auto Runtime::Impl::FieldPorts(const BlockLayout& layout, Built& out) -> std::string
    {
            std::string has;
            for (const auto& field : layout.fields) {
                const bool matrix = field.lines > 1 && field.count == 1;
                Port port { "field." + field.name, PortKind::eValue, field.Numbers() == 1 ? (field.scalar == 'b' ? "bool" : "number") : matrix ? "matrix" : "vector", false,
                            std::format("{}.{}: a {}, at byte {} of an element.", layout.type, field.name, FieldKind(field), field.offset), nullptr };
                if (field.scalar == 'f' && field.components >= 3 && field.lines == 1 && field.count == 1 && NamesColor(field.name)) port.editor = "color";
                out.ports.push_back(std::move(port));
                has += std::format("{}{}", has.empty() ? "" : ", ", field.name);
            }
            // What is before the array, once for the buffer: a field of its own, unless an element's has its name.
            for (const auto& field : layout.header) {
                if (layout.Find(field.name)) continue;
                const bool matrix = field.lines > 1 && field.count == 1;
                out.ports.push_back({ "field." + field.name, PortKind::eValue, field.Numbers() == 1 ? (field.scalar == 'b' ? "bool" : "number") : matrix ? "matrix" : "vector", false,
                                      std::format("{}: a {}, at byte {} of the buffer, before its elements: once for it, not for each.", field.name, FieldKind(field), field.offset), nullptr });
                has += std::format("{}{}", has.empty() ? "" : ", ", field.name);
            }
            return has.empty() ? std::string("none") : has;
        }

    auto Runtime::Impl::LayOut(const std::vector<std::tuple<std::string, std::string, std::uint32_t>>& fields, const bool storage) -> Expected<BlockLayout>
    {
            BlockLayout layout;
            layout.storage = storage;
            std::uint32_t at = 0, aligned = 4;
            std::string text;
            const auto up = [](const std::uint32_t n, const std::uint32_t to) { return (n + to - 1) / to * to; };
            for (const auto& [name, type, count] : fields) {
                BlockField field;
                field.name = name;
                field.count = std::max(count, 1u);
                static const std::map<std::string_view, std::tuple<char, std::uint32_t, std::uint32_t>> types {   // scalar, components, columns
                    { "eFloat", { 'f', 1, 1 } }, { "eInt", { 'i', 1, 1 } }, { "eUInt", { 'u', 1, 1 } }, { "eBool", { 'b', 1, 1 } },
                    { "eVec2", { 'f', 2, 1 } }, { "eVec3", { 'f', 3, 1 } }, { "eVec4", { 'f', 4, 1 } },
                    { "eIVec2", { 'i', 2, 1 } }, { "eIVec3", { 'i', 3, 1 } }, { "eIVec4", { 'i', 4, 1 } },
                    { "eUVec2", { 'u', 2, 1 } }, { "eUVec3", { 'u', 3, 1 } }, { "eUVec4", { 'u', 4, 1 } },
                    { "eMat3", { 'f', 3, 3 } }, { "eMat4", { 'f', 4, 4 } } };
                const auto known = types.find(type);
                if (known == types.end())
                    return Fail(std::format("'{}' is a {}, which is none of eFloat, eInt, eUInt, eBool, eVec2, eVec3, eVec4, eIVec2 to eIVec4, eUVec2 to eUVec4, eMat3, eMat4", name, type));
                if (name.empty()) return Fail("a field of the layout has no name");
                if (layout.Find(name)) return Fail(std::format("the layout has two fields called '{}'", name));
                const auto [scalar, components, columns] = known->second;
                field.scalar = scalar;
                field.components = components;
                field.lines = columns;
                // One of it: how big, and on what.
                std::uint32_t size = components == 3 ? 12 : components * 4, alignment = components == 3 ? 16 : components * 4;
                if (columns > 1) { field.lineStride = 16; size = 16 * columns; alignment = 16; }
                if (field.count > 1) {
                    if (!storage) alignment = up(alignment, 16);
                    field.arrayStride = up(size, alignment);
                    size = field.arrayStride * field.count;
                }
                at = up(at, alignment);
                field.offset = at;
                at += size;
                aligned = std::max(aligned, alignment);
                text += std::format("{}{} {}", text.empty() ? "" : " ", FieldKind(field), name) + ";";
                layout.fields.push_back(std::move(field));
            }
            if (!storage) aligned = up(aligned, 16);
            layout.stride = std::max(up(at, aligned), aligned);
            layout.type = "{ " + text + " }";
            layout.name = layout.type;
            return layout;
        }

    auto Runtime::Impl::BuildBufferBuilder(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto find = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            bool storage = true;
            if (const Value* usage = find("usage")) {
                const std::string* chosen = usage->String();
                if (!chosen || (*chosen != "eStorage" && *chosen != "eUniform")) return Fail("'usage' is eStorage (a StructuredBuffer, std430) or eUniform (a ConstantBuffer, std140)");
                storage = *chosen == "eStorage";
            }
            std::size_t most = 0;
            if (const Value* count = find("count")) {
                const double* given = count->Number();
                if (!given || *given < 1 || *given > 1 << 20 || *given != std::floor(*given)) return Fail("'count' is how many elements: a whole number, 1 or more");
                most = static_cast<std::size_t>(*given);
            }
            std::vector<std::tuple<std::string, std::string, std::uint32_t>> fields;
            if (const Value* layout = find("layout")) {
                const auto* items = layout->Items();
                if (!items) return Fail("'layout' is a list of fields: { \"name\": \"position\", \"type\": \"eVec3\" }");
                for (const Value& item : *items) {
                    const Value* name = item.Find("name");
                    const Value* type = item.Find("type");
                    const Value* count = item.Find("count");
                    if (!name || !name->String()) return Fail("each field of 'layout' has a \"name\"");
                    if (count && (!count->Number() || *count->Number() < 1)) return Fail(std::format("the \"count\" of '{}' is how many of it there are: 1 or more", *name->String()));
                    fields.emplace_back(*name->String(), type && type->String() ? *type->String() : std::string("eFloat"), count ? static_cast<std::uint32_t>(*count->Number()) : 1u);
                }
            }
            if (fields.empty()) return Fail("it has no fields yet: give it a 'layout'");
            auto layout = LayOut(fields, storage);
            if (!layout) return Fail(layout.error());

            auto mapped = std::make_shared<Mapped>();
            mapped->layout = std::move(*layout);
            mapped->layout.array = storage || most > 1;
            mapped->most = most;
            const std::string has = FieldPorts(mapped->layout, out);
            return MakeMapped(node, inputs, out, std::move(mapped), has);
        }

    auto Runtime::Impl::BuildShaderBuffer(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto text = [&](const char* name) {
                const auto it = inputs.find(name);
                const std::string* given = it == inputs.end() ? nullptr : it->second.value.String();
                return given ? *given : std::string();
            };
            const std::string module = text("module"), entry = text("entry"), parameter = text("parameter");
            if (module.empty()) return Fail("'module' is the shader module that declares the buffer");
            if (entry.empty()) return Fail("'entry' is an entry point of the module that uses the buffer");
            const kb::Description* shaderType = kb::Find("Shader");
            const kb::Setting* entryPoint = shaderType ? shaderType->FindSetting("entryPoint") : nullptr;
            if (!entryPoint) return Fail("the framework describes no Shader builder");
            auto shader = BuildWith(*shaderType, node.id, {}, { { entryPoint, { kb::Value::Of(module), kb::Value::Of(entry) } } });
            if (!shader) {
                if (shader.error().find("cannot open file") != std::string::npos) return Fail(std::format("there is no {}.slang among the shaders", module));
                return Fail(shader.error());
            }
            const auto compiled = kor::capi::ShaderOf((*shader)->handle);
            Watch(out, compiled);

            const std::vector<BlockLayout> blocks = BlocksOf(compiled->Spirv());
            std::string declared;
            for (const auto& block : blocks) declared += std::format("{}{}", declared.empty() ? "" : ", ", block.name);
            if (declared.empty()) declared = "none";
            if (parameter.empty()) return Fail(std::format("'parameter' is what the shader calls the buffer. '{}' of {} uses: {}", entry, module, declared));
            auto block = std::ranges::find(blocks, parameter, &BlockLayout::name);
            if (block == blocks.end()) block = std::ranges::find(blocks, parameter, &BlockLayout::type);
            if (block == blocks.end()) return Fail(std::format("'{}' of {} uses no buffer '{}'. It uses: {}", entry, module, parameter, declared));

            auto mapped = std::make_shared<Mapped>();
            mapped->layout = *block;
            mapped->shader = std::move(*shader);
            const std::string has = FieldPorts(mapped->layout, out);

            return MakeMapped(node, inputs, out, std::move(mapped), has);
        }

    auto Runtime::Impl::MakeMapped(const Node& node, const Inputs& inputs, Built& out, std::shared_ptr<Mapped> mapped, const std::string& has) -> Expected<void>
    {
            const BlockLayout& layout = mapped->layout;
            // What its fields follow: an element each, when any is wired from a ForEach or from what is worked out of one.
            std::set<std::string> sources;
            std::map<std::string, bool> seen;
            const auto follows = [&](const std::string& name) {
                if (const auto wired = node.inputs.find(name); wired != node.inputs.end() && wired->second.link) Follows(wired->second.link->node, sources, mapped->chain, seen);
            };
            follows("fields");      // a struct as the graph runs, of each element: what it follows, its fields do
            if (const auto whole = inputs.find("fields"); whole != inputs.end() && (whole->second.resource || !whole->second.value.Named()))
                return Fail(std::format("'fields' takes a Bundle — a Bundle node's, a ForEach's element, or a struct as the graph runs — and is given {}",
                                        whole->second.resource ? std::string("a resource") : whole->second.value.ToJson()));
            for (const auto& [name, input] : inputs) {
                if (name == "instance") return Fail("'instance' is no more: wire each of its fields, `field.<name>`, from what holds it");
                if (!name.starts_with("field.")) continue;
                if (!layout.Find(std::string_view(name).substr(6)) && !layout.FindHeader(std::string_view(name).substr(6))) {
                    if (FromBundle(node.id, name)) continue;     // given by a Bundle, by name: let pass, and said on the node
                    return Fail(std::format("{} has no field '{}'. It has: {}", layout.type, name.substr(6), has));
                }
                if (input.resource) return Fail(std::format("'{}' takes a value, and was wired to a resource", name));
                if (!input.value.IsNull() && !input.value.Bool() && !NumbersOf(input.value, name.c_str())) return Fail(std::format("'{}' is a number, or a list of them", name));
                follows(name);
            }
            if (sources.size() > 1)
                return Fail(std::format("its fields follow more than one ForEach, '{}' and '{}': an element is one of a ForEach's", *sources.begin(), *std::next(sources.begin())));
            std::size_t count = mapped->most > 0 ? mapped->most : 1;
            if (layout.array && !sources.empty()) {
                mapped->source = *sources.begin();
                const auto source = built.find(mapped->source);
                count = source != built.end() && source->second.listed ? source->second.listed->Rows() : 0;
                if (mapped->most > 0) count = std::min(count, mapped->most);
            } else {
                mapped->chain.clear();      // a block of one is the first element's, which is what is there already
            }
            std::size_t earlier = 0;
            if (const auto after = inputs.find("after"); after != inputs.end() && after->second.resource) {
                if (!layout.array) return Fail(std::format("'after' is for a buffer of many elements, and '{}' is a block of one", layout.name));
                const auto first = built.find(after->second.resource->node);
                const Mapped* theirs = first == built.end() ? nullptr : first->second.mapped.get();
                if (!theirs || theirs->layout.type != layout.type || theirs->layout.stride != layout.stride || theirs->layout.first != layout.first)
                    return Fail(std::format("'after' takes another {} of {}", node.type, layout.type));
                mapped->after = after->second.resource->node;
                earlier = theirs->count;
                count += earlier;
            }

            // Room for the next power of two of them, or — given a count — for as many as it says.
            mapped->capacity = mapped->most > 0 ? earlier + mapped->most : std::bit_ceil(std::max<std::size_t>(count, 1));
            mapped->bytes.assign(layout.first + mapped->capacity * layout.stride, std::byte { 0 });
            auto buffer = kor::Buffer::RawBuilder{}.SetRawSize(static_cast<kor::i64>(mapped->bytes.size()))
                .SetUsage(kor::Flags<kor::Buffer::Usage>(layout.storage ? kor::Buffer::Usage::eStorage : kor::Buffer::Usage::eUniform) | kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eDynamic).SetIsPerFrame(true).Build();
            if (!buffer.Valid()) return Fail(buffer.Failure() ? buffer.Failure()->message : std::string("its buffer could not be made"));
            mapped->buffer = kor::ResourceRef<kor::Buffer>(buffer);
            mapped->held = MakeResource(std::move(buffer));
            mapped->held->node = node.id;
            mapped->buffer->Write(mapped->bytes);
            out.outputs["out"].resource = mapped->held;
            out.outputs["stride"].value = static_cast<double>(layout.stride);
            out.outputs["bytes"].value = static_cast<double>(mapped->bytes.size());
            out.outputs["count"].value = 0.0;
            out.mapped = std::move(mapped);
            Fill(node, out);
            return {};
        }

    auto Runtime::Impl::EachResource(const std::string& id, const std::string& output) const -> std::vector<Held>
    {
            std::vector<Held> all;
            const auto it = built.find(id);
            if (it == built.end()) return all;
            const Built& source = it->second;
            if (source.listed && world) {
                const EntityList& list = *source.listed;
                const auto read = std::ranges::find(list.fields, output, &EntityList::Read::output);
                if (read != list.fields.end() && read->resource) {
                    for (const kecs::Entity entity : list.entities) {
                        const auto* bytes = static_cast<const std::byte*>(world->Get(entity, read->component));
                        if (!bytes) continue;
                        Held held = *reinterpret_cast<const Held*>(bytes + read->field.offset);
                        while (held && held->replacedBy) held = held->replacedBy;
                        all.push_back(std::move(held));
                    }
                    return all;
                }
            }
            if (const auto out = source.outputs.find(output); out != source.outputs.end() && out->second.resource) all.push_back(out->second.resource);
            return all;
        }

    auto Runtime::Impl::PackedMeshes(const Node& node) const -> std::vector<Held>
    {
            std::vector<Held> meshes;
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return meshes;
            const Node* origin = Origin(wired->second.link->node);
            std::set<const Resource*> seen;
            for (Held& held : EachResource(origin ? origin->id : wired->second.link->node, wired->second.link->output))
                if (held && seen.insert(held.get()).second) meshes.push_back(std::move(held));
            return meshes;
        }

    auto Runtime::Impl::PackedNow(const Node& node) const -> std::vector<const Resource*>
    {
            std::vector<const Resource*> meshes;
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return meshes;
            const Node* origin = Origin(wired->second.link->node);
            const auto it = built.find(origin ? origin->id : wired->second.link->node);
            if (it == built.end()) return meshes;
            const auto add = [&](const Resource* mesh) {
                while (mesh && mesh->replacedBy) mesh = mesh->replacedBy.get();
                if (mesh && std::ranges::find(meshes, mesh) == meshes.end()) meshes.push_back(mesh);     // a few: one each
            };
            const Built& source = it->second;
            if (source.listed && world) {
                const auto read = std::ranges::find(source.listed->fields, wired->second.link->output, &EntityList::Read::output);
                if (read != source.listed->fields.end() && read->resource) {
                    for (const kecs::Entity entity : source.listed->entities)
                        if (const auto* bytes = static_cast<const std::byte*>(world->Get(entity, read->component)))
                            add(reinterpret_cast<const Held*>(bytes + read->field.offset)->get());
                    return meshes;
                }
            }
            if (const auto out = source.outputs.find(wired->second.link->output); out != source.outputs.end()) add(out->second.resource.get());
            return meshes;
        }

    auto Runtime::Impl::BuildMeshPack(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto asked = inputs.find("positions");
            const bool positionsOnly = asked != inputs.end() && asked->second.value.Bool() && *asked->second.value.Bool();
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return Fail("'meshes' is each element's mesh: wire a Filter's Mesh.mesh");
            const std::vector<Held> meshes = PackedMeshes(node);
            auto packing = std::make_shared<MeshPacking>();
            for (const Held& mesh : meshes) packing->of.push_back(mesh.get());
            // The wholes they are of — a model's meshes are one on the device — each copied once, in the order they come.
            std::vector<const Resource*> wholes;
            std::map<const Resource*, std::size_t> wholeAt;
            for (const Held& mesh : meshes) {
                if (mesh->kind != "Mesh") return Fail(std::format("'{}' is a {}, and only meshes are packed", mesh->node, mesh->kind));
                if (mesh->pending) continue;    // still being read: packed when it is
                const Resource* whole = mesh->part ? mesh->source.get() : mesh.get();
                if (whole && whole->handle && wholeAt.try_emplace(whole, wholes.size()).second) wholes.push_back(whole);
            }
            out.outputs["count"].value = static_cast<double>(meshes.size());
            out.packing = std::move(packing);
            // Nothing there yet to pack — no meshes, or only ones still being read: an empty mesh laid out as the default
            // meshes are, so that the pipeline and the Draw are built and draw nothing (the Draw still clears) until there
            // is. It is built again when one arrives.
            kor::VertexLayout layout = wholes.empty() ? DefaultLayout(DefaultTangents()) : kor::capi::MeshOf(wholes.front()->handle)->Layout();
            if (positionsOnly) {
                layout = {};
                layout.attributes.emplace_back("POSITION", "", 0u, 0u, kor::ChannelType::eFloat, 3u);
                layout.bindings.emplace_back(0u, 12u);
            }
            // Only these, of every mesh: a buffer each, as the first mesh that has it has it — zeros where a mesh has not —
            // whatever else each has and however it lays them out. Meshes of other files are then one.
            std::vector<std::string> only;
            if (const auto wanted = inputs.find("attributes"); wanted != inputs.end() && !wanted->second.value.IsNull()) {
                const auto* items = wanted->second.value.Items();
                if (!items || items->empty()) return Fail("'attributes' is the semantics to pack, each a buffer: [\"POSITION\", \"NORMAL\", \"UV\"]");
                for (const auto& item : *items) {
                    if (!item.String()) return Fail("'attributes' is the semantics to pack, each a buffer: [\"POSITION\", \"NORMAL\", \"UV\"]");
                    only.push_back(*item.String());
                }
            }
            if (!only.empty() && !positionsOnly) {
                const kor::VertexLayout fallback = DefaultLayout(DefaultTangents());
                kor::VertexLayout chosen;
                for (std::size_t i = 0; i < only.size(); ++i) {
                    std::optional<kor::VertexLayout::Attribute> found;
                    for (const Resource* whole : wholes) {
                        const auto& attributes = kor::capi::MeshOf(whole->handle)->Layout().attributes;
                        if (const auto it = std::ranges::find(attributes, std::string_view(only[i]), [](const auto& a) { return std::string_view(a.semantic); }); it != attributes.end()) { found = *it; break; }
                    }
                    if (!found)
                        if (const auto it = std::ranges::find(fallback.attributes, std::string_view(only[i]), [](const auto& a) { return std::string_view(a.semantic); }); it != fallback.attributes.end()) found = *it;
                    if (!found) return Fail(std::format("no mesh it packs has {}", only[i]));
                    if (found->channelType != kor::ChannelType::eFloat && found->channelType != kor::ChannelType::eUInt && found->channelType != kor::ChannelType::eInt)
                        return Fail(std::format("{} is not of 32-bit numbers: only such attributes are packed", only[i]));
                    chosen.attributes.emplace_back(only[i], "", static_cast<kor::u32>(i), 0u, found->channelType, found->channelCount);
                    chosen.bindings.emplace_back(static_cast<kor::u32>(i), found->channelCount * 4u);
                }
                layout = std::move(chosen);
            }
            const std::string shape = LayoutValueOf(layout).ToJson();
            std::vector<kor::u64> vertexBase, indexBase;
            kor::u64 vertices = 0, indices = 0;
            for (const Resource* whole : wholes) {
                const auto mesh = kor::capi::MeshOf(whole->handle);
                if (!positionsOnly && only.empty() && LayoutValueOf(mesh->Layout()).ToJson() != shape)
                    return Fail(std::format("its meshes' vertices are not laid out alike: '{}' and '{}' are not — a Filter of each kind, and a pack of each", wholes.front()->node, whole->node));
                if (!mesh->IndexBuffer() || !mesh->IndexCount() || mesh->IndexType() != kor::ChannelType::eUInt)
                    return Fail(std::format("'{}' is not drawn by indices of 32 bits, and only such meshes are packed", whole->node));
                vertexBase.push_back(vertices);
                indexBase.push_back(indices);
                vertices += mesh->VertexCount();
                indices += *mesh->IndexCount();
            }
            // One buffer for each of the layout's, and one of indices: each whole's after the last, copied on the device.
            // What rays can be traced through, where the device traces them: a RayScene's structures are made of these.
            const kor::Flags<kor::Buffer::Usage> traced = kor::Context::SupportsRayTracing() ? kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eAccelerationStructureInput)
                                                                                              : kor::Flags<kor::Buffer::Usage>();
            const auto made = [traced](const kor::u64 bytes, const kor::Flags<kor::Buffer::Usage> usage) {
                return kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<kor::u64>(bytes, 4)))
                    .SetUsage(usage | traced | kor::Buffer::Usage::eTransferDst | kor::Buffer::Usage::eTransferSrc | kor::Buffer::Usage::eStorage)
                    .SetType(kor::Buffer::Type::eDeviceLocal).Build();
            };
            std::vector<kor::Resource<kor::Buffer>> buffers;
            for (const auto& binding : layout.bindings) {
                buffers.push_back(made(vertices * binding.stride, kor::Buffer::Usage::eVertex));
                if (!buffers.back().Valid()) return Fail("the packed mesh's vertices could not be made a buffer");
            }
            kor::Resource<kor::Buffer> index = made(indices * 4, kor::Buffer::Usage::eIndex);
            if (!index.Valid()) return Fail("the packed mesh's indices could not be made a buffer");
            if (positionsOnly) {
                // Each whole's positions read back and laid one after another, three floats each, whatever it lays out
                // besides: written as one buffer.
                std::vector<float> positions;
                positions.reserve(vertices * 3);
                for (const Resource* whole : wholes) {
                    const auto mesh = kor::capi::MeshOf(whole->handle);
                    const auto& from = mesh->Layout();
                    const auto position = std::ranges::find(from.attributes, std::string_view("POSITION"), [](const auto& a) { return std::string_view(a.semantic); });
                    const auto buffers = mesh->VertexBuffers();
                    if (position == from.attributes.end() || position->binding >= buffers.size() || position->channelType != kor::ChannelType::eFloat || position->channelCount < 3)
                        return Fail(std::format("'{}' has no POSITION of three floats to pack", whole->node));
                    const auto binding = std::ranges::find(from.bindings, position->binding, &kor::VertexInputBindingDescription::binding);
                    const kor::u64 stride = binding == from.bindings.end() ? 12 : binding->stride;
                    const std::vector<std::byte> bytes = buffers[position->binding]->Read<std::byte>(mesh->VertexCount() * stride, 0);
                    if (bytes.size() < mesh->VertexCount() * stride) return Fail(std::format("'{}'s vertices could not be read back", whole->node));
                    for (kor::u64 v = 0; v < mesh->VertexCount(); ++v) {
                        float p[3];
                        std::memcpy(p, bytes.data() + v * stride + position->offset, sizeof p);
                        positions.insert(positions.end(), p, p + 3);
                    }
                }
                if (!positions.empty()) buffers[0]->Write(std::span<const float>(positions));
            }
            if (!only.empty() && !positionsOnly) {
                // Each attribute of each mesh read back, and laid one mesh after another in its own buffer.
                std::vector<std::vector<std::uint32_t>> packedWords(layout.attributes.size());
                for (std::size_t a = 0; a < layout.attributes.size(); ++a) packedWords[a].assign(vertices * layout.attributes[a].channelCount, 0u);
                for (std::size_t w = 0; w < wholes.size(); ++w) {
                    const auto mesh = kor::capi::MeshOf(wholes[w]->handle);
                    const auto& from = mesh->Layout();
                    const auto sources = mesh->VertexBuffers();
                    std::map<kor::u32, std::vector<std::byte>> read;     // each binding of it, read back once
                    for (std::size_t a = 0; a < layout.attributes.size(); ++a) {
                        const auto& to = layout.attributes[a];
                        const auto it = std::ranges::find(from.attributes, std::string_view(to.semantic), [](const auto& x) { return std::string_view(x.semantic); });
                        if (it == from.attributes.end() || it->binding >= sources.size()) continue;      // it has none: zeros
                        const auto binding = std::ranges::find(from.bindings, it->binding, &kor::VertexInputBindingDescription::binding);
                        const kor::u64 stride = binding == from.bindings.end() ? it->channelCount * 4u : binding->stride;
                        auto& bytes = read[it->binding];
                        if (bytes.empty()) bytes = sources[it->binding]->Read<std::byte>(mesh->VertexCount() * stride, 0);
                        if (bytes.size() < mesh->VertexCount() * stride) return Fail(std::format("'{}'s vertices could not be read back", wholes[w]->node));
                        const kor::u32 words = std::min(to.channelCount, it->channelCount);
                        for (kor::u64 v = 0; v < mesh->VertexCount(); ++v)
                            std::memcpy(packedWords[a].data() + (vertexBase[w] + v) * to.channelCount, bytes.data() + v * stride + it->offset, words * 4u);
                    }
                }
                for (std::size_t a = 0; a < layout.attributes.size(); ++a)
                    if (!packedWords[a].empty()) buffers[a]->Write(std::span<const std::uint32_t>(packedWords[a]));
            }
            if (!wholes.empty()) kor::CommandBuffer::SingleTimeCommand([&](kor::CommandBuffer& cb) {
                for (std::size_t w = 0; w < wholes.size(); ++w) {
                    const auto mesh = kor::capi::MeshOf(wholes[w]->handle);
                    const auto from = mesh->VertexBuffers();
                    for (std::size_t b = 0; b < layout.bindings.size() && !positionsOnly && only.empty(); ++b) {
                        const kor::u32 binding = layout.bindings[b].binding, stride = layout.bindings[b].stride;
                        if (binding >= from.size()) continue;
                        cb.CopyBuffer(from[binding], kor::ResourceRef<const kor::Buffer>(buffers[b]), mesh->VertexCount() * stride, 0, vertexBase[w] * stride);
                    }
                    cb.CopyBuffer(*mesh->IndexBuffer(), kor::ResourceRef<const kor::Buffer>(index), static_cast<kor::u64>(*mesh->IndexCount()) * 4, 0, indexBase[w] * 4);
                }
            }, kor::CommandBuffer::Usage::eTransfer).Wait();
            // Each buffer a resource of its own, the mesh referring to it: what a compute shader reads of it (a MeshBuffers).
            kor::Mesh::Builder builder;
            std::vector<Held> own;
            for (std::size_t b = 0; b < layout.bindings.size(); ++b) {
                builder.SetVertexBuffer(layout.bindings[b].binding, kor::ResourceRef<kor::Buffer>(buffers[b]));
                own.push_back(MakeResource(std::move(buffers[b])));
            }
            builder.SetIndexBuffer(kor::ResourceRef<kor::Buffer>(index), kor::ChannelType::eUInt).SetVertexLayout(layout);
            own.push_back(MakeResource(std::move(index)));
            auto mesh = builder.Build();
            if (!mesh.Valid()) return Fail(mesh.Failure() ? mesh.Failure()->message : std::string("the packed mesh could not be built"));
            Held packed = MakeResource(std::move(mesh));
            packed->buffers = std::move(own);
            packed->kind = "Mesh";
            packed->node = node.id;

            // Each mesh's part of it: what an indirect command draws, and a shader's copy of the same.
            Value::List ranges;
            std::vector<std::byte> bytes(meshes.size() * 48, std::byte { 0 });
            bool anyBounds = false;
            for (std::size_t i = 0; i < meshes.size(); ++i) {
                const Resource& m = *meshes[i];
                const Resource* whole = m.part ? m.source.get() : &m;
                const auto at = whole ? wholeAt.find(whole) : wholeAt.end();
                std::uint32_t firstIndex = 0, indexCount = 0;
                std::int32_t vertexOffset = 0;
                if (!m.pending && at != wholeAt.end()) {
                    const auto source = kor::capi::MeshOf(whole->handle);
                    firstIndex = static_cast<std::uint32_t>(indexBase[at->second]) + (m.part ? m.firstIndex : 0);
                    indexCount = m.part ? m.indexCount : *source->IndexCount();
                    vertexOffset = static_cast<std::int32_t>(vertexBase[at->second]) + (m.part ? m.vertexOffset : 0);
                }
                const float* box = m.bounded ? m.bounds : whole && whole->bounded ? whole->bounds : nullptr;
                const Value::List low { box ? box[0] : 0.0, box ? box[1] : 0.0, box ? box[2] : 0.0 }, high { box ? box[3] : 0.0, box ? box[4] : 0.0, box ? box[5] : 0.0 };
                ranges.emplace_back(Value::Members { { "firstIndex", static_cast<double>(firstIndex) }, { "indexCount", static_cast<double>(indexCount) },
                                                     { "vertexOffset", static_cast<double>(vertexOffset) }, { "boundsMin", low }, { "boundsMax", high } });
                std::byte* to = bytes.data() + i * 48;
                std::memcpy(to, &firstIndex, 4);
                std::memcpy(to + 4, &indexCount, 4);
                std::memcpy(to + 8, &vertexOffset, 4);
                if (box) {
                    for (int axis = 0; axis < 3; ++axis) {
                        std::memcpy(to + 16 + axis * 4, &box[axis], 4);
                        std::memcpy(to + 32 + axis * 4, &box[axis + 3], 4);
                    }
                    if (!anyBounds) { std::copy(box, box + 6, packed->bounds); anyBounds = true; }
                    for (int axis = 0; axis < 3; ++axis) {
                        packed->bounds[axis] = std::min(packed->bounds[axis], box[axis]);
                        packed->bounds[axis + 3] = std::max(packed->bounds[axis + 3], box[axis + 3]);
                    }
                }
            }
            packed->bounded = anyBounds;
            auto table = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 48)))
                .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eDynamic).Build();
            if (!table.Valid()) return Fail("its ranges could not be made a buffer");
            table->Write(std::span<const std::byte>(bytes));
            Held rangeBuffer = MakeResource(std::move(table));
            rangeBuffer->node = node.id;
            out.outputs["mesh"].resource = std::move(packed);
            out.outputs["layout"].value = LayoutValueOf(layout);
            out.outputs["ranges"].value = Value(std::move(ranges));
            out.outputs["rangeBuffer"].resource = std::move(rangeBuffer);
            return {};
        }

    auto Runtime::Impl::BuildMeshlets(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto given = inputs.find("mesh");
            if (given == inputs.end() || !given->second.resource) return Fail("'mesh' is the meshes as one: a MeshPack's mesh");
            Held held = given->second.resource;
            while (held && held->replacedBy) held = held->replacedBy;
            if (!held || held->kind != "Mesh" || !held->handle) return Fail("'mesh' is a mesh: a MeshPack's");
            const auto rangesGiven = inputs.find("ranges");
            const auto* ranges = rangesGiven == inputs.end() ? nullptr : rangesGiven->second.value.Items();
            if (!ranges) return Fail("'ranges' is each mesh's part of it: the MeshPack's ranges");
            const auto number = [&](const char* name, const double otherwise) {
                const auto it = inputs.find(name);
                return it != inputs.end() && it->second.value.Number() ? *it->second.value.Number() : otherwise;
            };
            const auto mostVertices = static_cast<std::uint32_t>(std::clamp(number("maxVertices", 64), 3.0, 256.0));
            const auto mostTriangles = static_cast<std::uint32_t>(std::clamp(number("maxTriangles", 124), 1.0, 256.0));

            // Where its vertices are, and its indices: read back once.
            const auto mesh = kor::capi::MeshOf(held->handle);
            const auto& layout = mesh->Layout();
            const auto position = std::ranges::find(layout.attributes, std::string_view("POSITION"), [](const auto& a) { return std::string_view(a.semantic); });
            const auto sources = mesh->VertexBuffers();
            if (position == layout.attributes.end() || position->binding >= sources.size() || position->channelType != kor::ChannelType::eFloat || position->channelCount < 3)
                return Fail("its mesh has no POSITION of three floats");
            if (!mesh->IndexBuffer() || mesh->IndexType() != kor::ChannelType::eUInt) return Fail("its mesh is not drawn by indices of 32 bits");
            const auto binding = std::ranges::find(layout.bindings, position->binding, &kor::VertexInputBindingDescription::binding);
            const kor::u64 stride = binding == layout.bindings.end() ? 12 : binding->stride;
            const kor::u64 vertexCount = mesh->VertexCount();
            const std::vector<std::byte> vertexBytes = vertexCount ? sources[position->binding]->Read<std::byte>(vertexCount * stride, 0) : std::vector<std::byte>();
            const kor::u64 indexCount = mesh->IndexCount().value_or(0);
            const std::vector<std::uint32_t> indices = indexCount ? (*mesh->IndexBuffer())->Read<std::uint32_t>(indexCount, 0) : std::vector<std::uint32_t>();
            if (vertexBytes.size() < vertexCount * stride || indices.size() < indexCount) return Fail("its mesh's vertices could not be read back");
            const auto at = [&](const std::uint32_t vertex) {
                float p[3] { 0.f, 0.f, 0.f };
                if (vertex < vertexCount) std::memcpy(p, vertexBytes.data() + vertex * stride + position->offset, sizeof p);
                return kor::Vec3(p[0], p[1], p[2]);
            };

            struct Meshlet { std::uint32_t vertexOffset, vertexCount, triangleOffset, triangleCount; float sphere[4]; float cone[4]; };
            static_assert(sizeof(Meshlet) == 48);
            // A run of a mesh's triangles, cut by itself: its meshlets' offsets count from its own vertices and triangles.
            struct Piece { std::vector<Meshlet> meshlets; std::vector<std::uint32_t> vertices, triangles; };
            struct Task { std::size_t range; std::uint64_t first, count; std::int64_t offset; };
            // Each mesh's triangles in runs of so many, cut at once on as many threads: a run ends a meshlet where the
            // mesh would have gone on with it, and nowhere else is it cut otherwise than one after another.
            constexpr std::uint64_t RunTriangles = 1u << 18;
            std::vector<Task> tasks;
            for (std::size_t r = 0; r < ranges->size(); ++r) {
                const Value& range = (*ranges)[r];
                const auto field = [&](const char* name) { const Value* found = range.Find(name); return found && found->Number() ? *found->Number() : 0.0; };
                const auto first = static_cast<std::uint64_t>(field("firstIndex")), count = static_cast<std::uint64_t>(field("indexCount"));
                const auto offset = static_cast<std::int64_t>(field("vertexOffset"));
                if (count < 3) { tasks.push_back({ r, first, 0, offset }); continue; }
                for (std::uint64_t at = 0; at + 3 <= count; at += RunTriangles * 3)
                    tasks.push_back({ r, first + at, std::min<std::uint64_t>(RunTriangles * 3, count - at), offset });
            }
            std::vector<Piece> pieces(tasks.size());
            const auto cutRun = [&](const Task& task, std::vector<std::uint32_t>& local) {
                Piece& piece = pieces[&task - tasks.data()];
                // Its vertices' places in the meshlet being made, by the vertex less the least of the run's.
                std::uint32_t least = ~0u, greatest = 0;
                for (std::uint64_t i = 0; i < task.count && task.first + i < indices.size(); ++i) {
                    const auto v = static_cast<std::int64_t>(indices[task.first + i]) + task.offset;
                    if (v < 0 || static_cast<kor::u64>(v) >= vertexCount) continue;
                    least = std::min(least, static_cast<std::uint32_t>(v));
                    greatest = std::max(greatest, static_cast<std::uint32_t>(v));
                }
                if (least > greatest) return;
                if (local.size() < greatest - least + 1) local.assign(greatest - least + 1, ~0u);
                std::vector<std::uint32_t> made;                         // the vertices of the meshlet being made
                std::vector<kor::Vec3> faces;                            // its triangles' normals
                const auto finish = [&](Meshlet meshlet) {
                    if (meshlet.triangleCount == 0) return;
                    kor::Vec3 low(1e30f), high(-1e30f);
                    for (const std::uint32_t v : made) { low = kor::Min(low, at(v)); high = kor::Max(high, at(v)); }
                    const kor::Vec3 middle = (low + high) * 0.5f;
                    float reach = 0.f;
                    for (const std::uint32_t v : made) reach = std::max(reach, kor::Length(at(v) - middle));
                    kor::Vec3 axis(0.f);
                    for (const auto& face : faces) axis = axis + face;
                    float cutoff = 1.f;     // no cone: never culled by it
                    if (const float length = kor::Length(axis); length > 1e-6f && !faces.empty()) {
                        axis = axis / length;
                        float lowest = 1.f;
                        for (const auto& face : faces) lowest = std::min(lowest, kor::Dot(axis, face));
                        if (lowest > 0.1f) cutoff = std::sqrt(1.f - lowest * lowest);
                    }
                    meshlet.sphere[0] = middle.x; meshlet.sphere[1] = middle.y; meshlet.sphere[2] = middle.z; meshlet.sphere[3] = reach;
                    meshlet.cone[0] = axis.x; meshlet.cone[1] = axis.y; meshlet.cone[2] = axis.z; meshlet.cone[3] = cutoff;
                    for (const std::uint32_t v : made) local[v - least] = ~0u;
                    made.clear();
                    faces.clear();
                    piece.meshlets.push_back(meshlet);
                };
                Meshlet meshlet { 0, 0, 0, 0, {}, {} };
                for (std::uint64_t t = 0; t + 3 <= task.count && task.first + t + 3 <= indices.size(); t += 3) {
                    std::uint32_t corner[3];
                    for (int c = 0; c < 3; ++c) corner[c] = static_cast<std::uint32_t>(static_cast<std::int64_t>(indices[task.first + t + c]) + task.offset);
                    if (corner[0] >= vertexCount || corner[1] >= vertexCount || corner[2] >= vertexCount) continue;
                    int fresh = 0;
                    for (int c = 0; c < 3; ++c) fresh += local[corner[c] - least] == ~0u && (c == 0 || corner[c] != corner[0]) && (c < 2 || corner[c] != corner[1]) ? 1 : 0;
                    if (meshlet.vertexCount + fresh > mostVertices || meshlet.triangleCount + 1 > mostTriangles) {
                        finish(meshlet);
                        meshlet = { static_cast<std::uint32_t>(piece.vertices.size()), 0, static_cast<std::uint32_t>(piece.triangles.size()), 0, {}, {} };
                    }
                    std::uint32_t packed = 0;
                    for (int c = 0; c < 3; ++c) {
                        std::uint32_t& place = local[corner[c] - least];
                        if (place == ~0u) {
                            place = meshlet.vertexCount++;
                            piece.vertices.push_back(corner[c]);
                            made.push_back(corner[c]);
                        }
                        packed |= place << (8 * c);
                    }
                    piece.triangles.push_back(packed);
                    ++meshlet.triangleCount;
                    const kor::Vec3 a = at(corner[0]), b = at(corner[1]), c = at(corner[2]);
                    const kor::Vec3 n = kor::Cross(b - a, c - a);
                    if (const float length = kor::Length(n); length > 1e-12f) faces.push_back(n / length);
                }
                finish(meshlet);
            };
            {
                std::atomic<std::size_t> next { 0 };
                const auto work = [&] {
                    std::vector<std::uint32_t> local;   // all ~0 between meshlets: finish puts back what it took
                    for (std::size_t i; (i = next++) < tasks.size();) cutRun(tasks[i], local);
                };
                const std::size_t threads = std::min<std::size_t>({ std::max(1u, std::thread::hardware_concurrency()), 16, tasks.size() });
                std::vector<std::jthread> helpers;
                for (std::size_t t = 1; t < threads; ++t) helpers.emplace_back(work);
                work();
            }
            // The runs one after another, as one mesh's meshlets were cut: each mesh's are together, in order.
            std::vector<Meshlet> meshlets;
            std::vector<std::uint32_t> vertices, triangles, meshRanges;
            double most = 0;
            for (std::size_t i = 0, r = 0; r < ranges->size(); ++r) {
                const auto firstMeshlet = static_cast<std::uint32_t>(meshlets.size());
                for (; i < tasks.size() && tasks[i].range == r; ++i) {
                    Piece& piece = pieces[i];
                    const auto vertexBase = static_cast<std::uint32_t>(vertices.size()), triangleBase = static_cast<std::uint32_t>(triangles.size());
                    for (Meshlet meshlet : piece.meshlets) {
                        meshlet.vertexOffset += vertexBase;
                        meshlet.triangleOffset += triangleBase;
                        meshlets.push_back(meshlet);
                    }
                    vertices.insert(vertices.end(), piece.vertices.begin(), piece.vertices.end());
                    triangles.insert(triangles.end(), piece.triangles.begin(), piece.triangles.end());
                    piece = {};
                }
                const auto cut = static_cast<std::uint32_t>(meshlets.size()) - firstMeshlet;
                most = std::max(most, static_cast<double>(cut));
                meshRanges.insert(meshRanges.end(), { firstMeshlet, cut, 0u, 0u });
            }
            const auto buffer = [&](const std::span<const std::byte> bytes) -> Held {
                auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 16)))
                    .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferDst | kor::Buffer::Usage::eTransferSrc)
                    .SetType(kor::Buffer::Type::eDeviceLocal).Build();
                if (!made.Valid()) return nullptr;
                if (!bytes.empty()) made->Write(bytes);
                return MakeResource(std::move(made));
            };
            Held meshletBuffer = buffer(std::as_bytes(std::span(meshlets))), vertexBuffer = buffer(std::as_bytes(std::span(vertices)));
            Held triangleBuffer = buffer(std::as_bytes(std::span(triangles))), rangeBuffer = buffer(std::as_bytes(std::span(meshRanges)));
            if (!meshletBuffer || !vertexBuffer || !triangleBuffer || !rangeBuffer) return Fail("its meshlets could not be made buffers");
            out.outputs["meshlets"].resource = std::move(meshletBuffer);
            out.outputs["vertices"].resource = std::move(vertexBuffer);
            out.outputs["triangles"].resource = std::move(triangleBuffer);
            out.outputs["meshRanges"].resource = std::move(rangeBuffer);
            out.outputs["count"].value = static_cast<double>(meshlets.size());
            out.outputs["most"].value = most;
            return {};
        }

    auto Runtime::Impl::BuildRayScene(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            if (!kor::Context::SupportsRayTracing()) return Fail("this device has no ray tracing");
            const auto wired = node.inputs.find("meshes");
            if (wired == node.inputs.end() || !wired->second.link) return Fail("'meshes' is each element's mesh: wire the Filter's Mesh.mesh the MeshPack packs");
            const Node* filter = Origin(wired->second.link->node);
            if (!filter || !IsEach(filter->type)) return Fail("'meshes' comes from a Filter: its Mesh.mesh");
            const auto given = inputs.find("mesh");
            Held pack = given == inputs.end() ? nullptr : given->second.resource;
            while (pack && pack->replacedBy) pack = pack->replacedBy;
            if (!pack || pack->kind != "Mesh" || !pack->handle) return Fail("'mesh' is the meshes as one: the MeshPack's mesh");
            const auto rangesGiven = inputs.find("ranges");
            const auto* ranges = rangesGiven == inputs.end() ? nullptr : rangesGiven->second.value.Items();
            if (!ranges) return Fail("'ranges' is each mesh's part of it: the MeshPack's ranges");

            auto state = std::make_shared<RayState>();
            state->filter = filter->id;
            if (const auto it = inputs.find("masked"); it != inputs.end() && it->second.value.String() && !it->second.value.String()->empty() && world)
                state->masked = world->Find(*it->second.value.String());
            if (const auto it = inputs.find("noShadow"); it != inputs.end() && it->second.value.String() && !it->second.value.String()->empty() && world)
                state->unshadowing = world->Find(*it->second.value.String());
            const auto mesh = kor::capi::MeshOf(pack->handle);
            for (const auto& range : *ranges) {
                const auto field = [&](const char* name) { const Value* found = range.Find(name); return found && found->Number() ? *found->Number() : 0.0; };
                const auto indexCount = static_cast<kor::u64>(field("indexCount"));
                if (indexCount < 3) { state->bottoms.push_back(nullptr); continue; }
                kor::AccelerationStructure::Builder builder;
                builder.AddGeometry({ .mesh = mesh, .firstVertex = static_cast<kor::u64>(std::max(0.0, field("vertexOffset"))), .vertexCount = 0,
                                      .firstIndex = static_cast<kor::u64>(field("firstIndex")), .indexCount = indexCount });
                auto bottom = builder.Build();
                if (!bottom.Valid()) return Fail(bottom.Failure() ? bottom.Failure()->message : std::string("a mesh's structure could not be made"));
                state->bottoms.push_back(MakeResource(std::move(bottom)));
            }
            const auto buffer = [](const kor::u64 bytes, kor::ResourceRef<kor::Buffer>& written) -> Held {
                auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<kor::u64>(bytes, 16)))
                    .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc).SetType(kor::Buffer::Type::eDynamic).Build();
                if (!made.Valid()) return nullptr;
                written = kor::ResourceRef<kor::Buffer>(made);
                return MakeResource(std::move(made));
            };
            state->scene = buffer(16, state->sceneWritten);
            const EntityList* list = built.contains(filter->id) ? built.at(filter->id).listed.get() : nullptr;
            state->meshOfRoom = std::max<std::size_t>(64, list ? list->entities.size() * 2 : 0);
            state->meshOf = buffer(state->meshOfRoom * 4, state->meshOfWritten);
            if (!state->scene || !state->meshOf) return Fail("its buffers could not be made");
            const std::uint32_t none[4] { ~0u, 0u, 0u, 0u };
            state->sceneWritten->Write(std::span<const std::uint32_t>(none));
            out.rays = state;
            UpdateRays(node.id, out, true);
            out.outputs["scene"].resource = state->scene;
            out.outputs["meshOf"].resource = state->meshOf;
            return {};
        }

    auto Runtime::Impl::UpdateRays(const std::string& id, Built& node, const bool building) -> void
    {
            RayState& state = *node.rays;
            const auto found = built.find(state.filter);
            const EntityList* list = found == built.end() ? nullptr : found->second.listed.get();
            if (!list || !world) return;
            // What the instances are now, as one number.
            std::uint64_t now = 1469598103934665603ull;
            const auto mix = [&](const void* data, const std::size_t size) {
                const auto* bytes = static_cast<const unsigned char*>(data);
                for (std::size_t i = 0; i < size; ++i) now = (now ^ bytes[i]) * 1099511628211ull;
            };
            std::vector<kecs::Mat4> placed;
            placed.reserve(list->entities.size());
            for (std::size_t i = 0; i < list->entities.size(); ++i) {
                const kecs::Entity entity = list->entities[i];
                placed.push_back(world->Alive(entity) ? world->WorldMatrix(entity) : kecs::Mat4 {});
                mix(placed.back().m.data(), sizeof(float) * 16);
                const std::uint32_t group = list->grouped && i < list->groupOf.size() ? list->groupOf[i] : 0u;
                const bool masked = state.masked != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.masked);
                const bool unshadowing = state.unshadowing != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.unshadowing);
                mix(&group, sizeof group);
                mix(&masked, sizeof masked);
                mix(&unshadowing, sizeof unshadowing);
            }
            if (state.made && now == state.seen) return;
            state.seen = now;
            state.made = true;
            kor::AccelerationStructure::Builder builder;
            std::vector<std::uint32_t> meshOf(list->entities.size(), 0u);
            for (std::size_t i = 0; i < list->entities.size(); ++i) {
                const std::uint32_t group = list->grouped && i < list->groupOf.size() ? list->groupOf[i] : 0u;
                meshOf[i] = group;
                if (group >= state.bottoms.size() || !state.bottoms[group]) continue;
                kor::Mat4 transform;
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) transform[column][row] = placed[i].At(column, row);
                const kecs::Entity entity = list->entities[i];
                builder.AddInstance({ .blas = kor::capi::AccelerationStructureOf(state.bottoms[group]->handle), .transform = transform,
                                      .instanceCustomIndex = static_cast<kor::u32>(i), .hitGroupIndex = 0,
                                      .opaque = !(state.masked != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.masked)),
                                      .mask = static_cast<kor::u8>(state.unshadowing != kecs::NoComponent && world->Alive(entity) && world->Has(entity, state.unshadowing) ? 0x02 : 0x03) });
            }
            if (builder.instances.empty()) {
                // Nothing left to place: no scene, rather than the one of what was there before.
                node.outputs["count"].value = 0.0;
                if (!state.top) return;
                retiredResources.push_back(std::move(state.top));
                const std::uint32_t none[4] { ~0u, 0u, 0u, 0u };
                state.sceneWritten->Write(std::span<const std::uint32_t>(none));
                return;
            }
            auto top = builder.Build();
            if (!top.Valid()) return;
            if (state.top) retiredResources.push_back(std::move(state.top));
            state.top = MakeResource(std::move(top));
            const auto index = kor::Bindless::AccelerationStructure(kor::capi::AccelerationStructureOf(state.top->handle));
            const std::uint32_t scene[4] { index.Valid() ? index.index : ~0u, static_cast<std::uint32_t>(list->entities.size()), 0u, 0u };
            state.sceneWritten->Write(std::span<const std::uint32_t>(scene));
            // More instances than its row buffer has room for: the node is made again, with more.
            if (meshOf.size() > state.meshOfRoom) { node.signature.clear(); stale = true; }
            else if (!meshOf.empty()) state.meshOfWritten->Write(std::span<const std::uint32_t>(meshOf));
            node.outputs["structure"].resource = state.top;
            node.outputs["count"].value = static_cast<double>(list->entities.size());
            // What is wired to the structure itself is built again with the new one; what reads it by its index is not.
            const bool direct = current && std::ranges::any_of(current->nodes, [&](const Node& other) {
                return std::ranges::any_of(other.inputs, [&](const auto& input) { return input.second.link && input.second.link->node == id && input.second.link->output == "structure"; });
            });
            if (!building && direct) { node.stamp = ++counter; stale = true; }
        }

    auto Runtime::Impl::IndirectCommands(const Inputs& inputs, const bool gpu) -> Expected<std::pair<std::vector<std::byte>, std::size_t>>
    {
            const auto value = [&](const char* name) -> const Value* { const auto it = inputs.find(name); return it == inputs.end() ? nullptr : &it->second.value; };
            const Value* rangesValue = value("ranges");
            const Value* groupsValue = value("groups");
            const auto* ranges = rangesValue ? rangesValue->Items() : nullptr;
            const auto* groups = groupsValue ? groupsValue->Items() : nullptr;
            if (!ranges) return Fail("'ranges' is each mesh's part of the mesh drawn: a MeshPack's ranges");
            if (!groups) return Fail("'groups' is how many of each to draw: a Filter's groups, grouped by Mesh");
            if (ranges->size() != groups->size())
                return Fail(std::format("there are {} meshes and {} groups: group the Filter by Mesh, and pack its Mesh.mesh", ranges->size(), groups->size()));
            const auto number = [](const Value& of, const char* name) { const Value* found = of.Find(name); return found && found->Number() ? *found->Number() : 0.0; };
            std::vector<std::byte> bytes(ranges->size() * 20, std::byte { 0 });
            std::size_t instances = 0;
            for (std::size_t i = 0; i < ranges->size(); ++i) {
                const auto count = static_cast<std::uint32_t>(number((*groups)[i], "count"));
                const std::uint32_t words[5] { static_cast<std::uint32_t>(number((*ranges)[i], "indexCount")), gpu ? 0u : count,
                                               static_cast<std::uint32_t>(number((*ranges)[i], "firstIndex")),
                                               std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(number((*ranges)[i], "vertexOffset"))),
                                               static_cast<std::uint32_t>(number((*groups)[i], "first")) };
                std::memcpy(bytes.data() + i * 20, words, 20);
                instances += count;
            }
            return std::pair { std::move(bytes), instances };
        }

    auto Runtime::Impl::OnTheGpu(const Inputs& inputs) -> bool
    {
            const auto it = inputs.find("mode");
            return it != inputs.end() && it->second.value.String() && *it->second.value.String() == "eGPU";
        }

    auto Runtime::Impl::BuildIndirectDraws(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            if (const auto it = inputs.find("mode"); it != inputs.end() && !it->second.value.IsNull())
                if (const std::string* mode = it->second.value.String(); !mode || (*mode != "eCPU" && *mode != "eGPU")) return Fail("'mode' is eCPU or eGPU");
            const bool gpu = OnTheGpu(inputs);
            auto commands = IndirectCommands(inputs, gpu);
            if (!commands) return Fail(commands.error());
            auto& [bytes, instances] = *commands;
            // Written by the CPU each frame it changes, and read as a draw's commands, each frame in flight its own. On the
            // GPU's, one on the device: written in by a pass of its own each frame, for a pass after it to count into.
            auto buffer = gpu
                ? kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 20)))
                      .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eIndirect) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc
                                | kor::Buffer::Usage::eTransferDst)
                      .SetType(kor::Buffer::Type::eDeviceLocal).Build()
                : kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(bytes.size(), 20)))
                      .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eIndirect) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc)
                      .SetType(kor::Buffer::Type::eDynamic).SetIsPerFrame(true).Build();
            if (!buffer.Valid()) return Fail(buffer.Failure() ? buffer.Failure()->message : std::string("its buffer could not be made"));
            auto packed = std::make_shared<Packed>();
            packed->buffer = kor::ResourceRef<kor::Buffer>(buffer);
            packed->held = MakeResource(std::move(buffer));
            packed->held->node = node.id;
            if (!bytes.empty() && !gpu) packed->buffer->Write(std::span<const std::byte>(bytes));
            packed->bytes = std::move(bytes);
            packed->room = instances;
            packed->everyFrame = gpu;
            out.outputs["commands"].resource = packed->held;
            out.outputs["out"].resource = packed->held;     // what it holds, to be looked at: its commands
            out.outputs["drawCount"].value = static_cast<double>(packed->bytes.size() / 20);
            out.outputs["stride"].value = 20;
            if (gpu) {
                auto visible = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(instances, 1) * 4))
                    .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc | kor::Buffer::Usage::eTransferDst)
                    .SetType(kor::Buffer::Type::eDeviceLocal).Build();
                if (!visible.Valid()) return Fail("its visible list could not be made a buffer");
                Held held = MakeResource(std::move(visible));
                held->node = node.id;
                out.outputs["visible"].resource = std::move(held);
                // Its commands start at none every frame on the GPU, written as the frame is recorded — a pass of its
                // own, before anything counts into them. A buffer the CPU writes is copied to the other frames' copies
                // after, what was counted into it with it.
                auto plan = std::make_shared<PassPlan>();
                plan->uses.push_back({ MemberOf(packed->held.get()), false, true });
                auto now = std::make_shared<std::vector<std::byte>>(packed->bytes);
                plan->prepare = [packed, now] { *now = packed->bytes; };
                plan->record = [target = kor::ResourceRef<const kor::Buffer>(packed->buffer), now](kor::CommandBuffer& cb, const PassContext&) {
                    // An update a command buffer carries is of 64 KiB at most: written in as many as it takes.
                    constexpr std::size_t Most = 65536;
                    for (std::size_t at = 0; at < now->size(); at += Most)
                        cb.FillBuffer(target, now->data() + at, at, std::min(Most, now->size() - at));
                };
                out.pass = std::move(plan);
            }
            out.packed = std::move(packed);
            return {};
        }

    auto Runtime::Impl::BuildPrimitive(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            static const std::string shapes = [] {
                std::string all;
                const auto& names = DefaultMeshNames();
                for (std::size_t i = 0; i < names.size(); ++i)
                    all += std::format("{}e{}{}", i == 0 ? "" : i + 1 == names.size() ? " or " : ", ", static_cast<char>(std::toupper(static_cast<unsigned char>(names[i][0]))), names[i].substr(1));
                return all;
            }();
            std::string shape = "eCube";
            if (const auto it = inputs.find("shape"); it != inputs.end() && !it->second.value.IsNull()) {
                if (!it->second.value.String()) return Fail(std::format("'shape' is {}", shapes));
                shape = *it->second.value.String();
            }
            // eCube is "cube": the name of the shape, as the default meshes have it.
            std::string name = shape.size() > 1 && shape[0] == 'e' ? shape.substr(1) : std::string();
            if (!name.empty()) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
            const auto data = ShapeData(name);
            if (!data) return Fail(std::format("'{}' is no shape: {}", shape, shapes));
            const VertexData vertices = ToVertexData(*data, DefaultTangents());
            auto resource = MeshOf(vertices);
            if (!resource) return Fail(resource.error());
            (*resource)->node = node.id;
            out.outputs["out"].resource = std::move(*resource);
            Value::List attributes;
            for (const auto& attribute : vertices.attributes)
                attributes.emplace_back(Value::Members { { "semantic", attribute.semantic }, { "count", static_cast<double>(attribute.count) } });
            auto layout = LayoutValue(Value(std::move(attributes)));
            if (!layout) return Fail(layout.error());
            out.outputs["layout"].value = std::move(*layout);
            return {};
        }

    auto Runtime::Impl::Listed(std::string name) -> std::string
    {
            if (const auto open = name.find('['); open != std::string::npos)
                if (const auto close = name.find(']', open); close != std::string::npos) name.erase(open + 1, close - open - 1);
            return name;
        }

    auto Runtime::Impl::BundleNote(const Node& node, const Built& made) const -> std::string
    {
            if (const auto said = bundleNotes.find(node.id); said != bundleNotes.end()) return said->second;
            const auto filled = fromBundle.find(node.id);
            if (filled == fromBundle.end() || filled->second.empty()) return {};
            const std::string prefix = filled->second.begin()->substr(0, filled->second.begin()->find('.') + 1);
            std::vector<Port> ports = made.ports;
            if (made.pipeline) ports.insert(ports.end(), made.pipeline->ports.begin(), made.pipeline->ports.end());
            std::string unmatched, missing;
            for (const auto& name : filled->second)
                if (std::ranges::find(ports, name, &Port::name) == ports.end()) unmatched += std::format("{}'{}'", unmatched.empty() ? "" : ", ", name.substr(prefix.size()));
            for (const auto& port : ports)
                if (port.name.starts_with(prefix) && !node.inputs.contains(port.name)) missing += std::format("{}'{}'", missing.empty() ? "" : ", ", port.name.substr(prefix.size()));
            std::string note;
            if (!unmatched.empty()) note = std::format("the bundle has {}, which it has not", unmatched);
            if (!missing.empty()) note += std::format("{}it has {}, which the bundle has not", note.empty() ? "" : "; ", missing);
            return note;
        }

    auto Runtime::Impl::Build(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const NodeType* type = FindType(node.type);
            if (!type) return Fail(std::format("there is no node type '{}'", node.type));
            if (type->builder) {
                return IsPipeline(type->name) ? BuildPipeline(node, *type->builder, inputs, out) : BuildBuilder(node, *type->builder, inputs, out);
            }
            // The engine's own: only the inputs its type lists.
            // A ShaderBuffer has its struct's fields besides: which, it says itself, once it has read its shader.
            const bool fields = IsMapped(node.type) || node.type == "Bundle";     // a Bundle has its members'
            // A Script node has its script's: its fields, and what each of its resources is made with — its to say.
            const bool script = node.type == "Script";
            // A Draw or a Dispatch has its pipeline's besides, which it says once it is built: and what it took before, to say where that went.
            const bool draws = node.type == "Draw" || node.type == "Dispatch" || node.type == "TraceRays";
            // A lens took a view and an extent before: what became of them is its to say.
            const bool lens = node.type == "Perspective" || node.type == "Orthographic" || node.type == "Frustum" || node.type == "Oblique" || node.type == "Paraboloid";
            const auto own = [&](const std::string& name) {
                return (draws && (name.starts_with("param.") || name.starts_with("push.") || name == "with" || name == "without"))
                    || (lens && (name == "view" || name == "extent")) || (node.type == "ForEach" && (name == "select" || name == "which" || name == "entity"));
            };
            for (const auto& name : inputs | std::views::keys)
                if (!(fields && name.starts_with("field.")) && !(script && name.find('.') != std::string::npos) && !own(name) && std::ranges::find(type->inputs, Listed(name), &Port::name) == type->inputs.end())
                    return Fail(std::format("a {} has no input '{}'", type->name, name));
            if (type->category == "Passes") {
                const auto truthy = [](const Value& value) { return value.Bool() ? *value.Bool() : value.Number() && *value.Number() != 0.0; };
                // Left out: there is the node, and no pass of it.
                const auto enabled = inputs.find("enabled");
                if (enabled != inputs.end() && !truthy(enabled->second.value)) return {};
                // Left out of the views it is not for: the game's, or an editor's scene view.
                if (const auto views = inputs.find("views"); views != inputs.end() && views->second.value.String()) {
                    const std::string& which = *views->second.value.String();
                    if (which != "all" && which != "game" && which != "editor") return Fail(std::format("'views' is all, game or editor, not '{}'", which));
                    if ((which == "game" && editorView) || (which == "editor" && !editorView)) return {};
                }
                const auto async = inputs.find("async");
                const bool onAsync = async != inputs.end() && truthy(async->second.value);
                Inputs rest = inputs;
                rest.erase("enabled");
                rest.erase("async");
                rest.erase("views");
                auto built = BuildPass(node, rest, out);
                if (built && onAsync && out.pass) {
                    out.pass->async = true;
                    if (!kor::Context::SupportsAsyncCompute()) out.status.note = "async: this device has no second queue, so it runs on the graphics queue, in order";
                    else if (out.pass->readsScreen || out.pass->writesScreen) out.status.note = "async: it uses the screen, so it stays on the graphics queue";
                }
                return built;
            }
            if (node.type == "Note") return {};     // words: there is nothing of it to build
            if (node.type == "DebugView") {
                const auto picture = inputs.find("picture");
                if (picture == inputs.end() || !picture->second.resource) return Fail("'picture' is the picture shown in its place: wire an image to it");
                out.outputs["out"].resource = picture->second.resource;
                out.outputs["shown"].value = Value(node.id == debugView);
                return {};
            }
            if (node.type == "Screen") {
                auto screen = std::make_shared<Resource>();
                screen->kind = "Image";
                screen->node = node.id;
                screen->screen = true;
                out.outputs["out"].resource = std::move(screen);
                // How big the window is: what an image that is to be shown in it, a pixel for a pixel, is sized by.
                const kor::UVec2 size { std::max(screenExtent.x, 1u), std::max(screenExtent.y, 1u) };
                out.outputs["extent"].value = Value(Value::List { size.x, size.y, 1 });
                out.outputs["size"].value = Value(Value::List { size.x, size.y });
                out.outputs["width"].value = size.x;
                out.outputs["height"].value = size.y;
                out.outputs["aspect"].value = static_cast<double>(size.x) / static_cast<double>(size.y);
                out.outputs["format"].value = screenFormat;
                out.outputs["scale"].value = static_cast<double>(screenScale);
                return {};
            }
            if (ReadsTheWorld(node.type)) return BuildLive(node, inputs, out);
            if (node.type == "WriteBack") {
                // Checked here; what it does, it does each frame (Runtime::WriteBack).
                const auto buffer = inputs.find("buffer");
                if (buffer == inputs.end() || !BufferRefOf(buffer->second.resource.get()).Get()) return Fail("'buffer' needs a buffer wired in: what is read back");
                const auto each = inputs.find("forEach");
                const std::string* id = each == inputs.end() ? nullptr : each->second.value.String();
                const Node* forEach = id && current ? CurrentNode(*id) : nullptr;
                if (!forEach || !IsEach(forEach->type)) return Fail("'forEach' is the id of the ForEach whose elements the buffer's are");
                const auto stride = inputs.find("stride");
                const double* bytes = stride == inputs.end() ? nullptr : stride->second.value.Number();
                if (!bytes || *bytes < 4) return Fail("'stride' is how many bytes an element is: 4 or more");
                for (const char* name : { "position", "rotation" })
                    if (const auto at = inputs.find(name); at != inputs.end() && !at->second.value.IsNull()) {
                        const double* offset = at->second.value.Number();
                        if (!offset || *offset < 0 || *offset + (name[0] == 'p' ? 12 : 16) > *bytes) return Fail(std::format("'{}' is where in an element it is, in bytes, within its stride", name));
                    }
                if (const auto fields = inputs.find("fields"); fields != inputs.end() && !fields->second.value.IsNull()) {
                    const auto* members = fields->second.value.Named();
                    if (!members) return Fail("'fields' is { \"Component.field\": offset }, each where it is in an element");
                    for (const auto& [name, offset] : *members)
                        if (name.find('.') == std::string::npos || !offset.Number()) return Fail(std::format("'fields' has '{}': each is `Component.field`, and where it is in bytes", name));
                }
                return {};
            }
            if (node.type == "MeshBuffers") {
                // A mesh's buffers, as buffers: where its positions are, and its indices — a part's, of its whole's.
                const auto given = inputs.find("mesh");
                Held mesh = given == inputs.end() ? nullptr : given->second.resource;
                while (mesh && mesh->replacedBy) mesh = mesh->replacedBy;
                if (!mesh || mesh->kind != "Mesh") return Fail("'mesh' needs a mesh wired in: a Primitive, an entity's, a model's");
                if (mesh->pending) return Fail("its mesh is still being read");
                const Resource* whole = mesh->part ? mesh->source.get() : mesh.get();
                if (!whole || !whole->handle || whole->buffers.empty()) return Fail("its mesh has no buffers of its own to read: one made another way");
                const auto device = kor::capi::MeshOf(whole->handle);
                const auto& layout = device->Layout();
                const auto position = std::ranges::find(layout.attributes, std::string_view("POSITION"), [](const auto& a) { return std::string_view(a.semantic); });
                if (position == layout.attributes.end() || position->binding >= whole->buffers.size()) return Fail("its mesh has no POSITION");
                const auto binding = std::ranges::find(layout.bindings, position->binding, &kor::VertexInputBindingDescription::binding);
                const bool indexed = device->IndexBuffer().has_value();
                out.outputs["vertices"].resource = whole->buffers[position->binding];
                if (indexed) out.outputs["indices"].resource = whole->buffers.back();
                out.outputs["stride"].value = static_cast<double>(binding == layout.bindings.end() ? 0 : binding->stride);
                out.outputs["position"].value = static_cast<double>(position->offset);
                out.outputs["vertexCount"].value = static_cast<double>(mesh->part ? mesh->vertexCount : device->VertexCount());
                out.outputs["indexCount"].value = static_cast<double>(mesh->part ? mesh->indexCount : indexed ? device->IndexCount().value_or(0) : 0);
                out.outputs["firstIndex"].value = static_cast<double>(mesh->part ? mesh->firstIndex : 0);
                out.outputs["vertexOffset"].value = static_cast<double>(mesh->part ? mesh->vertexOffset : 0);
                // And where its normals and its first UVs are, when it has them: what a mesh shader or a hit shader reads
                // of a vertex besides where it is.
                for (const auto& [semantic, name] : { std::pair { "NORMAL", "normals" }, std::pair { "UV", "uvs" } }) {
                    const auto it = std::ranges::find(layout.attributes, std::string_view(semantic), [](const auto& a) { return std::string_view(a.semantic); });
                    if (it == layout.attributes.end() || it->binding >= whole->buffers.size()) continue;
                    const auto bound = std::ranges::find(layout.bindings, it->binding, &kor::VertexInputBindingDescription::binding);
                    out.outputs[name].resource = whole->buffers[it->binding];
                    out.outputs[std::format("{}Stride", name)].value = static_cast<double>(bound == layout.bindings.end() ? 0 : bound->stride);
                    out.outputs[std::format("{}Offset", name)].value = static_cast<double>(it->offset);
                }
                return {};
            }
            if (node.type == "MeshPack") return BuildMeshPack(node, inputs, out);
            if (node.type == "Meshlets") return BuildMeshlets(inputs, out);
            if (node.type == "RayScene") return BuildRayScene(node, inputs, out);
            if (node.type == "IndirectDraws") return BuildIndirectDraws(node, inputs, out);
            if (node.type == "ShaderBuffer") return BuildShaderBuffer(node, inputs, out);
            if (node.type == "BufferBuilder") return BuildBufferBuilder(node, inputs, out);
            if (node.type == "Collect") { CollectAll(node, out); return {}; }
            if (node.type == "Primitive") return BuildPrimitive(node, inputs, out);
            if (node.type == "Script") return BuildScript(node, inputs, out);
            if (node.type == "ImageFile") {
                const auto file = inputs.find("file");
                const std::string* name = file == inputs.end() ? nullptr : file->second.value.String();
                if (!name || name->empty()) return Fail("'file' is the picture's file: under the project's assets folder, or by its whole path");
                const Held held = Asset(AssetId(*name, -1));
                if (!held) return Fail(std::format("'{}' could not be read as a picture", AssetFile(*name).generic_string()));
                out.outputs["out"].resource = held;
                return {};
            }
            if (node.type == "Reference") {
                const auto it = inputs.find("node");
                const std::string* target = it == inputs.end() ? nullptr : it->second.value.String();
                if (!target) return Fail("'node' is the id of the node it stands for");
                const auto found = built.find(*target);
                if (found == built.end()) return Fail(std::format("there is no node '{}'", *target));
                if (!found->second.status.ok) return Fail(std::format("'{}', which it stands for, failed", *target));
                out.outputs = found->second.outputs;
                return {};
            }
            return BuildValue(node, inputs, out);
        }

    auto Runtime::Impl::AddOutputs(const Node& node, const Inputs& inputs, Built& out) -> void
    {
            const auto add = [&](const std::string& name, Value value) {
                if (!out.outputs.contains(name)) out.outputs[name].value = std::move(value);
            };
            // What the object says of itself comes first: it is what is so, whatever was asked for.
            if (const auto made = out.outputs.find("out"); made != out.outputs.end() && made->second.resource && made->second.resource->handle) {
                const Resource& resource = *made->second.resource;
                if (const auto image = ImageRefOf(&resource); image.Get()) {
                    const auto extent = image->Extent();
                    add("extent", Value(Value::List { extent.x, extent.y, extent.z }));
                    add("size", Value(Value::List { extent.x, extent.y }));
                    add("width", extent.x);
                    add("height", extent.y);
                    add("depth", extent.z);
                    add("mipLevels", image->MipLevels());
                    add("arrayLayers", image->ArrayLayers());
                } else if (const auto buffer = BufferRefOf(&resource); buffer.Get()) {
                    add("bytes", static_cast<double>(buffer->size()));
                } else if (resource.kind == "Framebuffer") {
                    const auto extent = kor::capi::FramebufferOf(resource.handle)->Extent();
                    add("size", Value(Value::List { extent.x, extent.y }));
                    add("width", extent.x);
                    add("height", extent.y);
                } else if (resource.kind == "Mesh") {
                    const auto mesh = kor::capi::MeshOf(resource.handle);
                    add("vertexCount", static_cast<double>(mesh->VertexCount()));
                    add("indexCount", static_cast<double>(mesh->IndexCount().value_or(0)));
                }
            }
            const NodeType* type = FindType(node.type);
            const auto ports = type ? PortsFor(*type, node) : std::vector<Port> {};
            for (const auto& [name, input] : inputs)
                if (!input.resource && !input.value.IsNull()) add(name, input.value);
            for (const auto& port : ports)
                if (port.kind == PortKind::eValue && !port.initial.IsNull()) add(port.name, port.initial);
            // Parts as a whole.
            std::map<std::string, Value::List> wholes;
            for (const auto& port : ports) {
                if (port.name.size() < 3 || port.name[port.name.size() - 2] != '.') continue;
                const char part = port.name.back();
                if (part != 'x' && part != 'y' && part != 'z' && part != 'w') continue;
                const auto value = out.outputs.find(port.name);
                if (value == out.outputs.end() || !value->second.value.Number()) continue;
                wholes[port.name.substr(0, port.name.size() - 2)].push_back(value->second.value);
            }
            for (auto& [name, parts] : wholes) add(name, Value(std::move(parts)));
        }

    auto Runtime::Impl::InParts(const Node& node, Inputs& inputs) -> Expected<void>
    {
            const NodeType* type = FindType(node.type);
            if (!type) return {};
            for (auto it = inputs.begin(); it != inputs.end();) {
                const auto parts = PartsOf(*type, it->first);
                if (parts.empty()) { ++it; continue; }
                for (const auto& part : parts)
                    if (inputs.contains(part)) return Fail(std::format("'{}' is given whole, and '{}' on its own as well: one or the other", it->first, part));
                // Left open for a wire, and none in it yet: not given.
                if (it->second.value.IsNull() && !it->second.resource) { it = inputs.erase(it); continue; }
                const auto numbers = NumbersOf(it->second.value, it->first.c_str());
                if (!numbers || it->second.resource) return Fail(std::format("'{}' is a vector: a number for each of x, y and z", it->first));
                for (std::size_t i = 0; i < parts.size() && i < numbers->size(); ++i) {
                    Resolved part;
                    part.value = (*numbers)[i];
                    part.linked = it->second.linked;
                    inputs.emplace(parts[i], std::move(part));
                }
                it = inputs.erase(it);
            }
            return {};
        }

    auto Runtime::Impl::FillDefaults(const Node& node, Inputs& inputs) -> void
    {
            const NodeType* type = FindType(node.type);
            if (!type) return;
            for (auto& [name, input] : inputs) {
                const Value::Members* given = input.value.Named();
                if (!given || input.resource) continue;
                // One given more than once is listed once: `color[1].clear` is `color[].clear`.
                std::string listed = name;
                if (const auto open = listed.find('['); open != std::string::npos)
                    if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
                const auto port = std::ranges::find(type->inputs, listed, &Port::name);
                const Value::Members* defaults = port == type->inputs.end() ? nullptr : port->initial.Named();
                if (!defaults) continue;
                Value::Members merged = *given;
                for (const auto& [field, value] : *defaults)
                    if (std::ranges::find_if(merged, [&](const auto& m) { return m.first == field; }) == merged.end()) merged.emplace_back(field, value);
                // So does each struct of a list in it: a blend attachment given its factors still writes every channel, as
                // the framework's has it, not none.
                static const std::map<std::string, Value::Members, std::less<>> itemDefaults {
                    { "colorBlendState.attachments", { { "blendEnable", false }, { "srcColorBlendFactor", "eOne" }, { "dstColorBlendFactor", "eZero" },
                                                       { "colorBlendOp", "eAdd" }, { "srcAlphaBlendFactor", "eOne" }, { "dstAlphaBlendFactor", "eZero" },
                                                       { "alphaBlendOp", "eAdd" }, { "colorWriteMask", Value::List { "eR", "eG", "eB", "eA" } } } },
                };
                for (auto& [field, value] : merged) {
                    const auto items = itemDefaults.find(listed + "." + field);
                    if (items == itemDefaults.end() || !value.Items()) continue;
                    Value::List completed;
                    for (const Value& item : *value.Items()) {
                        Value::Members each = item.Named() ? *item.Named() : Value::Members {};
                        for (const auto& [name, initial] : items->second)
                            if (std::ranges::find_if(each, [&](const auto& m) { return m.first == name; }) == each.end()) each.emplace_back(name, initial);
                        completed.push_back(item.Named() ? Value(std::move(each)) : item);
                    }
                    value = Value(std::move(completed));
                }
                input.value = Value(std::move(merged));
            }
        }
}
