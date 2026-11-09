// kgraph: the runtime's own, building what each node makes: resources, pipelines, passes, values.

#include "runtime_impl.h"

namespace kgraph
{
    auto Runtime::Impl::CurrentNode(const std::string_view id) const -> const Node*
    {
            if (!current) return nullptr;
            if (currentById.size() != current->nodes.size()) return current->Find(id);
            const auto it = currentById.find(id);
            return it == currentById.end() ? nullptr : it->second;
        }

    auto Runtime::Impl::FromBundle(const std::string_view id, const std::string& name) const -> bool
    {
            const auto it = fromBundle.find(id);
            return it != fromBundle.end() && it->second.contains(name);
        }

    auto Runtime::Impl::ScriptNamed(const std::string_view name) const -> const Scripted*
    {
            if (source) return source->ScriptNamed(name);
            const auto it = scripts.find(name);
            return it == scripts.end() ? nullptr : &it->second;
        }

    auto Runtime::Impl::ScriptsVersion() const -> std::uint64_t
    { return source ? source->ScriptsVersion() : scriptsVersion; }

    auto Runtime::Impl::Kinds() const -> const std::vector<MaterialKind>&
    { return materialKinds.empty() && source ? source->Kinds() : materialKinds; }

    auto Runtime::Impl::WriteMaterials() -> void
    { materialsToWrite = true; }

    auto Runtime::Impl::WriteMaterialsNow() -> void
    {
            if (!std::exchange(materialsToWrite, false)) return;
            materialNotes = WriteMaterialModules(GeneratedFolder(), materialKinds, generatedOwned);
        }

    auto Runtime::Impl::GeneratedFolder() -> const std::filesystem::path&
    {
            if (generatedShaders.empty()) {
                generatedShaders = std::filesystem::temp_directory_path() / "koral" / "materials";
                kor::Shader::AddSearchPath(generatedShaders);
            }
            return generatedShaders;
        }

    auto Runtime::Impl::WriteSortModule(const std::string& element) -> Expected<std::string>
    {
            static const std::regex named(R"([A-Za-z_]\w*(\.[A-Za-z_]\w*)*)");
            if (!std::regex_match(element, named)) return Fail(std::format("'{}' is no type: a shader's float or uint, or a struct of a module, as module.Type", element));
            const std::size_t dot = element.rfind('.');
            const std::string type = dot == std::string::npos ? element : element.substr(dot + 1);
            const std::string imported = dot == std::string::npos ? std::string() : std::format("import {};\n", element.substr(0, dot));
            std::string module = "koral_sort_of_" + element;
            std::ranges::replace(module, '.', '_');
            std::string entries;
            for (const auto& [entry, step] : { std::pair { "bitonic", "BitonicStep" }, { "oddEven", "OddEvenStep" } })
                entries += std::format("[shader(\"compute\")]\n[numthreads(256, 1, 1)]\nvoid {}(uint3 id : SV_DispatchThreadID) {{ {}<{}>(koralSorted, koralSortPush, id.x); }}\n\n", entry, step, type);
            const std::filesystem::path& folder = GeneratedFolder();
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            const std::string source = std::format("// Generated for a sort of {}: not to be edited.\nimport koral_compare_sort;\n{}\n"
                                                   "[[vk::push_constant]] ConstantBuffer<KoralSortPush> koralSortPush;\nRWStructuredBuffer<{}> koralSorted;\n\n{}", element, imported, type, entries);
            if (!WriteIfChanged(folder / (module + ".slang"), source)) return Fail(std::format("{} could not be written", (folder / (module + ".slang")).generic_string()));
            return module;
        }

    auto Runtime::Impl::Find(const std::string_view id) const -> const Built*
    {
            if (const auto it = built.find(id); it != built.end()) return &it->second;
            if (const auto of = turns.find(id); of != turns.end() && !of->second.empty())
                if (const auto it = built.find(of->second.front()); it != built.end()) return &it->second;
            return nullptr;
        }

    auto Runtime::Impl::Unwatch(Built& node) -> void
    {
            for (auto& [shader, watch] : node.watches)
                if (shader.Alive()) const_cast<kor::Shader&>(*shader).UnregisterReloadCallback(watch);
            node.watches.clear();
        }

    auto Runtime::Impl::Retire(Built&& old) -> void
    { retired.push_back(std::move(old)); }

    auto Runtime::Impl::LetGo() -> void
    {
            retiring = std::exchange(retired, {});
            retiringResources = std::exchange(retiredResources, {});
            retiringSets = std::exchange(retiredSets, {});
            // A tuned mesh nothing holds any more: every number it was dragged through would stay otherwise. Asked for
            // again, it is made again; `$cube` as it starts is kept.
            for (auto it = defaultMeshes.begin(); it != defaultMeshes.end();) {
                if (it->first.contains('?') && it->second.use_count() == 1) {
                    retiredResources.push_back(std::move(it->second));
                    it = defaultMeshes.erase(it);
                } else ++it;
            }
        }

    auto Runtime::Impl::Watch(Built& node, kor::ResourceRef<const kor::Shader> shader) -> void
    {
            const kor::u64 watch = const_cast<kor::Shader&>(*shader).RegisterReloadCallback([this] { reloaded = true; });
            node.watches.emplace_back(std::move(shader), watch);
        }

    auto Runtime::Impl::Calls(const kb::Description& description, const Inputs& inputs, std::vector<kb::Value>& constructor,
                                              const std::function<bool(std::string_view)>& handledElsewhere) -> Expected<std::vector<kb::Call>>
    {
            std::set<std::string> used;
            for (const auto& field : description.constructor) {
                const auto it = inputs.find(std::string(field.name));
                auto value = Convert(*field.type, it == inputs.end() ? Resolved {} : it->second);
                if (!value) return Fail(std::format("'{}' {}", field.name, value.error()));
                if (it != inputs.end()) used.insert(it->first);
                constructor.push_back(std::move(*value));
            }
            std::vector<kb::Call> calls;
            // What an argument not given is: what its input starts as, when its type says (a depth attachment clears to
            // 1, a third dimension is 1), or nothing — the builder's own zero.
            const NodeType* type = FindType(description.name);
            const auto initial = [&](const std::string& name) -> const Value* {
                if (!type) return nullptr;
                std::string listed = name;
                if (const auto open = listed.find('['); open != std::string::npos)
                    if (const auto close = listed.find(']', open); close != std::string::npos) listed.erase(open + 1, close - open - 1);
                const auto port = std::ranges::find(type->inputs, listed, &Port::name);
                return port == type->inputs.end() || port->initial.IsNull() || port->kind != PortKind::eValue ? nullptr : &port->initial;
            };
            for (const auto& setting : description.settings) {
                // How many times it is given: once, or — for one given more than once — up to the highest index named.
                int count = setting.repeatable ? 0 : 1;
                if (setting.repeatable)
                    for (const auto& name : inputs | std::views::keys)
                        if (name.starts_with(std::string(setting.name) + "["))
                            count = std::max(count, std::atoi(name.c_str() + setting.name.size() + 1) + 1);
                for (int index = 0; index < count; ++index) {
                    kb::Call call { &setting, {} };
                    bool given = false;
                    std::vector<std::size_t> open;     // the arguments not given, filled from what they start as once the call is made
                    for (const auto& argument : setting.arguments) {
                        const std::string name = InputName(setting, argument, setting.repeatable ? index : -1);
                        const auto it = inputs.find(name);
                        if (it == inputs.end()) { open.push_back(call.arguments.size()); call.arguments.emplace_back(); continue; }
                        auto value = Convert(*argument.type, it->second);
                        if (!value) return Fail(std::format("'{}' {}", name, value.error()));
                        used.insert(name);
                        given = true;
                        call.arguments.push_back(std::move(*value));
                    }
                    if (!given) continue;
                    for (const std::size_t at : open) {
                        const auto& argument = setting.arguments[at];
                        const std::string name = InputName(setting, argument, setting.repeatable ? index : -1);
                        if (const Value* start = initial(name))
                            if (auto value = ConvertValue(*argument.type, *start)) call.arguments[at] = std::move(*value);
                    }
                    calls.push_back(std::move(call));
                }
            }
            for (const auto& name : inputs | std::views::keys)
                if (!used.contains(name) && !handledElsewhere(name)) return Fail(std::format("a {} has no input '{}'", description.name, name));
            return calls;
        }

    auto Runtime::Impl::BuildWith(const kb::Description& description, const std::string& nodeId, const std::vector<kb::Value>& constructor,
                                                      const std::vector<kb::Call>& calls) -> Expected<std::shared_ptr<Resource>>
    {
            const auto made = kb::Build(description, constructor, calls);
            if (!made) return Fail(made.error().message);
            auto resource = std::make_shared<Resource>();
            resource->handle = *made;
            resource->kind = std::string(description.result);
            resource->node = nodeId;
            // The framework keeps a shader by its module and entry point, one that failed included: asked for again
            // it hands back the same failure, though the file may be there now, or mended. It is tried afresh.
            if (!koral_resource_valid(*made) && description.name == "Shader") koral_resource_retry(*made);
            if (!koral_resource_valid(*made)) {
                if (description.name == "Shader") shaderFailed = true;
                const char* why = koral_resource_error_message(*made);
                return Fail(why && *why ? std::string(why) : std::format("the {} could not be built", description.name));
            }
            return resource;
        }

    auto Runtime::Impl::PipelineFor(const PipelineInfo& info, const kor::Mesh& mesh) -> Expected<kor::ResourceRef<const kor::GraphicsPipeline>>
    {
            const auto own = kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(info.resource->handle));
            const std::string fits = Fits(mesh, *own, info.vertexShader);
            if (fits.empty()) return own;
            if (!info.vertexShader.Get()) return Fail(fits);
            const auto has = mesh.Layout().Resolve(*info.vertexShader);
            if (!has) return Fail(std::format("its mesh has not what the vertex shader reads: {}", has.error().message));
            auto& variants = *info.variants;
            if (!variants.description || !variants.layoutSetting) return Fail(fits);
            const std::string key = LayoutSignature(mesh.Layout());
            if (const auto made = variants.made.find(key); made != variants.made.end())
                return kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(made->second->handle));
            if (const auto failed = variants.failed.find(key); failed != variants.failed.end()) return Fail(failed->second);
            // The node's calls, with its mesh's layout where the node's was.
            const auto converted = ConvertValue(*variants.layoutSetting->arguments[variants.layoutArgument].type, LayoutValueOf(mesh.Layout()));
            Expected<Held> made = converted ? Expected<Held>(Fail("")) : Expected<Held>(Fail(converted.error()));
            if (converted) {
                auto calls = variants.calls;
                const auto call = std::ranges::find(calls, variants.layoutSetting, &kb::Call::setting);
                if (call == calls.end()) made = Fail("the pipeline names no vertex shader to make it again with");
                else {
                    if (call->arguments.size() <= variants.layoutArgument) call->arguments.resize(variants.layoutArgument + 1);
                    call->arguments[variants.layoutArgument] = *converted;
                    made = BuildWith(*variants.description, variants.node, variants.constructor, calls);
                }
            }
            if (!made) {
                const std::string why = std::format("its mesh's vertex layout needs the pipeline made for it, and it could not be: {}", made.error());
                variants.failed[key] = why;
                return Fail(why);
            }
            const auto pipeline = kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf((*made)->handle));
            variants.made[key] = std::move(*made);
            return pipeline;
        }

    auto Runtime::Impl::FieldBytes(const kecs::FieldDescription& field, const Value& value, std::byte* at) -> std::string
    {
            std::vector<double> numbers;
            if (const double* number = value.Number()) numbers.push_back(*number);
            else if (const bool* flag = value.Bool()) numbers.push_back(*flag ? 1.0 : 0.0);
            else if (const auto* items = value.Items())
                for (const auto& item : *items) {
                    if (const double* number = item.Number()) numbers.push_back(*number);
                    else return "a list of numbers is what it takes";
                }
            else return "a number, true or false, or a vector of numbers is what it takes";
            const std::string& type = field.type;
            char scalar = 'f';
            std::size_t width = 4, count = 1;
            if (type == "f64") { scalar = 'f'; width = 8; }
            else if (type == "bool") { scalar = 'b'; width = 1; }
            else if (type == "i8" || type == "i16" || type == "i32" || type == "i64") { scalar = 'i'; width = static_cast<std::size_t>(std::stoi(type.substr(1))) / 8; }
            else if (type == "u8" || type == "u16" || type == "u32" || type == "u64" || type == "bool32") { scalar = 'u'; width = type == "bool32" ? 4 : static_cast<std::size_t>(std::stoi(type.substr(1))) / 8; }
            else if (type == "mat4") count = 16;
            else if (type == "quat") count = 4;
            else if (type.size() >= 4 && type.back() >= '2' && type.back() <= '4' && (type.starts_with("vec") || type.starts_with("ivec") || type.starts_with("uvec"))) {
                count = static_cast<std::size_t>(type.back() - '0');
                scalar = type[0] == 'i' ? 'i' : type[0] == 'u' ? 'u' : 'f';
            } else if (type != "f32") return std::format("a {} is no number or vector, and cannot be given here", type);
            if (numbers.size() > count) return std::format("it is {} {}, and was given {}", count, count == 1 ? "number" : "numbers", numbers.size());
            if (numbers.size() == 1 && count > 1) numbers.assign(count, numbers.front());     // one for every component
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                std::byte* to = at + i * width;
                const double number = numbers[i];
                if (scalar == 'b') *to = std::byte { number != 0.0 ? std::uint8_t { 1 } : std::uint8_t { 0 } };
                else if (scalar == 'f' && width == 8) std::memcpy(to, &number, 8);
                else if (scalar == 'f') { const auto v = static_cast<float>(number); std::memcpy(to, &v, 4); }
                else { const auto whole = static_cast<long long>(std::llround(number)); std::memcpy(to, &whole, width); }   // little-endian: the low bytes
            }
            return {};
        }

    auto Runtime::Impl::BuildScript(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto named = inputs.find("script");
            const std::string* name = named == inputs.end() ? nullptr : named->second.value.String();
            if (!name || name->empty()) return Fail("'script' is the class of one of the project's ResourceScripts");
            const Scripted* found = ScriptNamed(*name);
            if (!found) {
                std::string there;
                for (const auto& each : source ? source->scripts : scripts) there += std::format("{}{}", there.empty() ? "" : ", ", each.first);
                return Fail(std::format("there is no ResourceScript '{}'{}", *name, there.empty() ? std::string(": the project has none") : std::format(": there are {}", there)));
            }
            const ResourceScript& script = found->script;
            std::vector<std::byte> settings(script.size, std::byte { 0 });
            if (script.initial.size() == settings.size()) settings = script.initial;
            std::string has;
            for (const auto& field : script.fields) {
                const bool matrix = field.type == "mat4";
                const bool single = !matrix && field.type != "quat" && !field.type.starts_with("vec") && !field.type.starts_with("ivec") && !field.type.starts_with("uvec");
                Port port { "field." + field.name, PortKind::eValue, field.type == "bool" ? "bool" : single ? "number" : matrix ? "matrix" : "vector", false,
                            std::format("{}.{}: a {}.", script.name, field.name, field.type), nullptr };
                port.optional = true;
                port.place = matrix ? Placement::eWire : Placement::eNode;
                port.min = field.min;       // as the script says: its [Range], its [Min], its [Logarithmic]
                port.max = field.max;
                port.logarithmic = field.logarithmic;
                // A whole number's dragged a unit at a time; an unsigned one's none or more.
                port.integer = field.type.starts_with("i") || field.type.starts_with("u");
                if (field.type.starts_with("u")) port.min = std::max(port.min, 0.f);
                out.ports.push_back(std::move(port));
                has += std::format("{}{}", has.empty() ? "" : ", ", field.name);
            }
            // Each resource's inputs, as its kind has them, starting as the source says; and it, an output.
            std::string makes;
            for (const auto& resource : script.resources) {
                const NodeType* kind = FindType(resource.kind);
                if (!kind || !kind->builder) return Fail(std::format("{}.{} is a {}, which is no resource a script can make", script.name, resource.name, resource.kind));
                const auto declared = [&](const std::string& input) -> const Value* {
                    for (const auto& [setting, value] : resource.settings) if (setting == input) return &value;
                    return nullptr;
                };
                for (const auto& each : kind->inputs) {
                    Port port = each;
                    port.name = resource.name + "." + each.name;
                    port.optional = true;
                    port.companion.clear();
                    port.group = resource.name;     // each of what it makes, a part of its own
                    port.when = {};
                    if (const Value* given = declared(each.name)) port.initial = *given;
                    else if (const auto dot = each.name.rfind('.'); dot != std::string::npos && each.name.size() == dot + 2)
                        if (const Value* whole = declared(each.name.substr(0, dot)); whole && whole->Items())
                            if (const std::size_t at = std::string_view("xyzw").find(each.name.back()); at < whole->Items()->size()) port.initial = (*whole->Items())[at];
                    if (each.kind == PortKind::eResource)
                        port.doc = std::format("{} — wired, or another of {}'s resources, as its source names it.", each.doc, script.name);
                    out.ports.push_back(std::move(port));
                }
                Port output { resource.name, PortKind::eResource, std::string(kind->builder->result), false,
                              std::format("{}.{}: a {}, as the script makes it.", script.name, resource.name, resource.kind), nullptr };
                out.gives.push_back(std::move(output));
                makes += std::format("{}{}", makes.empty() ? "" : ", ", resource.name);
            }
            for (const auto& [input, given] : inputs) {
                if (input == "script") continue;
                if (input.starts_with("field.")) {
                    const kecs::FieldDescription* field = nullptr;
                    for (const auto& each : script.fields) if (each.name == std::string_view(input).substr(6)) field = &each;
                    if (!field) return Fail(std::format("{} has no field '{}': it has {}", script.name, input.substr(6), has.empty() ? std::string("none") : has));
                    if (given.value.IsNull()) continue;
                    if (field->offset + field->size > settings.size()) continue;
                    if (const std::string why = FieldBytes(*field, given.value, settings.data() + field->offset); !why.empty())
                        return Fail(std::format("'{}': {}", input, why));
                    continue;
                }
                const std::string_view resource = std::string_view(input).substr(0, input.find('.'));
                if (input.find('.') == std::string::npos || !std::ranges::contains(script.resources, resource, &ScriptResource::name))
                    return Fail(std::format("{} makes no '{}': it makes {}", script.name, resource, makes.empty() ? std::string("nothing") : makes));
            }

            // What each is made of: what its source says, and what its kind starts with — then what the node says.
            std::map<std::string, Held, std::less<>> made;
            const auto of = [&](const ScriptResource& resource) -> Expected<Inputs> {
                const NodeType* kind = FindType(resource.kind);
                Node as;
                as.id = node.id;
                as.type = resource.kind;
                Inputs base, given;
                for (const auto& port : kind->inputs)
                    if (!port.optional && port.kind == PortKind::eValue && !port.initial.IsNull()) base[port.name].value = port.initial;
                for (const auto& [setting, value] : resource.settings) {
                    const auto port = std::ranges::find(kind->inputs, Listed(setting), &Port::name);
                    Resolved resolved;
                    if (port != kind->inputs.end() && port->kind == PortKind::eResource) {
                        // Another of the script's, by name.
                        const std::string* other = value.String();
                        if (!other || !made.contains(*other))
                            return Fail(std::format("{}.{}'s '{}' names {}, which is none of the script's resources made before it", script.name, resource.name, setting,
                                                    other ? std::format("'{}'", *other) : value.ToJson()));
                        resolved.resource = made.find(*other)->second;
                        resolved.linked = true;
                    } else {
                        resolved.value = value;
                    }
                    // What the source gives whole is the parts the kind starts with given otherwise.
                    for (const auto& part : PartsOf(*kind, setting)) base.erase(part);
                    base[setting] = std::move(resolved);
                }
                for (const auto& [input, value] : inputs)
                    if (input.size() > resource.name.size() + 1 && input.starts_with(resource.name) && input[resource.name.size()] == '.')
                        given[input.substr(resource.name.size() + 1)] = value;
                if (const auto parts = InParts(as, base); !parts) return Fail(std::format("{}.{}: {}", script.name, resource.name, parts.error()));
                if (const auto parts = InParts(as, given); !parts) return Fail(std::format("{}.{}: {}", script.name, resource.name, parts.error()));
                for (auto& [input, value] : given) base[input] = std::move(value);
                // Left open for a wire, and none in it yet: not given.
                std::erase_if(base, [](const auto& each) { return each.second.value.IsNull() && !each.second.resource; });
                FillDefaults(as, base);
                return base;
            };
            const auto build = [&](const ScriptResource& resource, const Inputs& with, const std::vector<std::byte>* filled) -> Expected<Held> {
                const kb::Description& description = *FindType(resource.kind)->builder;
                std::vector<kb::Value> constructor;
                auto calls = Calls(description, with, constructor, [](std::string_view) { return false; });
                if (!calls) return Fail(std::format("{}.{}: {}", script.name, resource.name, calls.error()));
                if (filled) {
                    const auto data = std::ranges::find_if(description.settings, [](const kb::Setting& setting) { return setting.name == "data"; });
                    if (data == description.settings.end()) return Fail(std::format("a {} cannot be given what it holds", description.name));
                    calls->push_back({ &*data, { kb::Value(*filled) } });
                }
                auto built = BuildWith(description, node.id, constructor, *calls);
                if (!built) return Fail(std::format("{}.{}: {}", script.name, resource.name, built.error()));
                (*built)->label = resource.name;
                // A view is of something: kept, for a pass that uses the view to say it uses that.
                if (description.name == "ImageView") if (const auto it = with.find("image"); it != with.end()) (*built)->source = it->second.resource;
                if (description.name == "BufferView") if (const auto it = with.find("buffer"); it != with.end()) (*built)->source = it->second.resource;
                return built;
            };
            const auto sibling = [&](const ScriptResource& resource) {
                const NodeType* kind = FindType(resource.kind);
                return std::ranges::any_of(resource.settings, [&](const auto& setting) {
                    const auto port = std::ranges::find(kind->inputs, Listed(setting.first), &Port::name);
                    return port != kind->inputs.end() && port->kind == PortKind::eResource;
                });
            };

            // Those of nothing else of the script's, first: what it fills.
            struct First { const ScriptResource* resource; Inputs with; };
            std::vector<First> first;
            for (const auto& resource : script.resources) {
                if (sibling(resource)) continue;
                auto with = of(resource);
                if (!with) return Fail(with.error());
                auto built = build(resource, *with, nullptr);
                if (!built) return Fail(built.error());
                made[resource.name] = std::move(*built);
                first.push_back({ &resource, std::move(*with) });
            }
            if (!script.fill) {
                if (!script.resources.empty())
                    out.status.note = std::format("{} fills what it makes when the project runs with its scripts: until then it is made empty", script.name);
            } else {
                std::vector<Contents> contents;
                std::vector<std::vector<std::byte>> bytes;
                std::vector<const First*> filled;
                contents.reserve(first.size());
                bytes.reserve(first.size());
                for (const auto& each : first) {
                    const std::string& kind = each.resource->kind;
                    if (kind != "Image" && kind != "Buffer") continue;
                    Contents one;
                    one.name = each.resource->name;
                    one.node = node.id;
                    one.kind = kind;
                    std::size_t size = 0;
                    const Held& held = made[one.name];
                    if (kind == "Image") {
                        const auto image = ImageRefOf(held.get());
                        if (image->IsPerFrame()) return Fail(std::format("{}.{} is filled once, and a per-frame image is a copy for each frame in flight: it cannot be per frame", script.name, one.name));
                        one.format = FormatName(image->PixelFormat());
                        const TexelLayout texel = TexelLayoutOf(one.format);
                        if (texel.channels == 0) return Fail(std::format("{}.{} is of {}, which a script cannot fill: a texel of it is not plain numbers", script.name, one.name, one.format));
                        one.extent = image->Extent();
                        one.layers = std::max<std::uint32_t>(image->ArrayLayers(), 1);
                        one.texelBytes = static_cast<std::uint32_t>(texel.Bytes());
                        size = static_cast<std::size_t>(one.extent.x) * one.extent.y * one.extent.z * one.layers * one.texelBytes;
                    } else {
                        const auto buffer = BufferRefOf(held.get());
                        if (buffer->IsPerFrame()) return Fail(std::format("{}.{} is filled once, and a per-frame buffer is a copy for each frame in flight: it cannot be per frame", script.name, one.name));
                        size = static_cast<std::size_t>(buffer->size());
                    }
                    bytes.emplace_back(size, std::byte { 0 });
                    one.bytes = bytes.back();
                    contents.push_back(std::move(one));
                    filled.push_back(&each);
                }
                if (const std::string why = script.fill(contents, settings); !why.empty()) return Fail(std::format("{}: {}", script.name, why));
                // Made again, the same, with what was written.
                for (std::size_t i = 0; i < filled.size(); ++i) {
                    auto rebuilt = build(*filled[i]->resource, filled[i]->with, &bytes[i]);
                    if (!rebuilt) return Fail(rebuilt.error());
                    made[filled[i]->resource->name] = std::move(*rebuilt);
                }
            }
            // Then what is made of them, each after what it names.
            std::vector<const ScriptResource*> rest;
            for (const auto& resource : script.resources) if (sibling(resource)) rest.push_back(&resource);
            while (!rest.empty()) {
                const auto ready = std::ranges::find_if(rest, [&](const ScriptResource* resource) {
                    return std::ranges::all_of(resource->settings, [&](const auto& setting) {
                        const auto port = std::ranges::find(FindType(resource->kind)->inputs, Listed(setting.first), &Port::name);
                        const std::string* other = setting.second.String();
                        return port == FindType(resource->kind)->inputs.end() || port->kind != PortKind::eResource || !other
                            || made.contains(*other) || !std::ranges::contains(rest, *other, &ScriptResource::name);
                    });
                });
                if (ready == rest.end()) return Fail(std::format("{}'s resources are made of each other, round: {} cannot be made", script.name, rest.front()->name));
                auto with = of(**ready);
                if (!with) return Fail(with.error());
                auto built = build(**ready, *with, nullptr);
                if (!built) return Fail(built.error());
                made[(*ready)->name] = std::move(*built);
                rest.erase(ready);
            }
            for (auto& [resource, held] : made) out.outputs[resource].resource = std::move(held);
            return {};
        }

    auto Runtime::Impl::UsedAs(const Resource& resource, const std::uint32_t usage) -> void
    {
            const Resource* made = &resource;
            while (made->source && (made->kind == "ImageView" || made->kind == "BufferView")) made = made->source.get();
            const auto node = currentById.find(made->node);
            if (node == currentById.end() || !MadeForItsUse(node->second->type) || shared.contains(made->node)) return;
            usedAsBy[made->node][user] |= usage;
            // Made without it: made again with it, and what uses it — at once, while the graph is being built (Evaluate
            // goes round again), and next frame when it is found so as a frame is readied (an entity's).
            if (const auto it = built.find(made->node); !evaluating && it != built.end() && (it->second.madeFor & usage) != usage) stale = true;
        }

    auto Runtime::Impl::MadeForItsUse(const std::string_view type) -> bool
    { return type == "Image" || type == "Buffer"; }

    auto Runtime::Impl::UsagesOf(const std::string_view type) -> const kb::Enumeration*
    {
            const kb::Description* description = kb::Find(type);
            const kb::Setting* setting = description ? description->FindSetting("usage") : nullptr;
            return setting && !setting->arguments.empty() ? setting->arguments.front().type->enumeration : nullptr;
        }

    auto Runtime::Impl::GivenUsage(const std::string_view type, const Inputs& inputs) -> std::uint32_t
    {
            const kb::Enumeration* usages = UsagesOf(type);
            const auto given = inputs.find("usage");
            if (!usages || given == inputs.end() || !given->second.value.Items()) return 0;
            std::uint32_t usage = 0;
            for (const auto& entry : usages->values)
                if (std::ranges::contains(*given->second.value.Items(), Value(std::string(entry.name)))) usage |= static_cast<std::uint32_t>(entry.value);
            return usage;
        }

    auto Runtime::Impl::UsageFor(const std::string_view id) const -> std::uint32_t
    {
            std::uint32_t usage = static_cast<std::uint32_t>(kor::Image::Usage::eTransferSrc);
            static_assert(static_cast<std::uint32_t>(kor::Image::Usage::eTransferSrc) == static_cast<std::uint32_t>(kor::Buffer::Usage::eTransferSrc));
            if (const auto it = usedAsBy.find(id); it != usedAsBy.end())
                for (const std::uint32_t by : it->second | std::views::values) usage |= by;
            return usage;
        }

    auto Runtime::Impl::BuildBuilder(const Node& node, const kb::Description& description, const Inputs& given, Built& out) -> Expected<void>
    {
            // A Buffer given no `type` is where it is best kept: on the device, which only passes reach — unless the CPU
            // reaches it too (a script was handed it: ReachedFromTheCpu), when it is memory the CPU writes into.
            Inputs automatic;
            const bool chosen = description.name == "Buffer" && (!given.contains("type") || given.at("type").value.IsNull());
            if (chosen) {
                automatic = given;
                automatic["type"].value = Value(reachedFromTheCpu.contains(node.id) ? "eDynamic" : "eDeviceLocal");
            }
            // An Image or a Buffer is made for what it is used as (UsedAs), and for what its `usage` adds.
            const kb::Enumeration* usages = MadeForItsUse(description.name) ? UsagesOf(description.name) : nullptr;
            const auto givenUsage = given.find("usage");
            const bool listed = givenUsage == given.end() || givenUsage->second.value.IsNull() || givenUsage->second.value.Items();
            const bool madeFor = usages && listed;
            if (madeFor) {
                if (!chosen) automatic = given;
                Value::List names;
                if (givenUsage != given.end() && givenUsage->second.value.Items()) names = *givenUsage->second.value.Items();
                out.givenUsage = GivenUsage(description.name, given);
                out.madeFor = out.givenUsage | UsageFor(node.id);
                for (const auto& entry : usages->values)
                    if (entry.value != 0 && (out.madeFor & entry.value) == entry.value && !(out.givenUsage & entry.value)) names.emplace_back(std::string(entry.name));
                automatic["usage"].value = Value(std::move(names));
            }
            const Inputs& inputs = chosen || madeFor ? automatic : given;
            if (chosen) out.automaticType = automatic.at("type").value.String() ? *automatic.at("type").value.String() : std::string();
            std::vector<kb::Value> constructor;
            const auto calls = Calls(description, inputs, constructor, [&](const std::string_view name) { return name == "transient" && MadeForItsUse(description.name); });
            if (!calls) return Fail(calls.error());
            // A framebuffer's images have to have been made to be drawn into.
            if (description.name == "Framebuffer")
                for (const auto& [name, input] : inputs) {
                    const auto image = ImageRefOf(input.resource.get());
                    if (!image.Get()) continue;
                    const bool depth = name.starts_with("depthStencil.");
                    const auto needed = depth ? kor::Image::Usage::eDepthStencilAttachment : kor::Image::Usage::eColorAttachment;
                    if (MadeWithout(*input.resource, needed))
                        return Fail(std::format("'{}' is drawn into: its image needs {} among its usage", name, depth ? "eDepthStencilAttachment" : "eColorAttachment"));
                }
            auto resource = BuildWith(description, node.id, constructor, *calls);
            if (!resource) return Fail(resource.error());
            // A view is of something: kept, for a pass that uses the view to say it uses that.
            if (description.name == "ImageView") if (const auto it = inputs.find("image"); it != inputs.end()) (*resource)->source = it->second.resource;
            if (description.name == "BufferView") if (const auto it = inputs.find("buffer"); it != inputs.end()) (*resource)->source = it->second.resource;
            if (description.name == "Shader") {
                // Told when the file is edited: what was built from it is built again (Update).
                Watch(out, kor::capi::ShaderOf((*resource)->handle));
            }
            if (const auto transient = given.find("transient"); MadeForItsUse(description.name) && transient != given.end() && transient->second.value.Bool() && *transient->second.value.Bool()) {
                if (auto made = TransientOf(**resource); !made) return Fail(made.error());
                else transients[node.id] = std::move(*made);
                (*resource)->transient = true;
            } else {
                transients.erase(node.id);
            }
            out.outputs["out"].resource = std::move(*resource);
            return {};
        }

    auto Runtime::Impl::TransientOf(const Resource& standIn) -> Expected<Transient>
    {
            Transient made;
            if (const auto image = ImageRefOf(&standIn); image.Get()) {
                if (image->ImageType() != kor::Image::Type::e2D || image->ArrayLayers() != 1 || image->Extent().z > 1)
                    return Fail("a transient image is a 2D image of one layer: one of more, or of three dimensions, is the node's own");
                if (image->Samples() != kor::SampleCount::e1) return Fail("a transient image is of one sample: one of more is the node's own");
                made.image = kor::ImageDesc { .format = image->PixelFormat(), .usage = image->UsageFlags(), .extent = kor::UVec2 { image->Extent().x, image->Extent().y },
                                              .mipLevels = image->MipLevels() };
                made.standIn = image.Get();
            } else if (const auto buffer = BufferRefOf(&standIn); buffer.Get()) {
                if (buffer->MemoryType() != kor::Buffer::Type::eDeviceLocal)
                    return Fail("a transient buffer is on the device (eDeviceLocal): one the CPU writes is the node's own");
                made.buffer = kor::BufferDesc { .size = static_cast<kor::i64>(buffer->size()), .usage = buffer->UsageFlags(), .type = kor::Buffer::Type::eDeviceLocal };
                made.standIn = buffer.Get();
            }
            return made;
        }

    auto Runtime::Impl::ShadersOf(const Inputs& inputs, kor::ResourceRef<const kor::Shader>* vertex) -> std::vector<kor::ResourceRef<const kor::Shader>>
    {
            std::vector<kor::ResourceRef<const kor::Shader>> shaders;
            for (const auto& [name, input] : inputs) {
                if (!input.resource || input.resource->kind != "Shader") continue;
                shaders.push_back(kor::capi::ShaderOf(input.resource->handle));
                if (vertex && name == "vertexShader.shader") *vertex = shaders.back();
            }
            return shaders;
        }

    auto Runtime::Impl::AsTheBuilderTakesThem(const Node& node, const kb::Description& description, const Inputs& given, PipelineInfo& info, Built& out) -> Expected<Inputs>
    {
            Inputs inputs = given;
            const std::string stages = description.name == "GraphicsPipeline" ? StagesOf(node) : std::string();
            if (const auto it = inputs.find("stages"); it != inputs.end()) {
                const std::string* chosen = it->second.value.String();
                if (description.name != "GraphicsPipeline") return Fail(std::format("a {} has no input 'stages'", description.name));
                if (!it->second.value.IsNull() && (!chosen || (*chosen != "eVertex" && *chosen != "eMesh"))) return Fail("'stages' is eVertex or eMesh");
                inputs.erase(it);
            }
            const kb::Description* shaderType = kb::Find("Shader");
            const kb::Setting* entryPoint = shaderType ? shaderType->FindSetting("entryPoint") : nullptr;
            for (const auto& setting : description.settings) {
                if (HiddenPipelineSetting(setting.name)) continue;
                const std::string_view mode = StageMode(setting.name);
                // How many times it is given: once, or up to the highest index any of its inputs names.
                int count = 1;
                if (setting.repeatable) {
                    count = 0;
                    for (const auto& argument : setting.arguments)
                        for (const std::string& shown : { PipelineInputName(setting, argument), InputName(setting, argument) }) {
                            const std::string prefix = shown.substr(0, shown.find("[]") + 1);
                            for (const auto& name : given | std::views::keys)
                                if (name.starts_with(prefix)) count = std::max(count, std::atoi(name.c_str() + prefix.size()) + 1);
                        }
                }
                for (int index = 0; index < count; ++index) {
                    for (const auto& argument : setting.arguments) {
                        const std::string shown = PipelineInputName(setting, argument, setting.repeatable ? index : -1);
                        const std::string taken = InputName(setting, argument, setting.repeatable ? index : -1);
                        // One that belongs to the other way of drawing is refused before anything is made of it.
                        if (!mode.empty() && mode != stages)
                            for (const std::string& name : { shown + ".module", shown + ".entry", shown, taken })
                                if (given.contains(name) && (IsShader(argument) || name == shown || name == taken))
                                    return Fail(std::format("'{}' is part of a pipeline whose 'stages' is {}, and this one's is {}", name, mode, stages));
                        if (IsShader(argument)) {
                            const auto module = inputs.find(shown + ".module");
                            const auto entry = inputs.find(shown + ".entry");
                            const std::string* file = module == inputs.end() ? nullptr : module->second.value.String();
                            const std::string* function = entry == inputs.end() ? nullptr : entry->second.value.String();
                            if (module != inputs.end() && module->second.linked) return Fail(std::format("'{}.module' is the module's name, and cannot be wired", shown));
                            // A type for its elements: the module is the one generated for it.
                            const auto element = inputs.find(shown + ".element");
                            const std::string* type = element == inputs.end() ? nullptr : element->second.value.String();
                            Expected<std::string> sorting = std::string();
                            if (element != inputs.end() && element->second.linked) return Fail(std::format("'{}.element' is a type's name, and cannot be wired", shown));
                            if (type && !type->empty()) {
                                if (!file || *file != "koral_compare_sort") return Fail(std::format("'{}.element' is the type koral_compare_sort sorts: no other module takes one", shown));
                                sorting = WriteSortModule(*type);
                                if (!sorting) return Fail(std::format("'{}.element': {}", shown, sorting.error()));
                                file = &*sorting;
                            }
                            if (file && !file->empty()) {
                                if (inputs.contains(taken)) return Fail(std::format("'{}' is named and wired both: one or the other", shown));
                                if (!entryPoint) return Fail("the framework describes no Shader builder");
                                // The entry point, when it is not said: the stage's name and Main, as shaders here are written — of
                                // a hit group's, the stage's own: `hitGroup[0].closestHit`'s is closestHitMain, `miss[1]`'s missMain.
                                std::string stage = shown.substr(shown.rfind('.') == std::string::npos ? 0 : shown.rfind('.') + 1);
                                stage = stage.substr(0, stage.find('['));
                                std::string main = function && !function->empty() ? *function : stage + "Main";
                                auto shader = BuildWith(*shaderType, node.id, {}, { { entryPoint, { kb::Value::Of(*file), kb::Value::Of(main) } } });
                                if (!shader) {
                                    // The commonest of them, said as what to do about it.
                                    if (!sorting->empty()) return Fail(std::format("'{}.element' is {}: {}", shown, *type, shader.error()));
                                    if (shader.error().find("cannot open file") != std::string::npos)
                                        return Fail(std::format("'{}': there is no {}.slang among the shaders. Put it in the project's shaders folder (the project has to be "
                                                                "saved for it to have one); it is picked up when it is there.", shown, *file));
                                    return Fail(std::format("'{}': {}", shown, shader.error()));
                                }
                                (*shader)->kind = "Shader";
                                Watch(out, kor::capi::ShaderOf((*shader)->handle));
                                info.named.push_back(*shader);
                                Resolved resolved;
                                resolved.resource = std::move(*shader);
                                resolved.linked = true;
                                inputs[taken] = std::move(resolved);
                            } else if (function && !function->empty()) {
                                return Fail(std::format("'{}.entry' is an entry point of a module: '{}.module' says which", shown, shown));
                            }
                            inputs.erase(shown + ".module");
                            inputs.erase(shown + ".entry");
                            inputs.erase(shown + ".element");
                        } else if (shown != taken) {
                            if (const auto it = inputs.find(shown); it != inputs.end()) {
                                inputs[taken] = it->second;
                                inputs.erase(shown);
                            }
                        }
                    }
                }
            }
            return inputs;
        }

    auto Runtime::Impl::BuildPipeline(const Node& node, const kb::Description& description, const Inputs& given, Built& out) -> Expected<void>
    {
            auto info = std::make_shared<PipelineInfo>();
            const auto translated = AsTheBuilderTakesThem(node, description, given, *info, out);
            if (!translated) return Fail(translated.error());
            const Inputs& inputs = *translated;
            info->shaders = ShadersOf(inputs, &info->vertexShader);
            const Interface interface = Reflect(info->shaders, Kinds(), world);
            info->ports = interface.ports;
            info->meshShaders = node.type == "GraphicsPipeline" && StagesOf(node) == "eMesh";
            info->vertexInputs = info->vertexShader.Get() && !info->vertexShader->BlockLayout().inputs.empty();
            out.pipeline = info;    // its inputs are known from here, built or not

            for (const auto& parameter : interface.parameters)
                if (!parameter.problem.empty()) return Fail(parameter.problem);
            const auto ours = [&](const std::string_view name) { return interface.Find(name) != nullptr; };
            for (const auto& name : inputs | std::views::keys)
                if (name.starts_with("push."))
                    return Fail(std::format("'{}' is a draw's now: give it on the Draw or the Dispatch that runs the pipeline, where what each draw has of its own is", name));
            for (const auto& name : inputs | std::views::keys)
                if ((name.starts_with("param.") || name.starts_with("constant.")) && !ours(name)) {
                    std::string known;
                    for (const auto& port : info->ports) known += std::format("{}{}", known.empty() ? "" : ", ", port.name);
                    return Fail(std::format("its shaders have no '{}'. They have: {}", name, known.empty() ? "nothing" : known));
                }

            std::vector<kb::Value> constructor;
            auto calls = Calls(description, inputs, constructor, ours);
            if (!calls) return Fail(calls.error());

            // The split, as bindings.
            const kb::Setting* binding = description.FindSetting("binding");
            const kb::Setting* constantSetting = description.FindSetting("specializationConstantNamed");
            std::uint64_t wired = 0, unwired = 0;
            std::vector<const Parameter*> bound, loose;
            for (const auto& parameter : interface.parameters) {
                const auto it = inputs.find("param." + parameter.name);
                const bool given = it != inputs.end() && it->second.resource;
                calls->push_back({ binding, { kb::Value::Of(parameter.name), kb::Value::Of(given ? std::uint64_t { 0 } : std::uint64_t { 1 }),
                                              kb::Value::Of(given ? wired++ : unwired++) } });
                if (given) { bound.push_back(&parameter); info->wired[parameter.name] = it->second.resource; }
                else { loose.push_back(&parameter); info->perEntity.push_back(parameter.name); }
            }
            for (const auto& constant : interface.constants) {
                const auto it = inputs.find("constant." + constant.name);
                // What a material shows of itself, as the view asks — unless the graph says otherwise.
                const bool shown = constant.name == ShaderDebugConstant && shaderDebug != 0 && (it == inputs.end() || it->second.value.IsNull());
                if (!shown && (it == inputs.end() || it->second.value.IsNull())) continue;
                const Value value = shown ? Value(shaderDebug) : it->second.value;
                std::vector<std::byte> bytes(constant.size);
                const double* number = value.Number();
                const bool* flag = value.Bool();
                if (!number && !flag) return Fail(std::format("'constant.{}' is a number, or true or false", constant.name));
                const double n = number ? *number : (*flag ? 1.0 : 0.0);
                if (constant.scalar == 0 && constant.size == 4) { const auto v = static_cast<float>(n); std::memcpy(bytes.data(), &v, 4); }
                else if (constant.scalar == 4 || (constant.scalar == 0 && constant.size == 8)) { std::memcpy(bytes.data(), &n, 8); }
                else if (constant.size == 8) { const auto v = static_cast<std::int64_t>(std::llround(n)); std::memcpy(bytes.data(), &v, 8); }
                else { const auto v = static_cast<std::int32_t>(std::llround(n)); std::memcpy(bytes.data(), &v, 4); }
                calls->push_back({ constantSetting, { kb::Value::Of(constant.name), kb::Value::Of(std::move(bytes)) } });
            }

            auto resource = BuildWith(description, node.id, constructor, *calls);
            if (!resource) return Fail(resource.error());
            KoralResource* handle = (*resource)->handle;
            info->pipeline =
                description.name == "ComputePipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::ComputePipelineOf(handle))
                : description.name == "GraphicsPipeline" ? kor::ResourceRef<const kor::Pipeline>(kor::capi::GraphicsPipelineOf(handle))
                : kor::ResourceRef<const kor::Pipeline>(kor::capi::RayTracingPipelineOf(handle));
            if (const auto it = inputs.find("framebuffer"); it != inputs.end()) info->framebuffer = it->second.resource;

            const auto samplerOf = [&](const Parameter& parameter) -> Held {
                const auto it = inputs.find("param." + parameter.name + ".sampler");
                return it == inputs.end() ? nullptr : it->second.resource;
            };

            // What is wired, bound once.
            if (!bound.empty()) {
                kor::DescriptorSet::Builder set(info->pipeline, 0);
                for (const Parameter* parameter : bound) {
                    const std::string port = "param." + parameter->name;
                    const Held& given = inputs.at(port).resource;
                    const auto written = WriteParameter(set, parameter->name, parameter->type, *given, samplerOf(*parameter).get(), port);
                    if (!written) return Fail(written.error());
                    // What a pass running the pipeline reads and writes, for the frame graph to order it by.
                    if (Member member = MemberOf(given.get()); member.id) info->uses.push_back({ std::move(member), parameter->Reads(), parameter->Writes() });
                }
                info->set = set.Build();
                if (!info->set.Valid())
                    return Fail(info->set.Failure() ? info->set.Failure()->message : std::string("its descriptor set could not be built"));
                info->hasSet = true;
            }

            // What is not: each draw's to give — a Draw's or a Dispatch's `param.<name>` (set 1) and `push.<name>`.
            for (const Parameter* parameter : loose) {
                info->slots.push_back({ parameter->name, parameter->name, parameter->type, false, parameter->Reads(), parameter->Writes(),
                                        parameter->instancing ? std::make_shared<Instancing>(*parameter->instancing) : nullptr });
                if (parameter->type != kor::DescriptorType::eCombinedImageSampler) continue;
                // Its sampler: the one the pipeline's node wires, for every draw; or each draw's own.
                if (Held sampler = samplerOf(*parameter)) info->samplers[parameter->name] = std::move(sampler);
                else info->slots.push_back({ parameter->name + ".sampler", parameter->name, kor::DescriptorType::eSampler, true, true, false });
            }
            const auto& constants = info->pipeline->PushConstants();
            if (!constants.empty()) {
                kor::u32 first = ~kor::u32 { 0 }, end = 0;
                for (const auto& member : constants | std::views::values) {
                    first = std::min(first, member.offset);
                    end = std::max(end, member.offset + member.size);
                }
                info->pushFirst = first;
                info->pushSize = end - first;
                for (const auto& [name, member] : constants) {
                    if (member.aggregate || member.count != 1) continue;    // its fields and elements are listed on their own
                    info->pushes.push_back({ name, member.offset - first, member.size, member.scalar, member.rows, member.columns, {} });
                }
                std::ranges::sort(info->pushes, {}, &PushMember::offset);
            }
            for (const Slot& slot : info->slots)
                info->drawPorts.push_back({ "param." + slot.field, PortKind::eResource, KindOf(slot.type), false,
                                            slot.sampler ? "The sampler its picture is read with." : "What the pipeline's node leaves unwired: each draw's own.", nullptr });
            for (PushMember& push : info->pushes) {
                Port port { "push." + push.name, PortKind::eValue, push.rows * push.columns == 16 ? "matrix" : push.rows * push.columns == 1 ? (push.scalar == 3 ? "bool" : "number") : "vector",
                            false, "A push constant: each draw's own. Its default in the shader when not given, or zero.", nullptr };
                port.optional = true;
                if (push.columns == 1) port.shape = ShapeOfScalar(push.scalar, push.rows);
                port.integer = port.shape.Whole();
                if (port.shape.scalar == Scalar::eUInt) port.min = 0.f;
                if (push.rows >= 3 && push.columns == 1 && push.scalar == 0 && NamesColor(push.name)) port.editor = "color";
                if (const auto* member = info->pipeline->FindPushConstant(push.name)) Annotate(port, member->annotation);
                push.initial = port.initial;
                info->drawPorts.push_back(std::move(port));
            }

            // What its draws take from nobody else is a component of its own, for an entity to hold: a ForEach of it gives
            // each as `field.<name>`, to be wired back to the draw.
            const std::set<std::string> taken = TakenElsewhere(node.id);
            auto layout = std::make_shared<EntityLayout>();
            // A buffer of each instance's is the draw's to make: what is in the component is each instance's own fields.
            for (const Slot& slot : info->slots)
                if (!taken.contains("param." + slot.field) && !slot.instancing) layout->slots.push_back(slot);
            // A picture an instance reads by its bindless index is held by the entity, as a parameter's picture would be; so
            // is an image it writes.
            for (const Slot& slot : info->slots) {
                if (!slot.instancing || taken.contains("param." + slot.field)) continue;
                for (auto& field : slot.instancing->fields) {
                    if (!field.Pictured() || field.Material()) continue;    // a material's is the material's to hold
                    if (std::ranges::find(layout->slots, field.name, &Slot::field) != layout->slots.end())
                        return Fail(std::format("'{}' is a picture of '{}' and of something else its draws take: the entity's own can be only one of them", field.name, slot.parameter));
                    field.at = layout->slots.size() * sizeof(Held);
                    const bool written = field.from == Instancing::Field::From::eStorage;
                    layout->slots.push_back({ field.name, field.name, written ? kor::DescriptorType::eStorageImage : kor::DescriptorType::eSampledImage, false, true, written });
                }
            }
            for (std::size_t i = 0; i < layout->slots.size(); ++i)
                layout->fields.push_back({ layout->slots[i].field, KindOf(layout->slots[i].type), i * sizeof(Held), sizeof(Held), &AssignHeld });
            const auto aligned = [](const std::size_t bytes) { return (bytes + 15) / 16 * 16; };
            layout->pushFirst = info->pushFirst;
            layout->pushAt = aligned(layout->slots.size() * sizeof(Held));
            std::size_t pushEnd = 0;
            for (const auto& [name, member] : constants) {
                if (member.aggregate || member.count != 1 || taken.contains("push." + name)) continue;
                const auto [type, size] = ConstantType(member);
                layout->fields.push_back({ name, type, layout->pushAt + member.offset - info->pushFirst, size, nullptr });
                pushEnd = std::max<std::size_t>(pushEnd, member.offset - info->pushFirst + size);
            }
            std::size_t end = layout->pushAt + pushEnd;
            for (const Slot& slot : info->slots) {
                if (!slot.instancing || taken.contains("param." + slot.field)) continue;
                for (auto& field : slot.instancing->fields) {
                    if (field.from != Instancing::Field::From::eComponent) continue;
                    if (std::ranges::find(layout->fields, field.name, &kecs::FieldDescription::name) != layout->fields.end())
                        return Fail(std::format("'{}' is a field of '{}' and of something else its draws take: the entity's own can be only one of them", field.name, slot.parameter));
                    field.at = end;
                    layout->fields.push_back({ field.name, field.type, end, field.size, nullptr });
                    end += field.size;
                }
            }
            layout->size = std::max<std::size_t>(aligned(end), 16);
            // A new entity's push constants start as their shader declares them.
            for (const PushMember& push : info->pushes) {
                if (push.initial.IsNull() || taken.contains("push." + push.name)) continue;
                if (layout->initial.empty()) layout->initial.assign(layout->size, std::byte { 0 });
                PackPush(push, push.initial, layout->initial.data() + layout->pushAt + push.offset);
            }
            if (world && !layout->fields.empty()) {
                info->layout = layout;
                const auto registered = Register(node.id, *info, out);
                if (!registered) return Fail(registered.error());
            }
            info->resource = *resource;
            // What it is made with, to be made again for a mesh of another vertex layout.
            if (description.name == "GraphicsPipeline") {
                auto& variants = *info->variants;
                variants.description = &description;
                variants.node = node.id;
                variants.constructor = constructor;
                variants.calls = *calls;
                for (const auto& setting : description.settings)
                    for (std::size_t a = 0; a < setting.arguments.size(); ++a)
                        if (setting.name == "vertexShader" && setting.arguments[a].name == "layout") { variants.layoutSetting = &setting; variants.layoutArgument = a; }
            }
            out.outputs["out"].resource = std::move(*resource);
            if (node.type == "ComputePipeline" && !info->shaders.empty()) {
                const auto& size = info->shaders.front()->BlockLayout().localSize;
                std::ranges::copy(size, info->numThreads);
                out.outputs["numThreads"].value = Value(Value::List { size[0], size[1], size[2] });
            }
            return {};
        }

    auto Runtime::Impl::Origin(const std::string& id) const -> const Node*
    {
            const auto find = [&](const std::string_view which) -> const Node* {
                if (!current) return nullptr;
                if (currentById.size() == current->nodes.size()) {
                    const auto it = currentById.find(which);
                    return it == currentById.end() ? nullptr : it->second;
                }
                return CurrentNode(which);
            };
            const Node* from = find(id);
            for (int hops = 0; from && from->type == "Reference" && hops < 16; ++hops) {
                const auto target = from->inputs.find("node");
                from = target != from->inputs.end() && target->second.value.String() ? find(*target->second.value.String()) : nullptr;
            }
            return from;
        }

    auto Runtime::Impl::TakenElsewhere(const std::string& id) const -> std::set<std::string>
    {
            if (!current) return {};
            // Every pipeline's at once, the first time one is asked of this graph.
            if (!takenByPipeline) {
                takenByPipeline.emplace();
                for (const Node& node : current->nodes) {
                    if (node.type != "Draw" && node.type != "Dispatch" && node.type != "TraceRays") continue;
                    const auto pipeline = node.inputs.find("pipeline");
                    if (pipeline == node.inputs.end() || !pipeline->second.link) continue;
                    if (const Node* of = Origin(pipeline->second.link->node)) TakenBy(node, of->id, (*takenByPipeline)[of->id]);
                }
            }
            const auto it = takenByPipeline->find(id);
            return it == takenByPipeline->end() ? std::set<std::string>() : it->second;
        }

    auto Runtime::Impl::TakenBy(const Node& node, const std::string& id, std::set<std::string>& taken) const -> void
    {
            {
                for (const auto& [name, input] : node.inputs) {
                    if (!name.starts_with("param.") && !name.starts_with("push.")) continue;
                    const std::string field = name.substr(name.find('.') + 1);
                    if (!input.link) { if (!input.value.IsNull()) taken.insert(name); continue; }
                    const Node* from = Origin(input.link->node);
                    const auto component = from ? from->inputs.find("component") : node.inputs.end();
                    const bool own = from && IsEach(from->type) && component != from->inputs.end() && component->second.value.String()
                                     && *component->second.value.String() == id && input.link->output == "field." + field;
                    const bool beside = from && IsEach(from->type) && input.link->output == id + "." + field;
                    if (!own && !beside) taken.insert(name);
                }
            }
        }

    auto Runtime::Impl::Register(const std::string& name, PipelineInfo& info, Built& out) -> Expected<void>
    {
            const EntityLayout& layout = *info.layout;
            kecs::ComponentDescription description;
            description.name = name;
            description.size = layout.size;
            description.alignment = 16;
            description.construct = &ConstructEntity;
            description.copy = &CopyEntity;
            description.relocate = &RelocateEntity;
            description.destroy = &DestroyEntity;
            description.fields = layout.fields;
            description.context = info.layout;

            const kecs::ComponentId existing = world->Find(name);
            if (existing == kecs::NoComponent) {
                info.component = world->Register(std::move(description));
                return {};
            }
            const kecs::ComponentDescription& now = world->Describe(existing);
            if (now.construct != &ConstructEntity)
                return Fail(std::format("its component would be named '{}' after it, and there is one so named already: the node needs another id", name));
            const bool same = now.size == layout.size && std::ranges::equal(now.fields, layout.fields, [](const kecs::FieldDescription& a, const kecs::FieldDescription& b) {
                return a.SameAs(b) && a.offset == b.offset;
            });
            if (!same) {
                try { out.migration = world->Reshape(existing, std::move(description)); }
                catch (const std::exception& e) { return Fail(e.what()); }
            }
            info.component = existing;
            return {};
        }

    auto Runtime::Impl::BuildPass(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            auto plan = std::make_shared<PassPlan>();
            const auto image = [&](const char* name) -> Expected<Held> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || !it->second.resource) return Fail(std::format("'{}' needs an image wired in", name));
                if (it->second.resource->screen) return it->second.resource;
                if (!ImageRefOf(it->second.resource.get()).Get()) return Fail(std::format("'{}' takes an Image, and was given a {}", name, it->second.resource->kind));
                return it->second.resource;
            };
            const auto lacks = [](const Resource& resource, const kor::Image::Usage usage) { return MadeWithout(resource, usage); };
            const auto value = [&](const char* name) -> const Value& {
                static const Value none;
                const auto it = inputs.find(name);
                return it == inputs.end() ? none : it->second.value;
            };
            const auto counts = [&](const char* prefix, kor::u32 (&into)[3], const char* what) -> Expected<void> {
                const char* axes[3] { ".x", ".y", ".z" };
                for (int axis = 0; axis < 3; ++axis) {
                    const std::string name = std::string(prefix) + axes[axis];
                    const Value& given = value(name.c_str());
                    if (given.IsNull()) continue;
                    const double* count = given.Number();
                    // Rounded up: what an image's extent divided by a group's size comes to is how many cover it.
                    if (!count || !(*count > 0) || *count > 65535) return Fail(std::format("'{}' is {}, more than none", name, what));
                    into[axis] = static_cast<kor::u32>(std::ceil(*count - 1e-9));
                }
                return {};
            };
            // What a pass is set up with, read one at a time: the first that is wrong is why the node fails.
            std::string wrong;
            const auto mistaken = [&](std::string why) { if (wrong.empty()) wrong = std::move(why); };
            const auto has = [&](const char* name) { return !value(name).IsNull(); };
            const auto num = [&](const char* name, const double otherwise, const double least = 0, const double most = 4294967295.0) -> double {
                const Value& given = value(name);
                if (given.IsNull()) return otherwise;
                const double* number = given.Number();
                if (!number || *number < least || *number > most) { mistaken(std::format("'{}' is a number from {} to {}", name, least, most)); return otherwise; }
                return *number;
            };
            const auto flag = [&](const char* name) -> std::optional<bool> {
                const Value& given = value(name);
                if (given.IsNull()) return std::nullopt;
                if (!given.Bool()) { mistaken(std::format("'{}' is true or false", name)); return std::nullopt; }
                return *given.Bool();
            };
            const auto pick = [&](const char* name, const std::vector<std::string>& of) -> int {   // which of them; -1 when not given
                const Value& given = value(name);
                if (given.IsNull()) return -1;
                const std::string* chosen = given.String();
                const auto at = chosen ? std::ranges::find(of, *chosen) : of.end();
                if (at == of.end()) {
                    std::string all;
                    for (const auto& each : of) all += std::format("{}{}", all.empty() ? "" : ", ", each);
                    mistaken(std::format("'{}' is one of {}", name, all));
                    return -1;
                }
                return static_cast<int>(at - of.begin());
            };
            const auto several = [&](const char* name, const std::size_t count, const char* what) -> std::optional<std::vector<float>> {
                const Value& given = value(name);
                if (given.IsNull()) return std::nullopt;
                const auto* items = given.Items();
                if (!items || items->size() != count || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; })) {
                    mistaken(std::format("'{}' is {} numbers: {}", name, count, what));
                    return std::nullopt;
                }
                std::vector<float> out;
                for (const auto& item : *items) out.push_back(static_cast<float>(*item.Number()));
                return out;
            };
            const auto each3 = [&](const std::string& prefix, const int otherwise) {
                kor::IVec3 out(otherwise);
                const char* axes[3] { ".x", ".y", ".z" };
                for (int axis = 0; axis < 3; ++axis) out[axis] = static_cast<int>(std::llround(num((prefix + axes[axis]).c_str(), otherwise, 0, 65536)));
                return out;
            };
            const auto whole = [&](const char* name, const double otherwise = 0) { return static_cast<kor::u32>(std::llround(num(name, otherwise))); };
            // A buffer wired in; empty when nothing is, and wrong when it has to be there or lacks what it is used as.
            const auto bufferIn = [&](const char* name, const bool needed, const kor::Buffer::Usage usage, const char* usageName, const char* as) -> kor::ResourceRef<const kor::Buffer> {
                const auto it = inputs.find(name);
                const auto ref = it == inputs.end() ? kor::ResourceRef<const kor::Buffer>() : BufferRefOf(it->second.resource.get());
                if (!ref.Get()) {
                    if (needed || (it != inputs.end() && it->second.resource)) mistaken(std::format("'{}' needs a buffer wired in", name));
                    return {};
                }
                if (MadeWithout(*it->second.resource, usage)) mistaken(std::format("the buffer wired to '{}' needs {} among its usage, {}", name, usageName, as));
                return ref;
            };
            const auto bufferUse = [](const kor::ResourceRef<const kor::Buffer>& buffer) {
                Member member;
                member.id = buffer.Get();
                member.buffer = buffer;
                return member;
            };
            // The pipeline a pass runs, and what its node worked out.
            const auto pipelineOf = [&](const char* kind) -> Expected<std::shared_ptr<const PipelineInfo>> {
                const auto it = inputs.find("pipeline");
                if (it == inputs.end() || !it->second.resource || it->second.resource->kind != kind) return Fail(std::format("'pipeline' needs a {} wired in", kind));
                const auto source = built.find(it->second.resource->node);
                if (source == built.end() || !source->second.pipeline) return Fail("'pipeline' is not a pipeline node's output");
                return std::shared_ptr<const PipelineInfo>(source->second.pipeline);
            };
            // What a draw has of its own, and where each comes from: what its pipeline leaves unwired, and its mesh. Wired
            // from a ForEach, it is a draw an element; otherwise, one.
            const auto drawsOf = [&](const std::shared_ptr<const PipelineInfo>& info, const bool draws) -> Expected<std::shared_ptr<EntityRun>> {
                out.ports = info->drawPorts;     // shown, built or not
                if (has("with") || has("without"))
                    return Fail("'with' and 'without' are a ForEach's now: draw from a ForEach of Mesh, `with` what else an entity has to have");
                for (const auto& name : inputs | std::views::keys)
                    if ((name.starts_with("param.") || name.starts_with("push.")) && std::ranges::find(info->drawPorts, name, &Port::name) == info->drawPorts.end()
                        && !FromBundle(node.id, name)) {     // given by a Bundle, by name: let pass, and said on the node
                        std::string known;
                        for (const auto& port : info->drawPorts) known += std::format("{}{}", known.empty() ? "" : ", ", port.name);
                        return Fail(std::format("its pipeline leaves it no '{}': a parameter its node wires is the same for every draw. It leaves: {}", name, known.empty() ? "nothing" : known));
                    }
                auto run = std::make_shared<EntityRun>();
                run->info = info;
                run->draws = draws;
                std::vector<std::string> names { "mesh" };
                for (const Slot& slot : info->slots) names.push_back("param." + slot.field);
                for (const PushMember& push : info->pushes) names.push_back("push." + push.name);
                // The ForEach they follow, when any does: one, as a buffer's fields do.
                std::set<std::string> sources;
                std::map<std::string, bool> seen;
                for (const auto& name : names)
                    if (const auto wired = node.inputs.find(name); wired != node.inputs.end() && wired->second.link)
                        Follows(wired->second.link->node, sources, run->chain, seen);
                if (sources.size() > 1)
                    return Fail(std::format("what it draws with follows more than one ForEach, '{}' and '{}': a draw is one element of one", *sources.begin(), *std::next(sources.begin())));
                const Built* each = nullptr;
                if (!sources.empty()) {
                    run->each = *sources.begin();
                    const auto found = built.find(run->each);
                    if (found == built.end() || !found->second.status.ok || !found->second.listed) return Fail(std::format("'{}' is not built", run->each));
                    each = &found->second;
                }
                const auto classify = [&](const std::string& name) -> Source {
                    Source source;
                    source.input = name;
                    const auto wired = node.inputs.find(name);
                    if (wired == node.inputs.end()) return source;
                    if (!wired->second.link) {
                        if (!wired->second.value.IsNull()) { source.kind = Source::Kind::eGiven; source.value = wired->second.value; }
                        return source;
                    }
                    const auto& link = *wired->second.link;
                    source.node = link.node;
                    source.output = link.output;
                    std::set<std::string> theirs;
                    std::vector<std::string> between;
                    std::map<std::string, bool> looked;
                    if (!each || !Follows(link.node, theirs, between, looked)) {
                        source.kind = Source::Kind::eWired;
                        if (const auto resolved = inputs.find(name); resolved != inputs.end()) source.resource = resolved->second.resource;
                        return source;
                    }
                    if (link.node != run->each) { source.kind = Source::Kind::eChain; run->rows = true; return source; }
                    for (const auto& read : each->listed->fields)
                        if (read.output == link.output) {
                            source.kind = Source::Kind::eField;
                            source.component = read.component;
                            source.field = read.field;
                            source.held = read.resource;
                            return source;
                        }
                    source.kind = Source::Kind::eWhere;
                    // Where it is, and which it is, are read straight from the world; anything else the ForEach says, through it.
                    if (link.output != "matrix" && link.output != "normalMatrix" && link.output != "entityId") { run->asked.insert(link.output); run->rows = true; }
                    return source;
                };
                run->mesh = classify("mesh");
                for (const Slot& slot : info->slots) {
                    run->slots.push_back(classify("param." + slot.field));
                    if (run->slots.back().kind == Source::Kind::eNone && slot.instancing) {
                        auto made = InstancesFor(node, *info, slot, *run);
                        if (!made) return Fail(made.error());
                        run->slots.back() = std::move(*made);
                    }
                    if (run->slots.back().kind == Source::Kind::eNone)
                        return Fail(slot.sampler ? std::format("'param.{}' needs a sampler: here, or wired on the pipeline for every draw", slot.field)
                                                 : std::format("'param.{}' is given nothing: wire it here — from a ForEach, for each element's own — or on the pipeline, for every draw", slot.field));
                    if (run->slots.back().kind == Source::Kind::eGiven) return Fail(std::format("'param.{}' takes a resource, wired: not a value", slot.field));
                }
                for (const PushMember& push : info->pushes) {
                    Source source = classify("push." + push.name);
                    // Given nothing: its default in the shader.
                    if (source.kind == Source::Kind::eNone && !push.initial.IsNull()) { source.kind = Source::Kind::eGiven; source.value = push.initial; }
                    if (source.kind == Source::Kind::eField && !source.held) {
                        kor::Pipeline::PushConstantMember member;
                        member.scalar = static_cast<kor::u8>(push.scalar);
                        member.rows = static_cast<kor::u8>(push.rows);
                        member.columns = static_cast<kor::u8>(push.columns);
                        member.size = push.size;
                        member.count = 1;
                        const auto [type, size] = ConstantType(member);
                        source.raw = source.field.type == type && source.field.size >= size;
                    }
                    if (source.kind == Source::Kind::eField && source.held) return Fail(std::format("'push.{}' is a value, and was wired to a resource", push.name));
                    run->pushes.push_back(std::move(source));
                }
                // What is worked out between asks the ForEach for what it is worked out of.
                for (const auto& id : run->chain)
                    if (const Node* between = current ? CurrentNode(id) : nullptr)
                        for (const auto& input : between->inputs | std::views::values)
                            if (input.link && input.link->node == run->each) run->asked.insert(input.link->output);
                return run;
            };

            if (node.type == "Clear") {
                const auto target = image("image");
                if (!target) return Fail(target.error());
                kor::Vec4 color(0.f, 0.f, 0.f, 1.f);
                if (const Value& given = value("color"); !given.IsNull()) {
                    const auto* items = given.Items();
                    if (!items || items->size() != 4 || !std::ranges::all_of(*items, [](const Value& v) { return v.Number() != nullptr; }))
                        return Fail("'color' is four numbers: red, green, blue, alpha");
                    for (int i = 0; i < 4; ++i) color[i] = static_cast<float>(*(*items)[static_cast<std::size_t>(i)].Number());
                }
                if ((*target)->screen) {
                    plan->writesScreen = true;
                    plan->record = [color](kor::CommandBuffer& cb, const PassContext& context) { cb.ClearColorImage(context.screen, color); };
                } else {
                    if (lacks(**target, kor::Image::Usage::eTransferDst)) return Fail("the image needs eTransferDst among its usage, to be cleared");
                    const auto ref = ImageRefOf(target->get());
                    // What is cleared to is the bits of a clear value: a depth image's depth and stencil, and an image of whole
                    // numbers' numbers, are those bits, not the floats a colour is.
                    const std::string format = FormatName(ref->PixelFormat());
                    const TexelLayout texel = TexelLayoutOf(format);
                    const auto bits = [](const std::uint32_t whole) { return std::bit_cast<float>(whole); };
                    if (format.starts_with("eD") || format.starts_with("eS8")) {
                        const double depth = num("depth", 1.0, 0.0, 1.0);
                        const double stencil = num("stencil", 0.0, 0.0, 255.0);
                        if (!wrong.empty()) return Fail(wrong);
                        color = kor::Vec4(static_cast<float>(depth), bits(static_cast<std::uint32_t>(stencil)), 0.f, 0.f);
                    } else if (texel.kind == 'U' || texel.kind == 'I') {
                        for (int i = 0; i < 4; ++i) {
                            const double n = std::trunc(static_cast<double>(color[i]));
                            color[i] = texel.kind == 'U' ? bits(static_cast<std::uint32_t>(std::clamp(n, 0.0, 4294967295.0)))
                                                         : bits(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(std::clamp(n, -2147483648.0, 2147483647.0))));
                        }
                    }
                    plan->uses.push_back({ MemberOf(ref), false, true });
                    plan->record = [color, ref](kor::CommandBuffer& cb, const PassContext&) { cb.ClearColorImage(ref, color); };
                }
            } else if (node.type == "Blit") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen) return Fail("'source' cannot be the screen");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be copied from");
                const auto from = ImageRefOf(source->get());
                kor::Blit how;
                if (pick("filter", { "eNearest", "eLinear" }) == 1) how.filtering = kor::Filter::eLinear;
                how.srcOffset = each3("sourceOffset", 0);
                how.srcExtent = each3("sourceExtent", -1);
                how.dstOffset = each3("destinationOffset", 0);
                how.dstExtent = each3("destinationExtent", -1);
                how.srcMipLevel = whole("sourceMip");
                how.dstMipLevel = whole("destinationMip");
                how.srcBaseArrayLayer = whole("sourceLayer");
                how.dstBaseArrayLayer = whole("destinationLayer");
                how.layerCount = whole("layers", 1);
                if (!wrong.empty()) return Fail(wrong);
                if (how.srcMipLevel >= from->MipLevels()) return Fail(std::format("'sourceMip' is {}, and the source has {} mip levels", how.srcMipLevel, from->MipLevels()));
                plan->uses.push_back({ MemberOf(from), true, false });
                if ((*destination)->screen) {
                    plan->writesScreen = true;
                    plan->record = [from, how](kor::CommandBuffer& cb, const PassContext& context) { cb.Blit(from, context.screen, how); };
                } else {
                    if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be copied to");
                    const auto to = ImageRefOf(destination->get());
                    if (how.dstMipLevel >= to->MipLevels()) return Fail(std::format("'destinationMip' is {}, and the destination has {} mip levels", how.dstMipLevel, to->MipLevels()));
                    // A part of it written leaves the rest as it was: it is read as well.
                    const bool part = has("destinationOffset.x") || has("destinationOffset.y") || has("destinationExtent.x") || has("destinationExtent.y") || how.dstMipLevel > 0;
                    plan->uses.push_back({ MemberOf(to), part, true });
                    plan->record = [from, to, how](kor::CommandBuffer& cb, const PassContext&) { cb.Blit(from, to, how); };
                }
            } else if (node.type == "Resolve") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen) return Fail("'source' cannot be the screen");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be resolved from");
                const auto from = ImageRefOf(source->get());
                if (from->Samples() == kor::SampleCount::e1)
                    return Fail("the source has one sample a pixel, and there is nothing to resolve: give it more (sampleCount), or use a Blit");
                kor::Resolve how;
                how.srcOffset = each3("sourceOffset", 0);
                how.srcExtent = each3("sourceExtent", -1);
                how.dstOffset = each3("destinationOffset", 0);
                how.dstExtent = each3("destinationExtent", -1);
                how.srcMipLevel = whole("sourceMip");
                how.dstMipLevel = whole("destinationMip");
                how.srcBaseArrayLayer = whole("sourceLayer");
                how.dstBaseArrayLayer = whole("destinationLayer");
                how.layerCount = whole("layers", 1);
                if (!wrong.empty()) return Fail(wrong);
                plan->uses.push_back({ MemberOf(from), true, false });
                if ((*destination)->screen) {
                    plan->writesScreen = true;
                    plan->record = [from, how](kor::CommandBuffer& cb, const PassContext& context) { cb.Resolve(from, context.screen, how); };
                } else {
                    if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be resolved to");
                    const auto to = ImageRefOf(destination->get());
                    if (to->Samples() != kor::SampleCount::e1) return Fail("the destination is multisampled too: a resolve is onto an image of one sample a pixel");
                    plan->uses.push_back({ MemberOf(to), false, true });
                    plan->record = [from, to, how](kor::CommandBuffer& cb, const PassContext&) { cb.Resolve(from, to, how); };
                }
            } else if (node.type == "Copy") {
                const auto source = image("source");
                if (!source) return Fail(source.error());
                const auto destination = image("destination");
                if (!destination) return Fail(destination.error());
                if ((*source)->screen || (*destination)->screen) return Fail("the screen is shown with a Blit: a Copy is between two images of one size");
                if (lacks(**source, kor::Image::Usage::eTransferSrc)) return Fail("the source needs eTransferSrc among its usage, to be copied from");
                if (lacks(**destination, kor::Image::Usage::eTransferDst)) return Fail("the destination needs eTransferDst among its usage, to be copied to");
                const auto from = ImageRefOf(source->get());
                const auto to = ImageRefOf(destination->get());
                if (from->Extent() != to->Extent())
                    return Fail(std::format("a Copy is between images of one size, and these are {}x{} and {}x{}: a Blit scales", from->Extent().x, from->Extent().y, to->Extent().x, to->Extent().y));
                plan->uses.push_back({ MemberOf(from), true, false });
                plan->uses.push_back({ MemberOf(to), false, true });
                plan->record = [from, to](kor::CommandBuffer& cb, const PassContext&) { cb.CopyImage(from, to); };
            } else if (node.type == "GenerateMipmaps") {
                const auto target = image("image");
                if (!target) return Fail(target.error());
                if ((*target)->screen) return Fail("the screen has no mip levels");
                if (lacks(**target, kor::Image::Usage::eTransferSrc) || lacks(**target, kor::Image::Usage::eTransferDst))
                    return Fail("the image needs eTransferSrc and eTransferDst among its usage: each level is made by copying the one above");
                const auto ref = ImageRefOf(target->get());
                if (ref->MipLevels() < 2) return Fail("the image has one mip level: give it more (mipLevels) for there to be any to fill");
                plan->uses.push_back({ MemberOf(ref), true, true });
                plan->record = [ref](kor::CommandBuffer& cb, const PassContext&) { cb.GenerateMipmaps(ref); };
            } else if (node.type == "CopyBuffer") {
                const auto from = bufferIn("source", true, kor::Buffer::Usage::eTransferSrc, "eTransferSrc", "to be copied from");
                const auto to = bufferIn("destination", true, kor::Buffer::Usage::eTransferDst, "eTransferDst", "to be copied to");
                const auto sourceOffset = static_cast<kor::u64>(num("sourceOffset", 0, 0, 1e15));
                const auto destinationOffset = static_cast<kor::u64>(num("destinationOffset", 0, 0, 1e15));
                if (!wrong.empty()) return Fail(wrong);
                if (sourceOffset >= from->size() || destinationOffset >= to->size()) return Fail("an offset is past the end of its buffer");
                const kor::u64 most = std::min(from->size() - sourceOffset, to->size() - destinationOffset);
                const auto bytes = static_cast<kor::u64>(num("bytes", static_cast<double>(most), 1, static_cast<double>(most)));
                if (!wrong.empty()) return Fail(wrong);
                plan->uses.push_back({ bufferUse(from), true, false });
                plan->uses.push_back({ bufferUse(to), bytes < to->size(), true });
                plan->record = [from, to, bytes, sourceOffset, destinationOffset](kor::CommandBuffer& cb, const PassContext&) {
                    cb.CopyBuffer(from, to, bytes, sourceOffset, destinationOffset);
                };
            } else if (node.type == "Keep") {
                // A copy of its own of what it is given, made like it — and to be read, sampled or as storage — and copied into where this pass runs.
                const auto given = inputs.find("of");
                if (given == inputs.end() || !given->second.resource) return Fail("'of' needs a buffer or an image wired in: what is kept a copy of");
                const Held& of = given->second.resource;
                if (of->screen) return Fail("the screen is not kept: draw it into an image, and keep that");
                Held kept;
                if (const auto buffer = BufferRefOf(of.get()); buffer.Get()) {
                    if (MadeWithout(*of, kor::Buffer::Usage::eTransferSrc)) return Fail("the buffer needs eTransferSrc among its usage, to be kept a copy of");
                    auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(buffer->size()))
                        .SetUsage(buffer->UsageFlags() | kor::Buffer::Usage::eTransferDst | kor::Buffer::Usage::eStorage).SetType(kor::Buffer::Type::eDeviceLocal).Build();
                    if (!made.Valid()) return Fail("its copy could not be made");
                    kept = MakeResource(std::move(made));
                    const auto to = BufferRefOf(kept.get());
                    // Its source as read and written, though only read: so that it takes its place among what writes it, where
                    // it is among the passes — not after all of them, as what only reads does — and copies it as it is there.
                    plan->uses.push_back({ bufferUse(buffer), true, true });
                    plan->uses.push_back({ bufferUse(to), false, true });
                    plan->record = [buffer, to](kor::CommandBuffer& cb, const PassContext&) { cb.CopyBuffer(buffer, to, buffer->size(), 0, 0); };
                } else if (const auto picture = ImageRefOf(of.get()); picture.Get()) {
                    if (MadeWithout(*of, kor::Image::Usage::eTransferSrc)) return Fail("the image needs eTransferSrc among its usage, to be kept a copy of");
                    if (picture->Samples() != kor::SampleCount::e1) return Fail("a multisampled image is not kept: resolve it into one of a sample a pixel, and keep that");
                    auto made = kor::Image::Builder().SetType(picture->ImageType()).SetFormat(picture->PixelFormat()).SetExtent(picture->Extent())
                        .SetMipLevels(picture->MipLevels()).SetArrayLayers(picture->ArrayLayers())
                        .SetUsage(picture->UsageFlags() | kor::Image::Usage::eTransferDst | kor::Image::Usage::eSampled).Build();
                    if (!made.Valid()) return Fail("its copy could not be made");
                    kept = MakeResource(std::move(made));
                    const auto to = ImageRefOf(kept.get());
                    plan->uses.push_back({ MemberOf(picture), true, true });     // (read and written: as a buffer's, above)
                    plan->uses.push_back({ MemberOf(to), false, true });
                    plan->record = [picture, to](kor::CommandBuffer& cb, const PassContext&) { cb.CopyImage(picture, to); };
                } else {
                    return Fail(std::format("'of' is a {}: a buffer or an image is kept", of->kind));
                }
                kept->node = node.id;
                out.outputs["out"].resource = std::move(kept);
                // Valid once it has been copied into: on the frames after the first that records it.
                out.recorded = std::make_shared<std::atomic<std::uint64_t>>(0);
                out.outputs["valid"].value = false;
                plan->record = [copy = std::move(plan->record), recorded = out.recorded](kor::CommandBuffer& cb, const PassContext& context) {
                    copy(cb, context);
                    ++*recorded;
                };
            } else if (node.type == "ClearBuffer") {
                const auto buffer = bufferIn("buffer", true, kor::Buffer::Usage::eTransferDst, "eTransferDst", "to be cleared");
                const auto offset = static_cast<kor::u64>(num("offset", 0, 0, 1e15));
                if (!wrong.empty()) return Fail(wrong);
                if (offset >= buffer->size()) return Fail("'offset' is past the end of the buffer");
                const auto bytes = static_cast<kor::u64>(num("bytes", static_cast<double>(buffer->size() - offset), 1, static_cast<double>(buffer->size() - offset)));
                if (!wrong.empty()) return Fail(wrong);
                plan->uses.push_back({ bufferUse(buffer), bytes < buffer->size(), true });
                plan->record = [buffer, offset, bytes](kor::CommandBuffer& cb, const PassContext&) { cb.ClearBuffer(buffer, offset, bytes); };
            } else if (node.type == "CopyBufferToImage" || node.type == "CopyImageToBuffer") {
                const bool toImage = node.type == "CopyBufferToImage";
                const auto picture = image("image");
                if (!picture) return Fail(picture.error());
                if ((*picture)->screen) return Fail("the screen is not copied to or from a buffer: an image is, and a Blit shows it");
                const auto buffer = toImage ? bufferIn("buffer", true, kor::Buffer::Usage::eTransferSrc, "eTransferSrc", "to be copied from")
                                            : bufferIn("buffer", true, kor::Buffer::Usage::eTransferDst, "eTransferDst", "to be copied to");
                if (lacks(**picture, toImage ? kor::Image::Usage::eTransferDst : kor::Image::Usage::eTransferSrc))
                    return Fail(toImage ? "the image needs eTransferDst among its usage, to be copied to" : "the image needs eTransferSrc among its usage, to be copied from");
                const auto ref = ImageRefOf(picture->get());
                kor::Copy how;
                how.bufferOffset = static_cast<kor::u64>(num("bufferOffset", 0, 0, 1e15));
                how.bufferRowLength = whole("bufferRowLength");
                how.bufferImageHeight = whole("bufferImageHeight");
                how.imageOffset = each3("imageOffset", 0);
                how.imageExtent = each3("imageExtent", -1);
                how.imageMipLevel = whole("mip");
                how.imageBaseArrayLayer = whole("layer");
                how.imageLayerCount = whole("layers", 1);
                if (!wrong.empty()) return Fail(wrong);
                if (how.imageMipLevel >= ref->MipLevels()) return Fail(std::format("'mip' is {}, and the image has {} mip levels", how.imageMipLevel, ref->MipLevels()));
                if (how.bufferOffset >= buffer->size()) return Fail("'bufferOffset' is past the end of the buffer");
                const bool part = has("imageOffset.x") || has("imageOffset.y") || has("imageExtent.x") || has("imageExtent.y") || how.imageMipLevel > 0;
                plan->uses.push_back({ bufferUse(buffer), toImage, !toImage });
                plan->uses.push_back({ MemberOf(ref), !toImage || part, toImage });
                if (toImage) plan->record = [buffer, ref, how](kor::CommandBuffer& cb, const PassContext&) { cb.CopyBufferToImage(buffer, ref, how); };
                else plan->record = [buffer, ref, how](kor::CommandBuffer& cb, const PassContext&) {
                    // An image made anew at another size, a frame before what reads it back is made to fit: left out
                    // for that frame, rather than copied past the end of the buffer.
                    if (const std::size_t texel = TexelLayoutOf(FormatName(ref->PixelFormat())).Bytes(); texel > 0) {
                        const auto extent = ref->Extent();
                        const std::size_t wide = how.imageExtent.x > 0 ? static_cast<std::size_t>(how.imageExtent.x) : std::max<std::size_t>(extent.x >> how.imageMipLevel, 1);
                        const std::size_t tall = how.imageExtent.y > 0 ? static_cast<std::size_t>(how.imageExtent.y) : std::max<std::size_t>(extent.y >> how.imageMipLevel, 1);
                        if (how.bufferOffset + wide * tall * texel * std::max<std::size_t>(how.imageLayerCount, 1) > buffer->size()) return;
                    }
                    cb.CopyImageToBuffer(ref, buffer, how);
                };
            } else if (node.type == "Readback") {
                // A few bytes of a buffer, or one texel of an image, copied into the next of its buffers each frame.
                auto state = std::make_shared<ReadState>();
                kor::ResourceRef<const kor::Buffer> from;
                kor::ResourceRef<const kor::Image> picture;
                kor::Copy one;
                kor::u64 offset = 0;
                const auto buffer = inputs.find("buffer");
                const auto pictureIn = inputs.find("image");
                if (buffer != inputs.end() && buffer->second.resource) {
                    from = bufferIn("buffer", true, kor::Buffer::Usage::eTransferSrc, "eTransferSrc", "to be read back");
                    static const std::map<std::string, std::pair<char, std::size_t>, std::less<>> types {
                        { "eFloat", { 'f', 1 } }, { "eInt", { 'i', 1 } }, { "eUInt", { 'u', 1 } }, { "eVec2", { 'f', 2 } }, { "eVec3", { 'f', 3 } }, { "eVec4", { 'f', 4 } },
                        { "eIVec2", { 'i', 2 } }, { "eIVec4", { 'i', 4 } }, { "eUVec2", { 'u', 2 } }, { "eUVec4", { 'u', 4 } } };
                    const Value& typed = value("type");
                    const auto found = types.find(typed.String() ? *typed.String() : std::string("eFloat"));
                    if (found == types.end()) return Fail("'type' is eFloat, eInt, eUInt, eVec2, eVec3, eVec4, eIVec2, eIVec4, eUVec2 or eUVec4");
                    state->kind = found->second.first;
                    state->count = found->second.second;
                    state->bytes = 4 * state->count;
                    offset = static_cast<kor::u64>(num("offset", 0, 0, 1e15));
                    if (!wrong.empty()) return Fail(wrong);
                    if (offset + state->bytes > from->size()) return Fail(std::format("{} bytes from {} is past the end of the buffer, of {}", state->bytes, offset, from->size()));
                    plan->uses.push_back({ bufferUse(from), true, false });
                } else if (pictureIn != inputs.end() && pictureIn->second.resource) {
                    const auto given = image("image");
                    if (!given) return Fail(given.error());
                    if ((*given)->screen) return Fail("the screen is not read back: draw it into an image, and read that");
                    if (lacks(**given, kor::Image::Usage::eTransferSrc)) return Fail("the image needs eTransferSrc among its usage, to be read back");
                    picture = ImageRefOf(given->get());
                    const TexelLayout texel = TexelLayoutOf(FormatName(picture->PixelFormat()));
                    if (texel.channels == 0) return Fail(std::format("a texel of {} is not read back: a format of channels of one size is", FormatName(picture->PixelFormat())));
                    state->texel = texel;
                    state->bytes = texel.Bytes();
                    const Value& at = value("texel");
                    std::vector<double> where;
                    if (!at.IsNull()) {
                        auto read = NumbersOf(at, "texel");
                        if (!read || read->size() < 2) return Fail("'texel' is x and y, and z or a layer");
                        where = std::move(*read);
                    }
                    one.imageMipLevel = whole("mip");
                    if (!wrong.empty()) return Fail(wrong);
                    if (one.imageMipLevel >= picture->MipLevels()) return Fail(std::format("'mip' is {}, and the image has {} mip levels", one.imageMipLevel, picture->MipLevels()));
                    const auto extent = picture->Extent();
                    const int wide = std::max(static_cast<int>(extent.x >> one.imageMipLevel), 1), tall = std::max(static_cast<int>(extent.y >> one.imageMipLevel), 1);
                    const int x = where.empty() ? 0 : static_cast<int>(where[0]), y = where.empty() ? 0 : static_cast<int>(where[1]);
                    if (x < 0 || y < 0 || x >= wide || y >= tall) return Fail(std::format("texel ({}, {}) is outside the image, of {} by {}", x, y, wide, tall));
                    const int z = where.size() > 2 ? static_cast<int>(where[2]) : 0;
                    const bool layered = picture->ArrayLayers() > 1;
                    one.imageOffset = kor::IVec3(x, y, layered ? 0 : z);
                    one.imageBaseArrayLayer = layered ? static_cast<kor::u32>(std::max(z, 0)) : 0u;
                    one.imageExtent = { 1, 1, 1 };
                    plan->uses.push_back({ MemberOf(picture), true, false });
                } else {
                    return Fail("'buffer' or 'image' needs wiring: what is read back");
                }
                for (std::size_t i = 0; i < ReadState::Ring; ++i) {
                    auto made = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(std::max<std::size_t>(state->bytes, 4))).SetUsage(kor::Buffer::Usage::eTransferDst)
                        .SetType(kor::Buffer::Type::eReadback).Build();
                    if (!made.Valid()) return Fail("a buffer it is read back into could not be made");
                    state->buffers[i] = kor::ResourceRef<const kor::Buffer>(made);
                    state->owners[i] = std::make_shared<kor::Resource<kor::Buffer>>(std::move(made));
                }
                out.readback = state;
                out.outputs["ready"].value = false;
                plan->record = [state, from, picture, one, offset](kor::CommandBuffer& cb, const PassContext&) {
                    const auto into = state->buffers[state->copies % ReadState::Ring];
                    if (from.Get()) cb.CopyBuffer(from, into, state->bytes, offset, 0);
                    else cb.CopyImageToBuffer(picture, into, one);
                    ++state->copies;
                };
            } else if (node.type == "Pick") {
                // Which entity is at a pixel: the image of ids a pipeline draws `entityId` into, read back when asked.
                const auto picture = image("ids");
                if (!picture) return Fail(picture.error());
                if ((*picture)->screen) return Fail("'ids' is an image of which entity is where, not the screen");
                if (lacks(**picture, kor::Image::Usage::eTransferSrc)) return Fail("the image needs eTransferSrc among its usage, to be read back");
                const auto ref = ImageRefOf(picture->get());
                // The depth round the pixel too, where it is wired: three rows of three, read into a buffer of their own.
                kor::ResourceRef<const kor::Image> depth;
                if (const auto it = inputs.find("depth"); it != inputs.end() && it->second.resource) {
                    depth = ImageRefOf(it->second.resource.get());
                    if (!depth.Get() || MadeWithout(*it->second.resource, kor::Image::Usage::eTransferSrc)) return Fail("'depth' is a depth image with eTransferSrc among its usage, to be read back");
                    if (depth->Extent().x != ref->Extent().x || depth->Extent().y != ref->Extent().y) return Fail("'depth' is as big as 'ids': the depth they were drawn with");
                }
                auto readback = kor::Buffer::RawBuilder{}.SetRawSize(4).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build();
                // (Room past the nine depths: the copy reckons a depth texel's size on the safe side.)
                auto depths = depth.Get() ? kor::Buffer::RawBuilder{}.SetRawSize(9 * 8).SetUsage(kor::Buffer::Usage::eTransferDst).SetType(kor::Buffer::Type::eReadback).Build()
                                          : kor::Resource<kor::Buffer>();
                if (!readback.Valid() || (depth.Get() && !depths.Valid())) return Fail("the buffer a pixel is read back into could not be made");
                const kor::ResourceRef<const kor::Buffer> into(readback), depthsInto(depths);
                const auto owner = std::make_shared<std::pair<kor::Resource<kor::Buffer>, kor::Resource<kor::Buffer>>>(std::move(readback), std::move(depths));
                plan->uses.push_back({ MemberOf(ref), true, false });
                if (depth.Get()) plan->uses.push_back({ MemberOf(depth), true, false });
                plan->prepare = [this, state = this->pick, owner, ref, deep = depth.Get() != nullptr] {
                    // More frames than are in flight: by then the frame that copied it has finished.
                    if (state->waiting > 0 && --state->waiting == 0) {
                        const auto id = owner->first->Read<kor::u32>(1);
                        if (deep) {
                            const auto read = owner->second->Read<float>(9);
                            state->hit.deep = read.size() == 9;
                            for (std::size_t i = 0; i < read.size() && i < 9; ++i) state->hit.depth[i] = read[i];
                        }
                        if (auto then = std::exchange(state->then, {}); then && !id.empty()) then(id[0], state->hit);
                    }
                    state->copyAt.reset();
                    if (const auto asked = std::exchange(state->asked, std::nullopt)) {
                        const auto extent = ref->Extent();
                        state->copyAt = kor::IVec3(std::clamp(static_cast<int>(asked->first * static_cast<float>(extent.x)), 0, static_cast<int>(extent.x) - 1),
                                                   std::clamp(static_cast<int>(asked->second * static_cast<float>(extent.y)), 0, static_cast<int>(extent.y) - 1), 0);
                        state->hit = {};
                        state->hit.pixel = { state->copyAt->x, state->copyAt->y };
                        state->hit.extent = { extent.x, extent.y };
                        state->hit.viewProjection = viewProjection;
                        state->waiting = 4;
                    }
                };
                plan->record = [state = this->pick, into, ref, depthsInto, depth](kor::CommandBuffer& cb, const PassContext&) {
                    if (!state->copyAt) return;
                    kor::Copy one;
                    one.imageOffset = *state->copyAt;
                    one.imageExtent = { 1, 1, 1 };
                    cb.CopyImageToBuffer(ref, into, one);
                    if (!depth.Get()) return;
                    // Each of the nine on its own, the picture's edge standing in for what is past it.
                    const auto extent = depth->Extent();
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column) {
                            kor::Copy texel;
                            texel.imageOffset = { std::clamp(state->copyAt->x + column - 1, 0, static_cast<int>(extent.x) - 1),
                                                  std::clamp(state->copyAt->y + row - 1, 0, static_cast<int>(extent.y) - 1), 0 };
                            texel.imageExtent = { 1, 1, 1 };
                            texel.bufferOffset = static_cast<kor::u64>(row * 3 + column) * 4;
                            cb.CopyImageToBuffer(depth, depthsInto, texel);
                        }
                };
            } else if (node.type == "DebugDraw") {
                // The scene's debug shapes — a gizmo, a light's reach — over a picture, seen as the scene is.
                const auto picture = image("image");
                if (!picture) return Fail(picture.error());
                if ((*picture)->screen) return Fail("the debug shapes are drawn into an image, which is then shown: not straight to the screen");
                if (lacks(**picture, kor::Image::Usage::eColorAttachment)) return Fail("the image needs eColorAttachment among its usage, to be drawn into");
                const auto color = ImageRefOf(picture->get());
                auto builder = kor::Framebuffer::Builder().AddColor({ .view = color });
                kor::ResourceRef<const kor::Image> depth;
                if (const auto it = inputs.find("depth"); it != inputs.end() && it->second.resource) {
                    depth = ImageRefOf(it->second.resource.get());
                    if (!depth.Get() || MadeWithout(*it->second.resource, kor::Image::Usage::eDepthStencilAttachment)) return Fail("'depth' is a depth image, with eDepthStencilAttachment among its usage");
                    builder.SetDepth({ .view = depth });
                }
                auto framebuffer = builder.Build();
                if (!framebuffer.Valid()) return Fail(framebuffer.Failure() ? framebuffer.Failure()->message : std::string("its framebuffer could not be made"));
                const kor::ResourceRef<const kor::Framebuffer> target(framebuffer);
                const auto owner = std::make_shared<kor::Resource<kor::Framebuffer>>(std::move(framebuffer));
                plan->uses.push_back({ MemberOf(color), true, true });
                if (depth.Get()) plan->uses.push_back({ MemberOf(depth), true, true });
                auto matrix = std::make_shared<kor::Mat4>(1.f);
                plan->prepare = [this, owner, target, matrix] {
                    *matrix = viewProjection;
                    if (debug) debug->Prepare(target, *matrix);
                };
                plan->record = [this, target, matrix](kor::CommandBuffer& cb, const PassContext&) {
                    if (debug) debug->Record(cb, *matrix, target);
                };
            } else if (node.type == "Dispatch" || node.type == "TraceRays") {
                const bool rays = node.type == "TraceRays";
                const auto info = pipelineOf(rays ? "RayTracingPipeline" : "ComputePipeline");
                if (!info) return Fail(info.error());
                // A group works on what the shader says it does, unless the node says otherwise.
                kor::u32 groups[3] { 1, 1, 1 }, groupSize[3] { 1, 1, 1 };
                for (int axis = 0; axis < 3; ++axis) groupSize[axis] = std::max<kor::u32>((*info)->numThreads[axis], 1);
                if (rays) std::ranges::fill(groupSize, 1u);   // a ray a pixel
                if (const auto read = counts(rays ? "size" : "groups", groups, rays ? "a number of rays" : "a number of groups"); !read) return Fail(read.error());
                if (const auto read = counts("groupSize", groupSize, "how many a group works on"); !rays && !read) return Fail(read.error());
                // How many groups, read from a buffer a pass before this one wrote.
                const auto indirectBuffer = rays ? kor::ResourceRef<const kor::Buffer>() : bufferIn("indirect", false, kor::Buffer::Usage::eIndirect, "eIndirect", "to be dispatched from");
                const auto indirectOffset = rays ? 0 : static_cast<kor::u64>(num("indirectOffset", 0, 0, 1e15));
                if (!wrong.empty()) return Fail(wrong);
                if (indirectBuffer.Get() && has("cover")) return Fail("'indirect' and 'cover' both say how many groups: one or the other");

                auto made = drawsOf(*info, false);
                if (!made) return Fail(made.error());
                const std::shared_ptr<EntityRun> run = std::move(*made);
                // A TraceRays' buffer of each instance's, as a Draw's: a pass may read its material too.
                if (!run->instances.empty()) {
                    out.outputs["instances"].resource = run->instances.front()->held;
                    out.outputs["instanceStride"].value = Value(static_cast<double>(run->instances.front()->instancing->stride));     // the bytes of an element of it
                }
                // A Dispatch of a Filter's entities: its groups once for each of them, one after another along z, in one dispatch.
                if (!rays)
                    if (const auto entities = node.inputs.find("entities"); entities != node.inputs.end() && entities->second.link) {
                        const Node* filter = Origin(entities->second.link->node);
                        const auto listed = filter ? built.find(filter->id) : built.end();
                        if (listed == built.end() || !listed->second.listed) return Fail("'entities' is a Filter's entities, or an Entities node's: what it runs once for each of");
                        if (!run->each.empty())
                            return Fail(std::format("it runs once for each of '{}', and is wired a value of each element of '{}' too: what each is to its shader is its "
                                                    "[entity(\"PER_INSTANCE\")] buffer's", filter->id, run->each));
                        if (indirectBuffer.Get()) return Fail("'indirect' says how many groups, and 'entities' that they are run once for each: one or the other");
                        run->batchOf = filter->id;
                    }
                // An image to cover: as many groups as it takes, each element's own image or one the pipeline wires.
                if (const Value& cover = value("cover"); !cover.IsNull()) {
                    const std::string* name = cover.String();
                    if (!name) return Fail("'cover' is the name of one of the pipeline's image parameters");
                    bool found = false;
                    for (std::size_t i = 0; i < (*info)->slots.size() && !found; ++i) {
                        const Slot& slot = (*info)->slots[i];
                        if (slot.field == *name && !slot.sampler && IsImage(slot.type)) { run->cover = static_cast<int>(i); found = true; }
                    }
                    if (const auto wired = (*info)->wired.find(*name); !found && wired != (*info)->wired.end()) {
                        if (const auto covered = ImageRefOf(wired->second.get()); covered.Get()) {
                            // A view of one of an image's smaller levels is as big as that level is.
                            const kor::u32 level = wired->second->kind == "ImageView" ? kor::capi::ImageViewOf(wired->second->handle)->BaseMipLevel() : 0;
                            const auto extent = covered->Extent();
                            for (int axis = 0; axis < 3; ++axis)
                                groups[axis] = std::max<kor::u32>((std::max<kor::u32>(static_cast<kor::u32>(extent[axis]) >> level, 1) + groupSize[axis] - 1) / groupSize[axis], 1);
                            found = true;
                        }
                    }
                    if (!found) return Fail(std::format("'cover' names one of the pipeline's image parameters, and '{}' is not one", *name));
                }
                std::ranges::copy(groups, run->groups);
                std::ranges::copy(groupSize, run->groupSize);
                plan->uses = (*info)->uses;
                if (indirectBuffer.Get()) plan->uses.push_back({ bufferUse(indirectBuffer), true, false });
                plan->entities = run;
                KoralResource* const handle = inputs.at("pipeline").resource->handle;
                const auto compute = rays ? kor::ResourceRef<const kor::ComputePipeline>() : kor::ResourceRef<const kor::ComputePipeline>(kor::capi::ComputePipelineOf(handle));
                const auto tracing = rays ? kor::ResourceRef<const kor::RayTracingPipeline>(kor::capi::RayTracingPipelineOf(handle)) : kor::ResourceRef<const kor::RayTracingPipeline>();
                plan->record = [compute, tracing, rays, indirectBuffer, indirectOffset, info = *info, run](kor::CommandBuffer& cb, const PassContext&) {
                    if (run->items.empty()) return;
                    if (rays) cb.BindRayTracingPipeline(tracing);
                    else cb.BindComputePipeline(compute);
                    if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    for (const Item& item : run->items) {
                        if (item.set.Get()) cb.BindDescriptorSet(1, item.set);
                        if (info->pushSize) cb.PushConstantBytes({ run->pushed.data() + item.push, info->pushSize }, info->pushFirst);
                        if (rays) cb.TraceRays(item.groups[0], item.groups[1], item.groups[2]);
                        else if (indirectBuffer.Get()) cb.DispatchIndirect(indirectBuffer, indirectOffset);
                        else cb.Dispatch(item.groups[0], item.groups[1], item.groups[2]);
                    }
                };
            } else {   // Draw
                const auto info = pipelineOf("GraphicsPipeline");
                if (!info) return Fail(info.error());
                Held target = (*info)->framebuffer;
                if (const auto it = inputs.find("framebuffer"); it != inputs.end() && it->second.resource) target = it->second.resource;
                if (!target || !target->handle || target->kind != "Framebuffer")
                    return Fail("it draws into a framebuffer: the pipeline's, or one wired to 'framebuffer'. To show it, Blit one of the framebuffer's images to the screen");
                const auto framebuffer = kor::capi::FramebufferOf(target->handle);

                // Which draw.
                enum : int { eAuto, ePlain, eIndexed, eTasks, eIndirect, eIndexedIndirect, eIndirectCount, eIndexedIndirectCount, eTasksIndirect };
                int command = std::max(pick("command", { "eAuto", "eDraw", "eDrawIndexed", "eDrawMeshTasks", "eDrawIndirect", "eDrawIndexedIndirect", "eDrawIndirectCount",
                                                         "eDrawIndexedIndirectCount", "eDrawMeshTasksIndirect" }), 0);
                if (!wrong.empty()) return Fail(wrong);
                if (command == eAuto && (*info)->meshShaders) command = eTasks;
                const bool tasks = command == eTasks || command == eTasksIndirect;
                if (tasks && !(*info)->meshShaders) return Fail("it draws mesh tasks, and the pipeline has no mesh shader: its 'stages' is eVertex");
                if (!tasks && (*info)->meshShaders) return Fail("the pipeline is drawn with mesh shaders: 'command' is eDrawMeshTasks or eDrawMeshTasksIndirect, or left as eAuto");
                const bool indirect = command >= eIndirect;
                const bool counted = command == eIndirectCount || command == eIndexedIndirectCount;
                const bool mustIndex = command == eIndexed || command == eIndexedIndirect || command == eIndexedIndirectCount;

                // What is drawn, and with what: once, or once for each element of the ForEach it is wired from.
                auto made = drawsOf(*info, true);
                if (!made) return Fail(made.error());
                const std::shared_ptr<EntityRun> run = std::move(*made);
                // Its buffer of each instance's, when it made or shares one: to be looked at like any buffer.
                if (!run->instances.empty()) {
                    out.outputs["instances"].resource = run->instances.front()->held;
                    out.outputs["instanceStride"].value = Value(static_cast<double>(run->instances.front()->instancing->stride));     // the bytes of an element of it
                    for (const auto& field : run->instances.front()->instancing->fields)     // where in an element its MATRIX is
                        if (field.from == Instancing::Field::From::eMatrix) { out.outputs["instanceMatrixOffset"].value = Value(static_cast<double>(field.offset)); break; }
                    if (run->drawn) out.outputs["drawnInstances"].resource = run->drawn;     // compacted: what its shader reads, which a cull writes
                }
                run->tasks = tasks;
                if (tasks && run->mesh.kind != Source::Kind::eNone) return Fail("mesh shaders make their own vertices: there is no 'mesh' to give them");
                if (!tasks && run->mesh.kind == Source::Kind::eNone && (mustIndex || (*info)->vertexInputs))
                    return Fail("its pipeline's vertex shader reads vertices: wire it a 'mesh' — a ForEach of Mesh's `field.mesh`, a mesh for each element, or a Primitive's");
                if (run->mesh.kind == Source::Kind::eGiven) return Fail("'mesh' takes a mesh, wired: not a value");
                const std::optional<kor::u64> vertexCount = has("vertexCount") ? std::optional<kor::u64>(whole("vertexCount", 3)) : std::nullopt;
                const kor::u32 firstVertex = whole("firstVertex");
                const auto vertexOffset = static_cast<kor::i32>(std::llround(num("vertexOffset", 0, -2147483648.0, 2147483647.0)));
                const kor::u32 instances = static_cast<kor::u32>(std::llround(num("instances", 1, 1)));
                const kor::u32 firstInstance = whole("firstInstance");
                kor::u32 taskGroups[3] { 1, 1, 1 };
                if (const auto read = counts("tasks", taskGroups, "a number of groups"); !read) return Fail(read.error());

                const auto indirectBuffer = bufferIn("indirect", indirect, kor::Buffer::Usage::eIndirect, "eIndirect", "to be drawn from");
                const auto countBuffer = bufferIn("count", counted, kor::Buffer::Usage::eIndirect, "eIndirect", "to be counted from");
                const auto indirectOffset = static_cast<kor::u64>(num("indirectOffset", 0, 0, 1e15));
                const auto countOffset = static_cast<kor::u64>(num("countOffset", 0, 0, 1e15));
                // None: an IndirectDraws of nothing yet — the framebuffer is cleared, and nothing drawn.
                const kor::u32 drawCount = static_cast<kor::u32>(std::llround(num("drawCount", 1, 0)));
                const kor::u32 stride = whole("stride");
                if (!wrong.empty()) return Fail(wrong);
                if (!indirect && indirectBuffer.Get()) return Fail("'indirect' is read by an Indirect command: choose one for 'command'");
                if (!counted && countBuffer.Get()) return Fail("'count' is read by an IndirectCount command: choose one for 'command'");

                // What the framebuffer starts as, and what is kept of it.
                kor::RenderInfo render(framebuffer);
                const std::optional<bool> loadAll = flag("load");
                const std::vector<std::string> loads { "eClear", "eLoad", "eDontCare" }, stores { "eStore", "eDontCare" };
                const auto loadOf = [&](const char* name) {
                    const int chosen = pick(name, loads);
                    if (chosen < 0) return loadAll.value_or(false) ? kor::LoadOperation::eLoad : kor::LoadOperation::eClear;
                    return chosen == 0 ? kor::LoadOperation::eClear : chosen == 1 ? kor::LoadOperation::eLoad : kor::LoadOperation::eDontCare;
                };
                const auto storeOf = [&](const char* name) { return pick(name, stores) == 1 ? kor::StoreOperation::eDontCare : kor::StoreOperation::eStore; };
                render.SetColorLoadOperation(loadOf("colorLoad")).SetDepthLoadOperation(loadOf("depthLoad")).SetStencilLoadOperation(loadOf("stencilLoad"));
                render.SetColorStoreOperation(storeOf("colorStore")).SetDepthStoreOperation(storeOf("depthStore")).SetStencilStoreOperation(storeOf("stencilStore"));
                if (const auto color = several("clearColor", 4, "red, green, blue, alpha"))
                    for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i) render.SetClearColor(i, kor::Vec4((*color)[0], (*color)[1], (*color)[2], (*color)[3]));
                if (has("clearDepth")) render.SetClearDepth(static_cast<float>(num("clearDepth", 1, 0, 1)));
                if (has("clearStencil")) render.SetClearStencil(static_cast<kor::i32>(whole("clearStencil")));

                // What the pipeline leaves to the draw.
                struct State {
                    std::optional<std::vector<float>> viewport, scissor, depthBias, blendConstants;
                    int cullMode = -1, frontFace = -1, depthCompare = -1;
                    std::optional<bool> depthTest, depthWrite, stencilTest, rasterizerDiscard, primitiveRestart;
                    std::optional<float> lineWidth;
                    std::optional<kor::u32> stencilReference, stencilCompareMask, stencilWriteMask;
                } state;
                state.viewport = several("viewport", 4, "x, y, width, height");
                state.scissor = several("scissor", 4, "x, y, width, height");
                state.depthBias = several("depthBias", 3, "a constant, a clamp, and a factor of the slope");
                state.blendConstants = several("blendConstants", 4, "red, green, blue, alpha");
                state.cullMode = pick("cullMode", { "eNone", "eFront", "eBack", "eFrontAndBack" });
                state.frontFace = pick("frontFace", { "eCounterClockwise", "eClockwise" });
                state.depthCompare = pick("depthCompare", { "eNever", "eLess", "eEqual", "eLessOrEqual", "eGreater", "eNotEqual", "eGreaterOrEqual", "eAlways" });
                state.depthTest = flag("depthTest");
                state.depthWrite = flag("depthWrite");
                state.stencilTest = flag("stencilTest");
                state.rasterizerDiscard = flag("rasterizerDiscard");
                state.primitiveRestart = flag("primitiveRestart");
                if (has("lineWidth")) state.lineWidth = static_cast<float>(num("lineWidth", 1, 0, 64));
                if (has("stencilReference")) state.stencilReference = whole("stencilReference");
                if (has("stencilCompareMask")) state.stencilCompareMask = whole("stencilCompareMask");
                if (has("stencilWriteMask")) state.stencilWriteMask = whole("stencilWriteMask");
                for (const auto* box : { &state.viewport, &state.scissor })
                    if (*box && (std::ranges::any_of(**box, [](const float v) { return v < 0.f; }) || (**box)[2] < 1.f || (**box)[3] < 1.f))
                        mistaken("'viewport' and 'scissor' are x, y, width, height, in pixels: none below zero, and a width and a height of 1 or more");
                if (!wrong.empty()) return Fail(wrong);

                if (const auto cull = node.inputs.find("cull"); cull != node.inputs.end() && cull->second.link) {
                    run->cullNode = cull->second.link->node;
                    run->cullOutput = cull->second.link->output;
                    if (const auto index = inputs.find("cullIndex"); index != inputs.end() && index->second.value.Number()) run->cullIndex = static_cast<int>(*index->second.value.Number());
                }
                if (const auto idle = node.inputs.find("idle"); idle != node.inputs.end() && idle->second.link) {
                    run->idleNode = idle->second.link->node;
                    run->idleOutput = idle->second.link->output;
                    if (const auto index = inputs.find("cullIndex"); index != inputs.end() && index->second.value.Number()) run->cullIndex = static_cast<int>(*index->second.value.Number());
                }
                run->command = mustIndex ? DrawWith::eDrawIndexed : command == eAuto ? DrawWith::eAuto : DrawWith::eDraw;
                run->whenChanged = flag("whenChanged").value_or(false);
                const auto drawsInto = [&](const kor::ResourceRef<const kor::ImageView>& view) {
                    if (!view.Get()) return;
                    run->targets.push_back(view.Get());
                    if (view->SourceImage().Get() && view->SourceImage()->IsPerFrame())
                        run->copies = std::max(run->copies, kor::Context::HasScheduler() ? kor::Context::Scheduler().ImageCount() : 1u);
                };
                for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i) drawsInto(framebuffer->ColorAttachment(i));
                if (framebuffer->HasDepthAttachment()) drawsInto(framebuffer->DepthAttachment());

                const bool colorLoaded = render.ColorLoadOperation() == kor::LoadOperation::eLoad;
                for (kor::u32 i = 0; i < framebuffer->ColorAttachmentCount(); ++i)
                    if (const auto view = framebuffer->ColorAttachment(i); view.Get()) plan->uses.push_back({ MemberOf(view->SourceImage()), colorLoaded, true });
                if (framebuffer->HasDepthAttachment())
                    if (const auto view = framebuffer->DepthAttachment(); view.Get()) plan->uses.push_back({ MemberOf(view->SourceImage()), true, true });
                plan->uses.insert(plan->uses.end(), (*info)->uses.begin(), (*info)->uses.end());
                if (indirectBuffer.Get()) plan->uses.push_back({ bufferUse(indirectBuffer), true, false });
                if (countBuffer.Get()) plan->uses.push_back({ bufferUse(countBuffer), true, false });
                plan->entities = run;
                plan->record = [=, info = *info, t0 = taskGroups[0], t1 = taskGroups[1], t2 = taskGroups[2]](kor::CommandBuffer& cb, const PassContext&) {
                    const auto extent = framebuffer->Extent();
                    if (run->idle) return;   // left as it was drawn last: not cleared, and nothing drawn
                    cb.BeginRendering(render);   // with nothing to draw, it still clears
                    // Bound with what goes with it: a pipeline made for another mesh's layout is bound as the node's is.
                    const auto ready = [&](const kor::ResourceRef<const kor::GraphicsPipeline>& bind) {
                        cb.BindGraphicsPipeline(bind);
                        const auto box = [&](const std::optional<std::vector<float>>& given, const int at, const kor::u32 otherwise) {
                            return given ? static_cast<kor::u32>((*given)[static_cast<std::size_t>(at)]) : otherwise;
                        };
                        cb.SetViewport(box(state.viewport, 0, 0), box(state.viewport, 1, 0), box(state.viewport, 2, extent.x), box(state.viewport, 3, extent.y));
                        cb.SetScissor(box(state.scissor, 0, 0), box(state.scissor, 1, 0), box(state.scissor, 2, extent.x), box(state.scissor, 3, extent.y));
                        if (state.cullMode >= 0) {
                            kor::Flags<kor::CullMode> cull {};
                            if (state.cullMode == 1 || state.cullMode == 3) cull |= kor::CullMode::eFront;
                            if (state.cullMode == 2 || state.cullMode == 3) cull |= kor::CullMode::eBack;
                            cb.SetCullMode(cull);
                        }
                        if (state.frontFace >= 0) cb.SetFrontFace(state.frontFace == 0 ? kor::FrontFace::eCounterClockwise : kor::FrontFace::eClockwise);
                        if (state.depthTest) cb.SetDepthTestEnable(*state.depthTest);
                        if (state.depthWrite) cb.SetDepthWriteEnable(*state.depthWrite);
                        if (state.depthCompare >= 0) cb.SetDepthCompareOp(static_cast<kor::CompareOp>(state.depthCompare));
                        if (state.depthBias) cb.SetDepthBiasEnable(true).SetDepthBias((*state.depthBias)[0], (*state.depthBias)[1], (*state.depthBias)[2]);
                        if (state.lineWidth) cb.SetLineWidth(*state.lineWidth);
                        if (state.blendConstants) cb.SetBlendConstants({ (*state.blendConstants)[0], (*state.blendConstants)[1], (*state.blendConstants)[2], (*state.blendConstants)[3] });
                        if (state.stencilTest) cb.SetStencilTestEnable(*state.stencilTest);
                        if (state.stencilReference) cb.SetStencilReference(kor::StencilFace::eFrontAndBack, *state.stencilReference);
                        if (state.stencilCompareMask) cb.SetStencilCompareMask(kor::StencilFace::eFrontAndBack, *state.stencilCompareMask);
                        if (state.stencilWriteMask) cb.SetStencilWriteMask(kor::StencilFace::eFrontAndBack, *state.stencilWriteMask);
                        if (state.rasterizerDiscard) cb.SetRasterizerDiscardEnable(*state.rasterizerDiscard);
                        if (state.primitiveRestart) cb.SetPrimitiveRestartEnable(*state.primitiveRestart);
                        if (info->hasSet) cb.BindDescriptorSet(0, info->set);
                    };
                    const kor::GraphicsPipeline* lastPipeline = nullptr;
                    const kor::Mesh* lastMesh = nullptr;
                    const kor::DescriptorSet* lastSet = nullptr;
                    const auto draw = [&](const Item& item) {
                        if (item.pipeline.Get() != lastPipeline) {
                            ready(item.pipeline);
                            lastPipeline = item.pipeline.Get();
                            lastMesh = nullptr;
                            lastSet = nullptr;
                        }
                        if (item.set.Get() && item.set.Get() != lastSet) { cb.BindDescriptorSet(1, item.set); lastSet = item.set.Get(); }
                        if (info->pushSize) cb.PushConstantBytes({ run->pushed.data() + item.push, info->pushSize }, info->pushFirst);
                        const kor::ResourceRef<const kor::Mesh>& bound = item.mesh;
                        if (bound.Get() && bound.Get() != lastMesh) { cb.BindMesh(bound); lastMesh = bound.Get(); }
                        if (indirect && drawCount == 0) return;
                        switch (command) {
                        case eTasks: cb.DrawMeshTasks(t0, t1, t2); break;
                        case eIndirect: cb.DrawIndirect(indirectBuffer, indirectOffset, drawCount, stride); break;
                        case eIndexedIndirect: cb.DrawIndexedIndirect(indirectBuffer, indirectOffset, drawCount, stride); break;
                        case eIndirectCount: cb.DrawIndirectCount(indirectBuffer, indirectOffset, countBuffer, countOffset, drawCount, stride); break;
                        case eIndexedIndirectCount: cb.DrawIndexedIndirectCount(indirectBuffer, indirectOffset, countBuffer, countOffset, drawCount, stride); break;
                        case eTasksIndirect: cb.DrawMeshTasksIndirect(indirectBuffer, indirectOffset, drawCount, stride); break;
                        default:
                            if (bound.Get() && item.part) cb.DrawIndexed(item.indexCount, instances, item.firstIndex, item.vertexOffset, firstInstance);
                            else if (item.indexed) cb.DrawIndexed(vertexCount.value_or(kor::WholeSize), instances, firstVertex, vertexOffset, firstInstance);
                            // With no mesh there is nothing to say how many: a triangle, unless the node says.
                            else cb.Draw(vertexCount.value_or(bound.Get() ? kor::WholeSize : 3), instances, firstVertex, firstInstance);
                        }
                    };
                    for (const Item& item : run->items) draw(item);
                    cb.EndRendering();
                };
            }
            // Its `idle`: while what it is given is not zero, or true, it records nothing — a value worked out each frame,
            // read as the frame is readied. (A Draw's is its own: a number for each turn.)
            if (node.type != "Draw")
                if (const auto idle = node.inputs.find("idle"); idle != node.inputs.end() && plan->record) {
                    const auto said = [](const Value& value) { return value.Bool() ? *value.Bool() : value.Number() && *value.Number() != 0.0; };
                    // Resting from the first frame as it is built to, before a frame has been readied to say so.
                    const auto now = inputs.find("idle");
                    auto resting = std::make_shared<std::atomic<bool>>(now != inputs.end() && said(now->second.value));
                    if (idle->second.link) {
                        plan->prepare = [this, before = std::move(plan->prepare), resting, said, from = *idle->second.link] {
                            if (before) before();
                            const auto source = built.find(from.node);
                            const auto output = source == built.end() ? nullptr : &source->second.outputs;
                            const auto it = output ? output->find(from.output) : decltype(output->find(from.output)) {};
                            resting->store(output && it != output->end() && said(it->second.value));
                        };
                    } else {
                        resting->store(said(idle->second.value));
                    }
                    plan->record = [record = std::move(plan->record), resting](kor::CommandBuffer& cb, const PassContext& context) {
                        if (!resting->load()) record(cb, context);
                    };
                    plan->idle = resting;
                }
            // On a page of the fixed step, what it writes is to be there for the next step: a copy for each frame in
            // flight is not — each step would go on from another copy.
            if (current && current->IsFixed(node.pass))
                for (const auto& use : plan->uses)
                    if (use.write && ((use.what.image.Get() && use.what.image->IsPerFrame()) || (use.what.buffer.Get() && use.what.buffer->IsPerFrame()))) {
                        out.status.note = "it writes a per-frame resource on the fixed step: a copy for each frame in flight, so a step does not go on "
                                          "from what the step before wrote. A state kept from step to step is one copy (isPerFrame false).";
                        break;
                    }
            out.pass = std::move(plan);
            return {};
        }

    auto Runtime::Impl::MatrixOf(const Value& value, const char* name) -> Expected<kor::Mat4>
    {
            const auto numbers = NumbersOf(value, name);
            if (!numbers || numbers->size() != 16) return Fail(std::format("'{}' is a matrix: sixteen numbers, column after column", name));
            kor::Mat4 m(1.f);
            for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = static_cast<float>((*numbers)[static_cast<std::size_t>(i)]);
            return m;
        }

    auto Runtime::Impl::SeenThrough() -> void
    { viewProjection = mainProjection * currentView.view; }

    auto Runtime::Impl::BuildView(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            const auto vector = [&](const char* name, const kor::Vec3 otherwise) -> Expected<kor::Vec3> {
                const Value* v = given(name);
                if (!v) return otherwise;
                const auto numbers = NumbersOf(*v, name);
                if (!numbers || numbers->size() < 3) return Fail(std::format("'{}' is a vector of three", name));
                return kor::Vec3(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]));
            };
            // What a View was before: a camera's or a light's, by its struct. It is numbers now, wired.
            for (const char* gone : { "of", "kind", "face", "fieldOfView", "size", "extent" })
                if (inputs.contains(gone))
                    return Fail(std::format("'{}' is no View's now: a View is a position, a target and up — a ForEach's `worldPosition`, that and its "
                                            "`forward` added, and its `up` — and a Perspective wired from it is the lens", gone));
            const auto position = vector("position", kor::Vec3(0.f));
            if (!position) return Fail(position.error());
            const auto target = vector("target", *position + kor::Vec3(0.f, 0.f, -1.f));
            if (!target) return Fail(target.error());
            const auto up = vector("up", kor::Vec3(0.f, 1.f, 0.f));
            if (!up) return Fail(up.error());
            const bool main = given("main") && given("main")->Bool() && *given("main")->Bool();

            kor::Mat4 view(1.f);
            if (main && viewOverride) {
                view = viewOverride->view;      // where whoever runs the graph says the scene is seen from: an editor's own
            } else {
                // Looking at where it is, or with up the way it looks, is no way to look: a step along -z, and another up.
                kor::Vec3 towards = *target - *position;
                towards = kor::Length(towards) > 1e-6f ? kor::Normalize(towards) : kor::Vec3(0.f, 0.f, -1.f);
                kor::Vec3 upward = kor::Length(*up) > 1e-6f ? kor::Normalize(*up) : kor::Vec3(0.f, 1.f, 0.f);
                if (kor::Length(kor::Cross(towards, upward)) < 1e-4f) upward = std::abs(towards.y) > 0.99f ? kor::Vec3(0.f, 0.f, 1.f) : kor::Vec3(0.f, 1.f, 0.f);
                view = kor::LookAt(*position, *position + towards, upward);
            }
            const kor::Mat4 placed = kor::Inverse(view);
            if (main) { currentView.view = view; SeenThrough(); }
            const auto set = [&](const char* name, Value value) { out.outputs[name].value = std::move(value); };
            set("out", Matrix(view));
            set("view", Matrix(view));
            set("inverseView", Matrix(placed));
            set("position", Three({ placed[3][0], placed[3][1], placed[3][2] }));
            set("forward", Three(kor::Normalize(kor::Vec3(-placed[2][0], -placed[2][1], -placed[2][2]))));
            set("up", Three(kor::Normalize(kor::Vec3(placed[1][0], placed[1][1], placed[1][2]))));
            set("right", Three(kor::Normalize(kor::Vec3(placed[0][0], placed[0][1], placed[0][2]))));
            return {};
        }

    auto Runtime::Impl::BuildLens(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            static constexpr float Radians = 3.14159265358979323846f / 180.f;
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            std::string failed;
            const auto number = [&](const char* name, const float otherwise) {
                const Value* v = given(name);
                if (!v) return otherwise;
                if (!v->Number()) { if (failed.empty()) failed = std::format("'{}' is a number", name); return otherwise; }
                return static_cast<float>(*v->Number());
            };
            for (const char* gone : { "view", "extent" })
                if (inputs.contains(gone))
                    return Fail(std::format("'{}' is no lens's now: a lens is of nothing but its numbers — a MatrixMultiply of it and a View is the two as one, "
                                            "and a Screen's `aspect` is its aspect", gone));
            float aspect = 1.f;
            if (const Value* own = given("aspect")) {
                if (!own->Number() || !(*own->Number() > 0.0)) return Fail("'aspect' is a number more than 0: the picture's width over its height");
                aspect = static_cast<float>(*own->Number());
            }
            const bool main = given("main") && given("main")->Bool() && *given("main")->Bool();
            const auto set = [&](const char* name, Value value) { out.outputs[name].value = std::move(value); };
            const std::string& type = node.type;
            kor::Mat4 projection(1.f);
            if (type == "Perspective") {
                float fieldOfView = number("fieldOfView", 60.f), near = number("near", 0.1f), far = number("far", 100.f);
                if (main && viewOverride) { fieldOfView = viewOverride->fieldOfView / Radians; near = viewOverride->near; far = viewOverride->far; }
                fieldOfView = std::clamp(fieldOfView, 1.f, 179.f);
                near = std::max(near, 1e-4f);
                const bool endless = !(far > 0.f) || !std::isfinite(far);
                if (!endless && far <= near) return Fail(std::format("'far' ({}) is to be farther than 'near' ({}), or 0 for no far plane", far, near));
                const bool reversed = given("reversedZ") && given("reversedZ")->Bool() && *given("reversedZ")->Bool();
                if (main && viewOverride && viewOverride->orthographic > 0.f) {
                    // The editor's view seen straight on: a box as tall as it says, depth the same way round as the lens's.
                    const float high = viewOverride->orthographic * 0.5f, wide = high * aspect;
                    const float from = viewOverride->near, to = viewOverride->far;
                    projection = reversed ? kor::Orthographic(-wide, wide, -high, high, to, from) : kor::Orthographic(-wide, wide, -high, high, from, to);
                } else if (reversed) projection = kor::PerspectiveReversedZ(fieldOfView * Radians, aspect, near, endless ? std::numeric_limits<float>::infinity() : far);
                else if (endless) projection = kor::InfinitePerspective(fieldOfView * Radians, aspect, near);
                else projection = kor::Perspective(fieldOfView * Radians, aspect, near, far);
                set("fieldOfView", fieldOfView);
                set("aspect", aspect);
                set("near", near);
                set("far", endless ? 0.f : far);
                if (main) { currentView.fieldOfView = fieldOfView * Radians; currentView.near = near; currentView.far = endless ? 1e6f : far; }
            } else if (type == "Orthographic" || type == "Frustum") {
                const bool box = type == "Orthographic";
                const float high = number("size", 10.f) * 0.5f, wide = high * aspect;
                const float left = number("left", box ? -wide : -0.1f), right = number("right", box ? wide : 0.1f);
                const float bottom = number("bottom", box ? -high : -0.1f), top = number("top", box ? high : 0.1f);
                const float near = number("near", box ? 0.f : 0.1f), far = number("far", 100.f);
                if (left == right || bottom == top) return Fail("'left' and 'right', and 'bottom' and 'top', are to be apart: it would see nothing");
                if (far <= near) return Fail(std::format("'far' ({}) is to be farther than 'near' ({})", far, near));
                if (!box && near <= 0.f) return Fail("'near' is to be more than 0: a perspective starts in front of where it is seen from");
                projection = box ? kor::Orthographic(left, right, bottom, top, near, far) : kor::FrustumProjection(left, right, bottom, top, near, far);
                set("near", near);
                set("far", far);
            } else {   // Oblique
                const Value* own = given("projection");
                if (!own) return Fail("'projection' is the projection whose near plane is moved: a Perspective's");
                const auto lens = MatrixOf(*own, "projection");
                if (!lens) return Fail(lens.error());
                projection = *lens;
                const Value* plane = given("plane");
                const auto numbers = plane ? NumbersOf(*plane, "plane") : Expected<std::vector<double>>(Fail(""));
                if (!numbers || numbers->size() != 4) return Fail("'plane' is four numbers: its normal, and its distance");
                const kor::Vec4 clip(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]), static_cast<float>((*numbers)[3]));
                // Lengyel's: the far corner of the picture on the plane's side, and the depth row made the plane, scaled so
                // that corner is still at depth 1.
                const auto sign = [](const float v) { return v > 0.f ? 1.f : v < 0.f ? -1.f : 0.f; };
                const kor::Vec4 corner = kor::Inverse(projection) * kor::Vec4(sign(clip.x * projection[0][0]), sign(clip.y * projection[1][1]), 1.f, 1.f);
                const float along = kor::Dot(clip, corner);
                if (std::abs(along) < 1e-12f) return Fail("'plane' goes through where it is seen from: a near plane is in front of it");
                const kor::Vec4 row = clip * (1.f / along);
                for (int column = 0; column < 4; ++column) projection[column][2] = row[column];
            }
            if (type != "Oblique" && !(given("flipY") && given("flipY")->Bool() && !*given("flipY")->Bool()))
                for (int column = 0; column < 4; ++column) projection[column][1] = -projection[column][1];
            if (!failed.empty()) return Fail(failed);
            if (main) { mainProjection = projection; currentAspect = aspect; SeenThrough(); }
            set("out", Matrix(projection));
            set("projection", Matrix(projection));
            set("inverseProjection", Matrix(kor::Inverse(projection)));
            return {};
        }

    auto Runtime::Impl::BuildParaboloid(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            const float near = given("near") && given("near")->Number() ? static_cast<float>(*given("near")->Number()) : 0.1f;
            const float far = given("far") && given("far")->Number() ? static_cast<float>(*given("far")->Number()) : 100.f;
            if (far <= near) return Fail(std::format("'far' ({}) is to be farther than 'near' ({})", far, near));
            std::string hemisphere = "eFront";
            if (const Value* v = given("hemisphere")) {
                if (!v->String() || (*v->String() != "eFront" && *v->String() != "eBack")) return Fail("'hemisphere' is eFront or eBack");
                hemisphere = *v->String();
            }
            if (given("view")) return Fail("'view' is no Paraboloid's now: a MatrixMultiply of its `turn` and a View's matrix is the view it bends from");
            const bool back = hemisphere == "eBack";
            kor::Mat4 turn(1.f);
            if (back) turn[0][0] = turn[2][2] = -1.f;      // half a turn round its up: its back is its front
            const Value params(Value::List { near, far, 1.f / (far - near), back ? -1.f : 1.f });
            out.outputs["out"].value = params;
            out.outputs["params"].value = params;
            out.outputs["turn"].value = Matrix(turn);
            out.outputs["near"].value = near;
            out.outputs["far"].value = far;
            return {};
        }

    auto Runtime::Impl::BuildCubeFace(const Inputs& inputs, Built& out) -> Expected<void>
    {
            static const kor::Vec3 towards[6] { { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
            static const kor::Vec3 ups[6] { { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f } };
            const auto it = inputs.find("face");
            const double* face = it == inputs.end() ? nullptr : it->second.value.Number();
            const int which = face ? static_cast<int>(std::lround(*face)) : 0;
            if (which < 0 || which > 5) return Fail(std::format("'face' is from 0 to 5, not {}", which));
            out.outputs["direction"].value = Three(towards[which]);
            out.outputs["up"].value = Three(ups[which]);
            return {};
        }

    auto Runtime::Impl::BuildCascades(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto given = [&](const char* name) -> const Value* {
                const auto it = inputs.find(name);
                return it == inputs.end() || it->second.value.IsNull() ? nullptr : &it->second.value;
            };
            const auto number = [&](const char* name, const float otherwise) {
                const Value* v = given(name);
                return v && v->Number() ? static_cast<float>(*v->Number()) : otherwise;
            };
            const auto vector = [&](const Value* v, const kor::Vec3 otherwise) {
                if (!v) return otherwise;
                const auto numbers = NumbersOf(*v, "vector");
                return numbers && numbers->size() >= 3 ? kor::Vec3(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2])) : otherwise;
            };
            if (inputs.contains("camera")) return Fail("'camera' is no ShadowCascades' now: wire the camera's View to 'view' and its Perspective to 'projection'");
            if (!given("view") || !given("projection")) return Fail("'view' and 'projection' are the camera's the cascades are fitted to: a View's and a Perspective's");
            const auto view = MatrixOf(*given("view"), "view");
            if (!view) return Fail(view.error());
            const auto lens = MatrixOf(*given("projection"), "projection");
            if (!lens) return Fail(lens.error());
            const kor::Mat4 placed = kor::Inverse(*view), unseen = kor::Inverse(*lens);
            // Where the camera's near and far planes are, out of its projection: each corner of the picture at depth 0 and
            // at depth 1, as it is seen. A far plane at no distance — an endless perspective's — is none.
            const auto back = [&](const float x, const float y, const float z) -> std::optional<kor::Vec3> {
                const kor::Vec4 p = unseen * kor::Vec4(x, y, z, 1.f);
                if (std::abs(p.w) < 1e-9f) return std::nullopt;
                const kor::Vec3 at(p.x / p.w, p.y / p.w, p.z / p.w);
                if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || std::abs(at.z) > 1e7f) return std::nullopt;
                return at;
            };
            auto nearest = back(0.f, 0.f, 0.f), farthest = back(0.f, 0.f, 1.f);
            const bool reversed = nearest && (!farthest || -nearest->z > -farthest->z);
            const float nearZ = reversed ? 1.f : 0.f, farZ = reversed ? 0.f : 1.f;
            if (reversed) std::swap(nearest, farthest);
            if (!nearest) return Fail("'projection' has no near plane: a Perspective's, an Orthographic's or a Frustum's");
            const float cameraNear = std::max(-nearest->z, 0.001f), cameraFar = farthest ? -farthest->z : std::numeric_limits<float>::infinity();
            // A corner of what it sees, as far from it as `depth`: along the edge from the near plane to the far one — or,
            // with no far plane, along the ray from where it is seen from.
            const auto cornerAt = [&](const float x, const float y, const float depth) {
                const kor::Vec3 start = back(x, y, nearZ).value_or(kor::Vec3(0.f, 0.f, -cameraNear));
                if (const auto end = back(x, y, farZ); end && -end->z - -start.z > 1e-6f)
                    return start + (*end - start) * ((depth - -start.z) / (-end->z - -start.z));
                return start * (depth / std::max(-start.z, 1e-6f));
            };

            const float resolution = std::max(number("resolution", 2048.f), 1.f), lambda = std::clamp(number("lambda", 0.85f), 0.f, 1.f);
            const kor::Vec3 lower = vector(given("boundsMin"), { -20.f, 0.f, -20.f }), upper = vector(given("boundsMax"), { 20.f, 20.f, 20.f });
            kor::Vec3 direction = vector(given("direction"), kor::Vec3(-0.45f, -1.f, -0.35f));
            direction = kor::Length(direction) > 0.f ? kor::Normalize(direction) : kor::Vec3(0.f, -1.f, 0.f);

            const kor::Vec3 up = std::abs(direction.y) > 0.99f ? kor::Vec3(0.f, 0.f, 1.f) : kor::Vec3(0.f, 1.f, 0.f);
            const kor::Mat4 turned = kor::LookAt(kor::Vec3(0.f), direction, up);     // the light's turn, and no place
            const kor::Mat4 undo = kor::Inverse(turned);
            const float near = cameraNear, far = std::max(std::min(cameraFar, number("distance", 200.f)), near * 1.01f);
            Value::List matrices, splits, texels;
            float nearer = near;
            for (int cascade = 0; cascade < 4; ++cascade) {
                // Where the slice ends: between even steps and steps that grow with distance, as lambda says.
                const float part = static_cast<float>(cascade + 1) / 4.f;
                const float farther = (near + (far - near) * part) * (1.f - lambda) + near * std::pow(far / near, part) * lambda;
                // A sphere round the slice: as big whichever way the camera turns, so the shadow does not crawl.
                kor::Vec3 corners[8], centre(0.f);
                int at = 0;
                for (const float depth : { nearer, farther })
                    for (const float y : { -1.f, 1.f })
                        for (const float x : { -1.f, 1.f }) {
                            const kor::Vec3 seen = cornerAt(x, y, depth);
                            const kor::Vec4 corner = placed * kor::Vec4(seen.x, seen.y, seen.z, 1.f);
                            centre += corners[at++] = kor::Vec3(corner.x, corner.y, corner.z);
                        }
                centre /= 8.f;
                float radius = 0.f;
                for (const auto& corner : corners) radius = std::max(radius, kor::Length(corner - centre));
                radius = std::ceil(radius * 16.f) / 16.f;
                // Its centre moves a texel of the map at a time, for the same reason.
                const float texel = 2.f * radius / resolution;
                kor::Vec4 lit = turned * kor::Vec4(centre.x, centre.y, centre.z, 1.f);
                lit.x = std::floor(lit.x / texel) * texel;
                lit.y = std::floor(lit.y / texel) * texel;
                const kor::Vec4 snapped = undo * lit;
                const kor::Vec3 middle(snapped.x, snapped.y, snapped.z);
                // Back towards the light as far as the scene goes: what is above the slice casts into it.
                float reach = 0.f;
                for (int i = 0; i < 8; ++i)
                    reach = std::max(reach, kor::Dot(middle - kor::Vec3(i & 1 ? upper.x : lower.x, i & 2 ? upper.y : lower.y, i & 4 ? upper.z : lower.z), direction));
                const kor::Mat4 seen = kor::LookAt(middle - direction * (reach + radius), middle, up);
                const kor::Mat4 both = kor::Orthographic(-radius, radius, -radius, radius, 0.f, reach + 2.f * radius) * seen;
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) matrices.emplace_back(both[column][row]);
                splits.emplace_back(farther);
                texels.emplace_back(texel);
                nearer = farther;
            }
            out.outputs["matrices"].value = Value(std::move(matrices));
            out.outputs["out"].value = out.outputs["matrices"].value;
            out.outputs["splits"].value = Value(std::move(splits));
            out.outputs["worldTexel"].value = Value(std::move(texels));
            out.outputs["tileTexel"].value = 1.f / resolution;
            out.outputs["direction"].value = Three(direction);
            return {};
        }

    auto Runtime::Impl::BuildPointShadows(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto number = [&](const char* name, const float otherwise) {
                const auto it = inputs.find(name);
                const double* given = it == inputs.end() ? nullptr : it->second.value.Number();
                return given ? static_cast<float>(*given) : otherwise;
            };
            std::vector<double> lights;
            if (const auto it = inputs.find("lights"); it != inputs.end() && !it->second.value.IsNull()
                                                       && !(it->second.value.Items() && it->second.value.Items()->empty())) {    // none yet: no lights
                auto numbers = NumbersOf(it->second.value, "lights");
                if (!numbers) return Fail("'lights' is four numbers a light: where it is, and how far it reaches");
                lights = std::move(*numbers);
            }
            const int most = std::clamp(static_cast<int>(number("most", 8.f)), 1, 64);
            const float near = std::max(number("near", 0.05f), 0.001f);
            static const kor::Vec3 towards[6] { { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
            static const kor::Vec3 ups[6] { { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f } };
            Value::List matrices;
            matrices.reserve(static_cast<std::size_t>(most) * 96);
            const int each = static_cast<int>(number("each", 0.f));
            auto& drawn = pointShadowsDrawn[node.id];
            std::vector<bool> due(static_cast<std::size_t>(most), each <= 0);
            const int count = std::min(most, static_cast<int>(lights.size() / 4));
            for (int light = 0; light < count; ++light) {
                const auto at = static_cast<std::size_t>(light) * 4;
                const kor::Vec3 from(static_cast<float>(lights[at]), static_cast<float>(lights[at + 1]), static_cast<float>(lights[at + 2]));
                const float range = static_cast<float>(lights[at + 3]);
                const kor::Mat4 lens = kor::Perspective(1.5707963f, 1.f, near, std::max(range, near * 2.f));
                const std::array<float, 8> now { from.x, from.y, from.z, range, 0.f, 0.f, 0.f, 0.f };
                if (static_cast<std::size_t>(light) >= drawn.as.size()) drawn.as.resize(static_cast<std::size_t>(light) + 1, { 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f });
                if (drawn.as[static_cast<std::size_t>(light)] != now) { drawn.as[static_cast<std::size_t>(light)] = now; due[static_cast<std::size_t>(light)] = true; }
                for (int face = 0; face < 6; ++face) {
                    const kor::Mat4 both = lens * kor::LookAt(from, from + towards[face], ups[face]);
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row) matrices.emplace_back(both[column][row]);
                }
            }
            // A light there is none of: everything is to one side of what its faces see, and is drawn into none.
            for (int face = count * 6; face < most * 6; ++face)
                for (int i = 0; i < 16; ++i) matrices.emplace_back(i == 12 ? 2.0 : i == 15 ? 1.0 : 0.0);
            out.outputs["matrices"].value = Value(std::move(matrices));
            out.outputs["out"].value = out.outputs["matrices"].value;
            if (each > 0 && count > 0)
                for (int i = 0; i < each; ++i) due[static_cast<std::size_t>((drawn.next + i) % count)] = true;
            if (count > 0) drawn.next = (drawn.next + std::max(each, 0)) % count;
            Value::List idle;
            for (int face = 0; face < most * 6; ++face) idle.emplace_back(face / 6 < count && due[static_cast<std::size_t>(face / 6)] ? 0 : 1);
            out.outputs["idle"].value = Value(std::move(idle));
            out.outputs["count"].value = count;
            out.outputs["faces"].value = most * 6;
            return {};
        }

    auto Runtime::Impl::BuildSpotShadows(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto number = [&](const char* name, const float otherwise) {
                const auto it = inputs.find(name);
                const double* given = it == inputs.end() ? nullptr : it->second.value.Number();
                return given ? static_cast<float>(*given) : otherwise;
            };
            std::vector<double> lights;
            if (const auto it = inputs.find("lights"); it != inputs.end() && !it->second.value.IsNull()
                                                       && !(it->second.value.Items() && it->second.value.Items()->empty())) {    // none yet: no spots
                auto numbers = NumbersOf(it->second.value, "lights");
                if (!numbers || numbers->size() % 8 != 0)
                    return Fail("'lights' is eight numbers a spot: where it is, how far it reaches, the way it shines, and its outer angle in degrees");
                lights = std::move(*numbers);
            }
            const int most = std::clamp(static_cast<int>(number("most", 8.f)), 1, 64);
            const float near = std::max(number("near", 0.05f), 0.001f);
            Value::List matrices;
            matrices.reserve(static_cast<std::size_t>(most) * 16);
            const int each = static_cast<int>(number("each", 0.f));
            auto& drawn = pointShadowsDrawn[node.id];
            std::vector<bool> due(static_cast<std::size_t>(most), each <= 0);
            const int count = std::min(most, static_cast<int>(lights.size() / 8));
            for (int light = 0; light < count; ++light) {
                const auto at = static_cast<std::size_t>(light) * 8;
                const auto f = [&](const std::size_t i) { return static_cast<float>(lights[at + i]); };
                const kor::Vec3 from(f(0), f(1), f(2)), way(f(4), f(5), f(6));
                const float range = f(3), outer = f(7);
                const std::array<float, 8> now { from.x, from.y, from.z, range, way.x, way.y, way.z, outer };
                if (static_cast<std::size_t>(light) >= drawn.as.size()) drawn.as.resize(static_cast<std::size_t>(light) + 1, { 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f });
                if (drawn.as[static_cast<std::size_t>(light)] != now) { drawn.as[static_cast<std::size_t>(light)] = now; due[static_cast<std::size_t>(light)] = true; }
                // Twice its outer angle wide, and a little more, so that its cone's edge is inside the map; as far as its range.
                const float wide = std::clamp((2.f * outer + 2.f) * 0.0174532925f, 0.02f, 3.0f);
                const kor::Mat4 lens = kor::Perspective(wide, 1.f, near, std::max(range, near * 2.f));
                const kor::Vec3 forward = kor::Length(way) > 1e-6f ? kor::Normalize(way) : kor::Vec3(0.f, 0.f, -1.f);
                const kor::Vec3 up = std::abs(forward.y) > 0.99f ? kor::Vec3(0.f, 0.f, 1.f) : kor::Vec3(0.f, 1.f, 0.f);
                const kor::Mat4 both = lens * kor::LookAt(from, from + forward, up);
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) matrices.emplace_back(both[column][row]);
            }
            // A spot there is none of: everything is to one side of what it sees, and is drawn into none.
            for (int map = count; map < most; ++map)
                for (int i = 0; i < 16; ++i) matrices.emplace_back(i == 12 ? 2.0 : i == 15 ? 1.0 : 0.0);
            out.outputs["matrices"].value = Value(std::move(matrices));
            out.outputs["out"].value = out.outputs["matrices"].value;
            if (each > 0 && count > 0)
                for (int i = 0; i < each; ++i) due[static_cast<std::size_t>((drawn.next + i) % count)] = true;
            if (count > 0) drawn.next = (drawn.next + std::max(each, 0)) % count;
            Value::List idle;
            for (int map = 0; map < most; ++map) idle.emplace_back(map < count && due[static_cast<std::size_t>(map)] ? 0 : 1);
            out.outputs["idle"].value = Value(std::move(idle));
            out.outputs["count"].value = count;
            out.outputs["maps"].value = most;
            return {};
        }

    auto Runtime::Impl::InputShape(const Node& consumer, const std::string& name) const -> Shape
    {
            const auto in = [&](const std::vector<Port>& ports) -> Shape {
                const auto port = std::ranges::find(ports, name, &Port::name);
                return port == ports.end() ? Shape {} : port->shape;
            };
            if (const NodeType* type = FindType(consumer.type))
                if (const Shape shape = in(PortsFor(*type, consumer)); shape.Known()) return shape;
            if (name.starts_with("push.") || name.starts_with("param.")) {
                const auto pipeline = consumer.inputs.find("pipeline");
                if (pipeline == consumer.inputs.end() || !pipeline->second.link) return {};
                const Node* of = Origin(pipeline->second.link->node);
                const auto made = of ? built.find(of->id) : built.end();
                return made != built.end() && made->second.pipeline ? in(made->second.pipeline->drawPorts) : Shape {};
            }
            if (name.starts_with("constant."))
                if (const auto made = built.find(consumer.id); made != built.end() && made->second.pipeline) return in(made->second.pipeline->ports);
            return {};
        }

    auto Runtime::Impl::WiredShape(const std::string& id, const int depth) const -> Shape
    {
            if (!current || depth > 8) return {};
            for (const Node& consumer : current->nodes)
                for (const auto& [name, input] : consumer.inputs) {
                    if (!input.link || input.link->node != id) continue;
                    if (const Shape shape = InputShape(consumer, name); shape.Known()) return shape;
                    const NodeType* type = FindType(consumer.type);
                    const auto out = type ? std::ranges::find(type->outputs, std::string_view("out"), &Port::name) : std::vector<Port>::const_iterator {};
                    const bool passes = type && out != type->outputs.end() && out->type == "value"
                                        && std::ranges::any_of(type->inputs, [&](const Port& port) { return port.name == name && port.type == "value"; });
                    if (passes)
                        if (const Shape shape = WiredShape(consumer.id, depth + 1); shape.Known()) return shape;
                }
            return {};
        }

    auto Runtime::Impl::RandomShape(const Node& node) const -> Shape
    {
            if (const auto kind = node.inputs.find("type"); kind != node.inputs.end() && !kind->second.link && kind->second.value.String())
                if (const Shape named = ShapeNamed(*kind->second.value.String()); named.Known()) return named;
            const Shape wired = WiredShape(node.id);
            return wired.Known() && wired.scalar != Scalar::eBool ? wired : Shape { Scalar::eFloat, 1 };
        }

    auto Runtime::Impl::BuildRandom(const Node& node, const random::Distribution distribution, const Inputs& inputs, Value& result) -> Expected<void>
    {
            const NodeType* type = FindType(node.type);
            const auto input = [&](const std::string_view name) -> const Value& {
                static const Value none;
                if (const auto it = inputs.find(std::string(name)); it != inputs.end() && !it->second.value.IsNull()) return it->second.value;
                if (type)
                    if (const auto port = std::ranges::find(type->inputs, name, &Port::name); port != type->inputs.end()) return port->initial;
                return none;
            };
            if (const Value& kind = input("type"); !kind.IsNull() && (!kind.String() || (*kind.String() != "Auto" && !ShapeNamed(*kind.String()).Known())))
                return Fail("'type' is Auto, or a scalar or a vector of one: Float to UVec4");
            const auto shaped = randomShapes.find(node.id);
            const Shape shape = shaped != randomShapes.end() ? shaped->second : RandomShape(node);
            const std::size_t size = shape.components;
            const double* seed = input("seed").Number();
            if (!seed) return Fail("'seed' is a number: a whole one, 0 or more");
            std::mt19937_64 engine(static_cast<std::uint64_t>(std::clamp(std::trunc(*seed), 0.0, 18446744073709549568.0)));

            Value::List components;
            for (std::size_t component = 0; component < size; ++component) {
                random::Parameters with;
                for (const auto& each : random::InputsOf(distribution)) {
                    const Value& given = input(each.name);
                    double* to = random::ParameterNamed(with, each.name);
                    if (const double* n = given.Number()) { *to = *n; continue; }
                    const auto* items = given.Items();
                    if (!items || items->size() != size)
                        return Fail(std::format("'{}' is a number, or a vector of {} numbers, one for each component of the {} it gives", each.name, size, ShapeName(shape)));
                    const double* n = (*items)[component].Number();
                    if (!n) return Fail(std::format("'{}' is a vector of numbers", each.name));
                    *to = *n;
                }
                if (const auto wrong = random::Check(distribution, with, shape.Whole()))
                    return Fail(size > 1 ? std::format("{}, of component {}", *wrong, "xyzw"[component]) : *wrong);
                components.emplace_back(random::Draw(distribution, with, shape.Whole(), engine));
            }
            result = Conform(size == 1 ? components.front() : Value(std::move(components)), shape);
            return {};
        }

    auto Runtime::Impl::BuildColorConvert(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto text = [&](const char* name, const char* otherwise) {
                const auto it = inputs.find(name);
                return it == inputs.end() || !it->second.value.String() ? std::string(otherwise) : *it->second.value.String();
            };
            const std::string from = text("from", "eSrgb"), to = text("to", "eLinear");
            const auto color = inputs.find("color");
            if (color == inputs.end() || color->second.value.IsNull()) return Fail("'color' is not given");
            const auto toLinear = [](const double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
            const auto fromLinear = [](const double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; };
            std::vector<double> rgb;
            bool alone = false;     // one number, not a colour: worked on as one channel
            if (from == "eKelvin") {
                const double* kelvin = color->second.value.Number();
                if (!kelvin || *kelvin <= 0) return Fail("'color' is a temperature, in kelvin: 6500 is daylight");
                // Tanner Helland's fit of a black body's colour, as a display shows it; then made linear.
                const double t = std::clamp(*kelvin, 1000.0, 40000.0) / 100.0;
                const double r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
                const double g = t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60, -0.0755148492);
                const double b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
                for (const double c : { r, g, b }) rgb.push_back(toLinear(std::clamp(c, 0.0, 255.0) / 255.0));
            } else {
                if (const double* n = color->second.value.Number()) { rgb = { *n }; alone = true; }
                else {
                    auto numbers = NumbersOf(color->second.value, "color");
                    if (!numbers) return Fail(numbers.error());
                    if (numbers->size() < 3) return Fail("'color' is red, green, blue and, if given, alpha");
                    rgb = std::move(*numbers);
                }
                const std::size_t channels = alone ? 1 : 3;
                if (from == "eHsv") {
                    if (alone) return Fail("of eHsv, 'color' is hue, saturation and value");
                    const double h = (rgb[0] - std::floor(rgb[0])) * 6.0, sat = rgb[1], v = rgb[2];
                    const double c = v * sat, x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0)), m = v - c;
                    const int sector = static_cast<int>(h) % 6;
                    const double table[6][3] { { c, x, 0 }, { x, c, 0 }, { 0, c, x }, { 0, x, c }, { x, 0, c }, { c, 0, x } };
                    for (int i = 0; i < 3; ++i) rgb[static_cast<std::size_t>(i)] = toLinear(table[sector][i] + m);
                } else {
                    for (std::size_t i = 0; i < channels; ++i) {
                        if (from == "eSrgb") rgb[i] = toLinear(std::max(rgb[i], 0.0));
                        else if (from == "eGamma22") rgb[i] = std::pow(std::max(rgb[i], 0.0), 2.2);
                        else if (from != "eLinear") return Fail(std::format("'from' is eSrgb, eLinear, eGamma22, eHsv or eKelvin, not '{}'", from));
                    }
                }
            }
            // Linear now: how bright it is, and then what it is to be.
            out.outputs["luminance"].value = alone ? rgb[0] : 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
            const std::size_t channels = alone ? 1 : 3;
            if (to == "eHsv") {
                if (alone) return Fail("of eHsv, 'color' is red, green and blue");
                const double r = fromLinear(rgb[0]), g = fromLinear(rgb[1]), b = fromLinear(rgb[2]);
                const double most = std::max({ r, g, b }), least = std::min({ r, g, b }), d = most - least;
                double h = 0;
                if (d > 0) h = most == r ? std::fmod((g - b) / d, 6.0) : most == g ? (b - r) / d + 2 : (r - g) / d + 4;
                h /= 6.0;
                if (h < 0) h += 1;
                rgb[0] = h;
                rgb[1] = most > 0 ? d / most : 0;
                rgb[2] = most;
            } else {
                for (std::size_t i = 0; i < channels; ++i) {
                    if (to == "eSrgb") rgb[i] = fromLinear(std::max(rgb[i], 0.0));
                    else if (to == "eGamma22") rgb[i] = std::pow(std::max(rgb[i], 0.0), 1.0 / 2.2);
                    else if (to != "eLinear") return Fail(std::format("'to' is eLinear, eSrgb, eGamma22 or eHsv, not '{}'", to));
                }
            }
            out.outputs["out"].value = alone ? Value(rgb[0]) : Value(Value::List(rgb.begin(), rgb.end()));
            return {};
        }

    auto Runtime::Impl::BuildQuaternion(const std::string& type, const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto numbers = [&](const char* name, const std::size_t size) -> Expected<std::vector<double>> {
                const auto it = inputs.find(name);
                if (it == inputs.end() || it->second.value.IsNull()) return Fail(std::format("'{}' is not given", name));
                auto read = NumbersOf(it->second.value, name);
                if (!read) return Fail(read.error());
                if (read->size() != size) return Fail(std::format("'{}' is {} numbers", name, size));
                return read;
            };
            const auto quat = [&](const char* name) -> Expected<kecs::Quat> {
                auto q = numbers(name, 4);
                if (!q) return Fail(q.error());
                return kecs::Quat { static_cast<float>((*q)[0]), static_cast<float>((*q)[1]), static_cast<float>((*q)[2]), static_cast<float>((*q)[3]) };
            };
            const auto three = [&](const char* name) -> Expected<kecs::Vec3> {
                auto v = numbers(name, 3);
                if (!v) return Fail(v.error());
                return kecs::Vec3 { static_cast<float>((*v)[0]), static_cast<float>((*v)[1]), static_cast<float>((*v)[2]) };
            };
            const auto give = [&](const kecs::Quat& q) { out.outputs["out"].value = Value(Value::List { q.x, q.y, q.z, q.w }); };
            const auto times = [](const kecs::Quat& a, const kecs::Quat& b) {
                return kecs::Quat { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
            };
            if (type == "QuatFromEuler") {
                const auto rotation = three("rotation");
                if (!rotation) return Fail(rotation.error());
                give(kecs::ToQuat(*rotation));
            } else if (type == "QuatFromAxisAngle") {
                const auto axis = three("axis");
                if (!axis) return Fail(axis.error());
                const auto angle = inputs.find("angle");
                const double* degrees = angle == inputs.end() ? nullptr : angle->second.value.Number();
                if (!degrees) return Fail("'angle' is how far it turns, in degrees");
                const float length = std::sqrt(axis->x * axis->x + axis->y * axis->y + axis->z * axis->z);
                if (length <= 0.f) return Fail("'axis' is a direction: not of length 0");
                const float half = static_cast<float>(*degrees * std::numbers::pi / 360.0), sine = std::sin(half) / length;
                give({ axis->x * sine, axis->y * sine, axis->z * sine, std::cos(half) });
            } else if (type == "QuatMultiply") {
                const auto a = quat("a");
                if (!a) return Fail(a.error());
                const auto b = quat("b");
                if (!b) return Fail(b.error());
                give(times(*a, *b));
            } else {
                const auto q = quat(type == "QuatSlerp" ? "a" : "q");
                if (!q) return Fail(q.error());
                if (type == "QuatToEuler") {
                    const kecs::Vec3 angles = kecs::ToEuler(kecs::Normalize(*q));
                    out.outputs["out"].value = Value(Value::List { angles.x, angles.y, angles.z });
                } else if (type == "QuatToMatrix") {
                    out.outputs["out"].value = Matrix(ToKor(kecs::ComposeQuat({}, *q, { 1.f, 1.f, 1.f })));
                } else if (type == "QuatInverse") {
                    const float square = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
                    if (square <= 0.f) return Fail("'q' is of length 0: no turn to turn back");
                    give({ -q->x / square, -q->y / square, -q->z / square, q->w / square });
                } else if (type == "QuatRotate") {
                    const auto v = three("vector");
                    if (!v) return Fail(v.error());
                    const kecs::Quat unit = kecs::Normalize(*q);
                    const kecs::Quat turned = times(times(unit, { v->x, v->y, v->z, 0.f }), { -unit.x, -unit.y, -unit.z, unit.w });
                    out.outputs["out"].value = Value(Value::List { turned.x, turned.y, turned.z });
                } else if (type == "QuatSlerp") {
                    const auto b = quat("b");
                    if (!b) return Fail(b.error());
                    const auto it = inputs.find("t");
                    const double* t = it == inputs.end() ? nullptr : it->second.value.Number();
                    if (!t) return Fail("'t' is a number: 0 a, 1 b");
                    const kecs::Quat from = kecs::Normalize(*q);
                    kecs::Quat towards = kecs::Normalize(*b);
                    double cosine = from.x * towards.x + from.y * towards.y + from.z * towards.z + from.w * towards.w;
                    // The shorter way round: a quaternion and its negation turn alike.
                    if (cosine < 0) { towards = { -towards.x, -towards.y, -towards.z, -towards.w }; cosine = -cosine; }
                    double wa = 1.0 - *t, wb = *t;
                    if (cosine < 0.9995) {
                        const double angle = std::acos(std::min(cosine, 1.0)), sine = std::sin(angle);
                        wa = std::sin((1.0 - *t) * angle) / sine;
                        wb = std::sin(*t * angle) / sine;
                    }
                    give(kecs::Normalize({ static_cast<float>(wa * from.x + wb * towards.x), static_cast<float>(wa * from.y + wb * towards.y),
                                           static_cast<float>(wa * from.z + wb * towards.z), static_cast<float>(wa * from.w + wb * towards.w) }));
                } else {
                    return Fail(std::format("there is no node type '{}'", type));
                }
            }
            return {};
        }

    auto Runtime::Impl::BuildJitter(const Inputs& inputs, Built& out) -> Expected<void>
    {
            const auto number = [&](const char* name) -> const double* {
                const auto it = inputs.find(name);
                return it == inputs.end() ? nullptr : it->second.value.Number();
            };
            const double* frame = number("frame");
            if (!frame) return Fail("'frame' is which frame it is: a Time's `frame`");
            const double* given = number("phases");
            const auto phases = static_cast<std::uint64_t>(given ? std::max(1.0, std::floor(*given)) : 8.0);
            const std::uint64_t index = static_cast<std::uint64_t>(std::max(0.0, std::floor(*frame))) % phases;
            const auto halton = [](std::uint64_t i, const std::uint64_t base) {
                double f = 1.0, r = 0.0;
                while (i > 0) { f /= static_cast<double>(base); r += f * static_cast<double>(i % base); i /= base; }
                return r;
            };
            const double x = halton(index + 1, 2) - 0.5, y = halton(index + 1, 3) - 0.5;
            double width = 1.0, height = 1.0;
            if (const auto size = inputs.find("size"); size != inputs.end() && !size->second.value.IsNull()) {
                const auto numbers = NumbersOf(size->second.value, "size");
                if (!numbers || numbers->size() < 2) return Fail("'size' is the picture's width and height, in pixels");
                width = std::max((*numbers)[0], 1.0);
                height = std::max((*numbers)[1], 1.0);
            }
            const double cx = 2.0 * x / width, cy = 2.0 * y / height;
            out.outputs["pixels"].value = Value(Value::List { x, y });
            out.outputs["clip"].value = Value(Value::List { cx, cy });
            out.outputs["index"].value = static_cast<double>(index);
            // Moved in clip space by as much at every depth: each column's x and y gain its w times the offset.
            if (const auto projection = inputs.find("projection"); projection != inputs.end() && !projection->second.value.IsNull()) {
                auto m = NumbersOf(projection->second.value, "projection");
                if (!m || m->size() != 16) return Fail("'projection' is a matrix: sixteen numbers");
                for (std::size_t column = 0; column < 4; ++column) {
                    (*m)[column * 4 + 0] += cx * (*m)[column * 4 + 3];
                    (*m)[column * 4 + 1] += cy * (*m)[column * 4 + 3];
                }
                out.outputs["jittered"].value = Value(Value::List(m->begin(), m->end()));
            }
            return {};
        }

    auto Runtime::Impl::BuildValue(const Node& node, const Inputs& inputs, Built& out) -> Expected<void>
    {
            // One of what is wired to it, by number: in a ForEach's chain too, worked out again for each element.
            if (node.type == "Switch") {
                const auto index = inputs.find("index");
                const Value chosen = index == inputs.end() ? Value(0) : index->second.value;
                const int which = chosen.Bool() ? (*chosen.Bool() ? 1 : 0) : chosen.Number() ? static_cast<int>(std::llround(*chosen.Number())) : -1;
                const auto option = inputs.find(std::format("option[{}]", which));
                if (option == inputs.end()) return Fail(std::format("'index' is {}, and there is no 'option[{}]' wired", chosen.ToJson(), which));
                out.outputs["out"].value = option->second.value;
                out.outputs["out"].resource = option->second.resource;
                return {};
            }
            // A Bundle is wiring where it can be written out before anything runs (a buffer's `fields`, a draw's `push`); and
            // a value besides, its members by name, for what takes it whole — a Switch, a Previous, a function's edge.
            if (node.type == "Bundle") {
                Value::Members members;
                if (const auto base = inputs.find("base"); base != inputs.end() && base->second.value.Named()) members = *base->second.value.Named();
                std::vector<std::string> order;
                if (const auto names = inputs.find("names"); names != inputs.end() && names->second.value.Items())
                    for (const auto& name : *names->second.value.Items())
                        if (name.String()) order.push_back(*name.String());
                for (const auto& name : inputs | std::views::keys)
                    if (name.starts_with("field.") && !std::ranges::contains(order, name.substr(6))) order.push_back(name.substr(6));
                for (const auto& member : order) {
                    const auto given = inputs.find("field." + member);
                    if (given == inputs.end() || given->second.value.IsNull()) continue;     // a resource is wiring alone
                    if (const auto had = std::ranges::find(members, member, &Value::Members::value_type::first); had != members.end()) had->second = given->second.value;
                    else members.emplace_back(member, given->second.value);
                }
                out.outputs["out"].value = Value(std::move(members));
                return {};
            }
            // Taken apart as it runs, when what it takes is a struct only then (a Switch of two): an output for each member.
            if (node.type == "Unbundle") {
                if (const auto of = inputs.find("of"); of != inputs.end() && of->second.value.Named())
                    for (const auto& [name, value] : *of->second.value.Named()) out.outputs[name].value = value;
                return {};
            }
            const auto value = [&](const char* name) -> const Value& {
                static const Value none;
                const auto it = inputs.find(name);
                return it == inputs.end() ? none : it->second.value;
            };
            const auto given = [&](const char* name) -> Expected<const Value*> {
                const Value& v = value(name);
                if (v.IsNull()) return Fail(std::format("'{}' is not given", name));
                return &v;
            };
            Value& result = out.outputs["out"].value;
            const std::string& type = node.type;

            // A whole number of a kind: rounded toward zero, kept within what it holds.
            const auto whole = [](const double n, const bool unsigned_) {
                const double low = unsigned_ ? 0.0 : -2147483648.0, high = unsigned_ ? 4294967295.0 : 2147483647.0;
                return std::clamp(std::trunc(n), low, high);
            };
            if (const auto distribution = random::DistributionOfNode(type)) return BuildRandom(node, *distribution, inputs, result);
            if (type == "RandomDevice") {
                auto [kept, fresh] = deviceSeeds.try_emplace(node.id);
                if (fresh || kept->second.draw != value("draw")) kept->second = { value("draw"), std::random_device {}() };
                result = static_cast<double>(kept->second.seed);
                return {};
            }
            if (type == "Double") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = value("value");
            } else if (type == "Float") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = static_cast<double>(static_cast<float>(*value("value").Number()));
            } else if (type == "Int" || type == "UInt") {
                if (!value("value").Number()) return Fail("'value' is a number");
                result = whole(*value("value").Number(), type == "UInt");
            } else if ((type.starts_with("Vec") || type.starts_with("IVec") || type.starts_with("UVec")) && type.size() >= 4 && std::isdigit(static_cast<unsigned char>(type.back()))) {
                // As many components as it is of, each of its kind; one not given, 0.
                const int size = type.back() - '0';
                Value::List components;
                for (int i = 0; i < size; ++i) {
                    const char name[2] { "xyzw"[i], 0 };
                    const Value& given = value(name);
                    if (!given.IsNull() && !given.Number()) return Fail(std::format("'{}' is a number", name));
                    const double n = given.Number() ? *given.Number() : 0.0;
                    components.emplace_back(type.starts_with("Vec") ? static_cast<double>(static_cast<float>(n)) : whole(n, type.starts_with("U")));
                }
                result = Value(std::move(components));
            } else if (type == "Bool") {
                if (!value("value").Bool()) return Fail("'value' is true or false");
                result = value("value");
            } else if (type == "String") {
                if (!value("value").String()) return Fail("'value' is text");
                result = value("value");
            } else if (type == "Color") {
                const auto* items = value("color").Items();
                if (!items || items->size() < 3) return Fail("'color' is red, green, blue and alpha");
                result = value("color");
            } else if (type == "Split") {
                // Each component by its letter; one the vector does not have is 0.
                const auto numbers = NumbersOf(value("vector"), "vector");
                if (!numbers) return Fail(numbers.error());
                const char* letters[] { "x", "y", "z", "w" };
                for (std::size_t i = 0; i < 4; ++i) out.outputs[letters[i]].value = Value(i < numbers->size() ? (*numbers)[i] : 0.0);
                result = value("vector");
            } else if (type == "Append") {
                Value::List joined;
                for (const char* name : { "a", "b" }) {
                    const auto numbers = NumbersOf(value(name), name);
                    if (!numbers) return Fail(numbers.error());
                    for (const double number : *numbers) joined.emplace_back(number);
                }
                result = Value(std::move(joined));
            } else if (type == "Slice") {
                const auto numbers = NumbersOf(value("of"), "of");
                if (!numbers) return Fail(numbers.error());
                const double* first = value("first").Number();
                const double* count = value("count").Number();
                if (!first || !count || *first < 0 || *count < 0) return Fail("'first' and 'count' are numbers, from 0");
                const auto from = static_cast<std::size_t>(*first), many = static_cast<std::size_t>(*count);
                if (from + many > numbers->size())
                    return Fail(std::format("{} from the {}th is past the {} numbers of 'of'", many, from, numbers->size()));
                Value::List part;
                for (std::size_t i = from; i < from + many; ++i) part.emplace_back((*numbers)[i]);
                result = Value(std::move(part));
            } else if (type == "MatrixMultiply" || type == "MatrixInverse" || type == "MatrixTranspose" || type == "MatrixDeterminant") {
                const auto a = MatrixOf(value("a"), "a");
                if (!a) return Fail(a.error());
                if (type == "MatrixInverse") result = Matrix(kor::Inverse(*a));
                else if (type == "MatrixTranspose") result = Matrix(kor::Transpose(*a));
                else if (type == "MatrixDeterminant") result = Value(static_cast<double>(kor::Determinant(*a)));
                else {
                    const auto b = MatrixOf(value("b"), "b");
                    if (!b) return Fail(b.error());
                    result = Matrix(*a * *b);
                }
            } else if (type == "MatrixTranslate" || type == "MatrixRotate" || type == "MatrixAxisAngle" || type == "MatrixScale" || type == "MatrixCompose") {
                // A vector of three by name; a number, for a scale, the same every way.
                const auto three = [&](const char* name, const kor::Vec3 otherwise, const bool even = false) -> Expected<kor::Vec3> {
                    const Value& v = value(name);
                    if (v.IsNull()) return otherwise;
                    if (even && v.Number()) return kor::Vec3(static_cast<float>(*v.Number()));
                    const auto numbers = NumbersOf(v, name);
                    if (!numbers || numbers->size() < 3) return Fail(std::format("'{}' is a vector of three", name));
                    return kor::Vec3(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]));
                };
                const auto position = three(type == "MatrixTranslate" ? "offset" : "position", kor::Vec3(0.f));
                const auto rotation = three(type == "MatrixAxisAngle" ? "axis" : "rotation", kor::Vec3(0.f));
                const auto scale = three("scale", kor::Vec3(1.f), true);
                for (const auto* failed : { &position, &rotation, &scale })
                    if (!*failed) return Fail(failed->error());
                if (type == "MatrixAxisAngle") {
                    const double* angle = value("angle").Number();
                    if (!angle) return Fail("'angle' is a number of degrees");
                    if (kor::Length(*rotation) <= 0.f) return Fail("'axis' is a vector that is not all zero");
                    const kor::Quat turn = kor::Quat::AngleAxis(static_cast<float>(*angle) * 3.14159265358979323846f / 180.f, kor::Normalize(*rotation));
                    result = Matrix(kor::Compose(kor::Vec3(0.f), turn, kor::Vec3(1.f)));
                } else {
                    // Turned as an entity is, by the world's own arithmetic: x, y and z in degrees.
                    const bool moves = type == "MatrixTranslate" || type == "MatrixCompose", turns = type == "MatrixRotate" || type == "MatrixCompose";
                    const bool scales = type == "MatrixScale" || type == "MatrixCompose";
                    result = Matrix(ToKor(kecs::Compose(moves ? kecs::Vec3 { position->x, position->y, position->z } : kecs::Vec3 {},
                                                        turns ? kecs::Vec3 { rotation->x, rotation->y, rotation->z } : kecs::Vec3 {},
                                                        scales ? kecs::Vec3 { scale->x, scale->y, scale->z } : kecs::Vec3 { 1.f, 1.f, 1.f })));
                }
            } else if (type == "TransformPlane") {
                const auto m = MatrixOf(value("matrix"), "matrix");
                if (!m) return Fail(m.error());
                const auto numbers = NumbersOf(value("plane"), "plane");
                if (!numbers || numbers->size() != 4) return Fail("'plane' is four numbers: its normal, and its distance");
                const kor::Vec4 plane(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]), static_cast<float>((*numbers)[3]));
                // A plane moves as the inverse of a point does, transposed.
                const kor::Vec4 moved = kor::Transpose(kor::Inverse(*m)) * plane;
                result = Value(Value::List { moved.x, moved.y, moved.z, moved.w });
            } else if (type == "TransformPoint" || type == "TransformDirection") {
                const auto m = MatrixOf(value("matrix"), "matrix");
                if (!m) return Fail(m.error());
                const bool point = type == "TransformPoint";
                const char* name = point ? "point" : "direction";
                const auto numbers = NumbersOf(value(name), name);
                if (!numbers || numbers->size() < 3) return Fail(std::format("'{}' is a vector of three", name));
                const kor::Vec4 moved = *m * kor::Vec4(static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1]), static_cast<float>((*numbers)[2]), point ? 1.f : 0.f);
                const float w = point && std::abs(moved.w) > 1e-12f ? moved.w : 1.f;
                result = Three({ moved.x / w, moved.y / w, moved.z / w });
            } else if (type == "Constant") {
                static const std::map<std::string, double, std::less<>> constants {
                    { "ePi", 3.14159265358979323846 }, { "eTau", 6.28318530717958647692 }, { "eE", 2.71828182845904523536 },
                    { "eSqrt2", 1.41421356237309504880 }, { "ePhi", 1.61803398874989484820 } };
                const std::string* name = value("value").String();
                const auto it = name ? constants.find(*name) : constants.end();
                if (it == constants.end()) return Fail("'value' is ePi, eTau, eE, eSqrt2 or ePhi");
                result = Value(it->second);
            } else if (type == "Reflect" || type == "Refract" || type == "Project") {
                const char* first = type == "Project" ? "a" : "incident";
                const char* second = type == "Project" ? "onto" : "normal";
                const auto a = NumbersOf(value(first), first);
                if (!a) return Fail(a.error());
                const auto b = NumbersOf(value(second), second);
                if (!b) return Fail(b.error());
                if (a->size() != b->size()) return Fail(std::format("a vector of {} and one of {} do not go together", a->size(), b->size()));
                double dot = 0.0, square = 0.0;
                for (std::size_t i = 0; i < a->size(); ++i) { dot += (*a)[i] * (*b)[i]; square += (*b)[i] * (*b)[i]; }
                Value::List made;
                if (type == "Reflect") {
                    for (std::size_t i = 0; i < a->size(); ++i) made.emplace_back((*a)[i] - 2.0 * dot * (*b)[i]);
                } else if (type == "Project") {
                    for (std::size_t i = 0; i < a->size(); ++i) made.emplace_back(square > 0.0 ? (*b)[i] * dot / square : 0.0);
                } else {
                    const double* eta = value("eta").Number();
                    if (!eta) return Fail("'eta' is a number: the ratio of the indices of refraction");
                    const double k = 1.0 - *eta * *eta * (1.0 - dot * dot);
                    for (std::size_t i = 0; i < a->size(); ++i) made.emplace_back(k < 0.0 ? 0.0 : *eta * (*a)[i] - (*eta * dot + std::sqrt(k)) * (*b)[i]);
                }
                result = Value(std::move(made));
            } else if (type == "View") {
                return BuildView(inputs, out);
            } else if (type == "Perspective" || type == "Orthographic" || type == "Frustum" || type == "Oblique") {
                return BuildLens(node, inputs, out);
            } else if (type == "Paraboloid") {
                return BuildParaboloid(inputs, out);
            } else if (type == "CubeFace") {
                return BuildCubeFace(inputs, out);
            } else if (type == "ShadowCascades") {
                return BuildCascades(inputs, out);
            } else if (type == "PointShadows") {
                return BuildPointShadows(node, inputs, out);
            } else if (type == "SpotShadows") {
                return BuildSpotShadows(node, inputs, out);
            } else if (type == "ImageExtent") {
                const auto it = inputs.find("image");
                const auto image = it == inputs.end() ? kor::ResourceRef<const kor::Image>() : ImageRefOf(it->second.resource.get());
                if (!image.Get()) return Fail("'image' needs an image wired in");
                const auto extent = image->Extent();
                result = Value(Value::List { extent.x, extent.y, extent.z });
            } else if (type == "VertexLayout") {
                auto layout = LayoutValue(value("attributes"));
                if (!layout) return Fail(layout.error());
                result = std::move(*layout);
            } else if (type == "Length" || type == "Normalize") {
                const auto a = NumbersOf(value("a"), "a");
                if (!a) return Fail(a.error());
                double squared = 0.0;
                for (const double n : *a) squared += n * n;
                const double length = std::sqrt(squared);
                if (type == "Length") { result = Value(length); return {}; }
                Value::List unit;
                for (const double n : *a) unit.emplace_back(length > 0.0 ? n / length : 0.0);
                result = Value(std::move(unit));
            } else if (type == "Dot" || type == "Distance" || type == "Cross") {
                const auto a = NumbersOf(value("a"), "a");
                if (!a) return Fail(a.error());
                const auto b = NumbersOf(value("b"), "b");
                if (!b) return Fail(b.error());
                if (a->size() != b->size()) return Fail(std::format("a vector of {} and one of {} do not go together", a->size(), b->size()));
                if (type == "Cross") {
                    if (a->size() != 3) return Fail("a cross product is of two vectors of 3");
                    const auto& u = *a;
                    const auto& v = *b;
                    result = Value(Value::List { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] });
                    return {};
                }
                double sum = 0.0;
                for (std::size_t i = 0; i < a->size(); ++i) sum += type == "Dot" ? (*a)[i] * (*b)[i] : ((*a)[i] - (*b)[i]) * ((*a)[i] - (*b)[i]);
                result = Value(type == "Dot" ? sum : std::sqrt(sum));
            } else if (type == "Equal" || type == "NotEqual") {
                const auto a = given("a");
                if (!a) return Fail(a.error());
                const auto b = given("b");
                if (!b) return Fail(b.error());
                result = Value((**a == **b) == (type == "Equal"));
            } else if (type == "Less" || type == "LessOrEqual" || type == "Greater" || type == "GreaterOrEqual") {
                const auto compare = [&](const double a, const double b) {
                    return type == "Less" ? a < b : type == "LessOrEqual" ? a <= b : type == "Greater" ? a > b : a >= b;
                };
                const double* a = value("a").Number();
                const double* b = value("b").Number();
                if (a && b) {
                    result = Value(compare(*a, *b));
                } else {
                    // Of vectors, a component at a time; a number goes with every component of a vector.
                    const auto as = NumbersOf(value("a"), "a");
                    if (!as) return Fail(as.error());
                    const auto bs = NumbersOf(value("b"), "b");
                    if (!bs) return Fail(bs.error());
                    if (as->size() != bs->size() && as->size() != 1 && bs->size() != 1)
                        return Fail(std::format("'a' has {} components and 'b' {}: they are compared a component at a time", as->size(), bs->size()));
                    Value::List each;
                    for (std::size_t i = 0; i < std::max(as->size(), bs->size()); ++i)
                        each.emplace_back(compare((*as)[as->size() == 1 ? 0 : i], (*bs)[bs->size() == 1 ? 0 : i]));
                    result = Value(std::move(each));
                }
            } else if (type == "Any" || type == "All") {
                // A condition for each component, as a comparison of vectors gives it: true or false, or numbers not 0.
                const Value& a = value("a");
                std::vector<bool> each;
                if (const bool* one = a.Bool()) each = { *one };
                else if (const double* n = a.Number()) each = { *n != 0.0 };
                else if (const auto* items = a.Items()) {
                    for (const Value& item : *items) {
                        if (const bool* flag = item.Bool()) each.push_back(*flag);
                        else if (const double* n = item.Number()) each.push_back(*n != 0.0);
                        else return Fail("'a' is true or false, or a list of them");
                    }
                } else {
                    return Fail("'a' is true or false, or a list of them");
                }
                result = Value(type == "Any" ? std::ranges::any_of(each, std::identity {}) : std::ranges::all_of(each, std::identity {}));
            } else if (type == "Not") {
                const bool* a = value("a").Bool();
                if (!a) return Fail("'a' is true or false");
                result = Value(!*a);
            } else if (type == "And" || type == "Or" || type == "Xor") {
                const bool* a = value("a").Bool();
                const bool* b = value("b").Bool();
                if (!a || !b) return Fail("'a' and 'b' are each true or false");
                result = Value(type == "And" ? *a && *b : type == "Or" ? *a || *b : *a != *b);
            } else if (type == "Select") {
                if (const auto* conditions = value("condition").Items()) {
                    // A condition for each component: each component of a or of b, as its own says.
                    const auto as = NumbersOf(value("a"), "a");
                    if (!as) return Fail(as.error());
                    const auto bs = NumbersOf(value("b"), "b");
                    if (!bs) return Fail(bs.error());
                    Value::List chosen;
                    for (std::size_t i = 0; i < conditions->size(); ++i) {
                        const Value& c = (*conditions)[i];
                        const bool* flag = c.Bool();
                        const double* n = c.Number();
                        if (!flag && !n) return Fail("'condition' is true or false, or a list of them");
                        const bool pick = flag ? *flag : *n != 0.0;
                        const std::vector<double>& from = pick ? *as : *bs;
                        if (from.size() != 1 && from.size() <= i) return Fail(std::format("'condition' has {} components, and '{}' fewer", conditions->size(), pick ? "a" : "b"));
                        chosen.emplace_back(from[from.size() == 1 ? 0 : i]);
                    }
                    result = Value(std::move(chosen));
                    return {};
                }
                const bool* condition = value("condition").Bool();
                const double* number = value("condition").Number();
                if (!condition && !number) return Fail("'condition' is true or false");
                const auto chosen = given((condition ? *condition : *number != 0.0) ? "a" : "b");
                if (!chosen) return Fail(chosen.error());
                result = **chosen;
            } else if (type == "ColorConvert") {
                return BuildColorConvert(inputs, out);
            } else if (type.starts_with("Quat")) {
                return BuildQuaternion(type, inputs, out);
            } else if (type == "Jitter") {
                return BuildJitter(inputs, out);
            } else if (type == "DeviceSupports") {
                const std::string* feature = value("feature").String();
                if (!feature) return Fail("'feature' is what is asked of the device: eMeshShader, eRayTracing...");
                static const std::map<std::string, kor::Feature, std::less<>> features {
                    { "eMeshShader", kor::Feature::eMeshShader }, { "eRayTracing", kor::Feature::eRayTracing }, { "eShaderFloat64", kor::Feature::eShaderFloat64 },
                    { "eShaderInt64", kor::Feature::eShaderInt64 }, { "eShaderInt16", kor::Feature::eShaderInt16 }, { "eShaderFloat16", kor::Feature::eShaderFloat16 },
                    { "eInt64Atomics", kor::Feature::eInt64Atomics }, { "eAtomicFloat32", kor::Feature::eAtomicFloat32 }, { "eAtomicFloat64", kor::Feature::eAtomicFloat64 },
                    { "eMultiDrawIndirect", kor::Feature::eMultiDrawIndirect }, { "eDrawIndirectCount", kor::Feature::eDrawIndirectCount },
                    { "eGeometryShader", kor::Feature::eGeometryShader }, { "eTessellationShader", kor::Feature::eTessellationShader },
                    { "eFillModeNonSolid", kor::Feature::eFillModeNonSolid }, { "eWideLines", kor::Feature::eWideLines }, { "eSamplerAnisotropy", kor::Feature::eSamplerAnisotropy } };
                if (*feature == "eAsyncCompute") { result = Value(kor::Context::SupportsAsyncCompute()); return {}; }
                const auto it = features.find(*feature);
                if (it == features.end()) return Fail(std::format("there is no feature '{}'", *feature));
                result = Value(kor::Context::Supports(it->second));
            } else if (const auto operation = Operations().find(type); operation != Operations().end()) {
                // Each is named for what it is: a clamp's are x, min and max; a smoothstep's two edges and x.
                static const std::map<std::string, std::array<const char*, 5>, std::less<>> named {
                    { "Clamp", { "x", "min", "max" } }, { "Smoothstep", { "edge0", "edge1", "x" } }, { "Step", { "edge", "x" } },
                    { "Atan2", { "y", "x" } }, { "Lerp", { "a", "b", "t" } }, { "InverseLerp", { "a", "b", "value" } },
                    { "Remap", { "value", "fromMin", "fromMax", "toMin", "toMax" } } };
                static const std::array<const char*, 5> plain { "a", "b", "c" };
                const auto own = named.find(type);
                const int count = operation->second.first;
                std::vector<const Value*> arguments;
                for (int i = 0; i < count; ++i) {
                    const char* name = (own == named.end() ? plain : own->second)[static_cast<std::size_t>(i)];
                    const auto argument = given(name);
                    if (!argument) return Fail(argument.error());
                    arguments.push_back(*argument);
                }
                auto worked = Each(arguments, operation->second.second);
                if (!worked) return Fail(worked.error());
                result = std::move(*worked);
            } else {
                return Fail(std::format("there is no way to work out a {}", type));
            }
            return {};
        }

    auto Runtime::Impl::DefaultTangents() const -> Tangents
    { return source ? source->DefaultTangents() : defaultMeshTangents; }

    auto Runtime::Impl::DefaultLayout(const Tangents tangents) -> kor::VertexLayout
    {
            std::vector<std::pair<const char*, kor::u32>> attributes { { "POSITION", 3 }, { "NORMAL", 3 } };
            if (tangents == Tangents::eFour) attributes.emplace_back("TANGENT", 4);
            if (tangents == Tangents::eWithBitangent) attributes.insert(attributes.end(), { { "TANGENT", 3 }, { "BITANGENT", 3 } });
            attributes.emplace_back("UV", 2);
            kor::VertexLayout layout;
            kor::u32 offset = 0;
            for (const auto& [semantic, count] : attributes) {
                layout.attributes.emplace_back(semantic, "", 0u, offset * 4u, kor::ChannelType::eFloat, count);
                offset += count;
            }
            layout.bindings.emplace_back(0u, offset * 4u);
            return layout;
        }

    auto Runtime::Impl::LayoutKey(const VertexData& data) -> std::string
    {
            std::string key;
            for (const auto& attribute : data.attributes) key += std::format("{}:{}:{};", attribute.semantic, static_cast<int>(attribute.type), attribute.count);
            for (const auto& buffer : data.buffers) {
                key += "|";
                for (const auto& semantic : buffer) key += semantic + ",";
            }
            return key;
        }

    auto Runtime::Impl::ChannelOf(const AttributeType type) -> kor::ChannelType
    {
            return type == AttributeType::eInt ? kor::ChannelType::eInt : type == AttributeType::eUInt ? kor::ChannelType::eUInt : kor::ChannelType::eFloat;
        }

    auto Runtime::Impl::MeshOf(const VertexData& data) -> Expected<Held>
    { return MeshOf(Interleave(data)); }

    auto Runtime::Impl::Interleave(const VertexData& data) -> Interleaved
    {
            Interleaved out;
            if (out.problem = data.Problem(); !out.problem.empty()) return out;
            std::vector<std::vector<const VertexAttribute*>> groups;
            std::set<std::string_view> placed;
            for (const auto& buffer : data.buffers) {
                std::vector<const VertexAttribute*> group;
                for (const auto& semantic : buffer)
                    if (const VertexAttribute* attribute = data.Find(semantic); attribute && placed.insert(attribute->semantic).second) group.push_back(attribute);
                if (!group.empty()) groups.push_back(std::move(group));
            }
            std::vector<const VertexAttribute*> rest;
            for (const auto& attribute : data.attributes) if (!placed.contains(attribute.semantic)) rest.push_back(&attribute);
            if (!rest.empty()) groups.push_back(std::move(rest));

            for (std::size_t b = 0; b < groups.size(); ++b) {
                std::uint32_t stride = 0;
                for (const auto* attribute : groups[b]) stride += attribute->count;
                std::vector<std::uint32_t> words(static_cast<std::size_t>(stride) * data.vertexCount);
                std::uint32_t offset = 0;
                for (const auto* attribute : groups[b]) {
                    for (std::uint32_t v = 0; v < data.vertexCount; ++v)
                        std::copy_n(attribute->words.begin() + static_cast<std::ptrdiff_t>(v) * attribute->count, attribute->count,
                                    words.begin() + static_cast<std::ptrdiff_t>(v) * stride + offset);
                    out.layout.attributes.emplace_back(attribute->semantic, "", static_cast<kor::u32>(b), offset * 4u, ChannelOf(attribute->type), attribute->count);
                    offset += attribute->count;
                }
                out.layout.bindings.emplace_back(static_cast<kor::u32>(b), stride * 4u);
                out.buffers.push_back(std::move(words));
            }
            out.indices = data.indices;
            Resource box;
            Bound(box, data, 0, data.vertexCount);
            std::copy_n(box.bounds, 6, out.bounds);
            out.bounded = box.bounded;
            return out;
        }

    auto Runtime::Impl::MeshOf(Interleaved ready) -> Expected<Held>
    {
            if (!ready.problem.empty()) return Fail(ready.problem);
            kor::Mesh::Builder builder;
            std::vector<Held> buffers;
            for (std::size_t b = 0; b < ready.buffers.size(); ++b) {
                // Each buffer a resource of its own, the mesh referring to it: what a compute shader reads of the mesh.
                auto buffer = kor::Mesh::MakeBuffer(ready.buffers[b], kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eVertex) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc);
                builder.SetVertexBuffer(static_cast<kor::u32>(b), kor::ResourceRef<kor::Buffer>(buffer));
                buffers.push_back(MakeResource(std::move(buffer)));
            }
            if (!ready.indices.empty()) {
                auto buffer = kor::Mesh::MakeBuffer(ready.indices, kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eIndex) | kor::Buffer::Usage::eStorage | kor::Buffer::Usage::eTransferSrc);
                builder.SetIndexBuffer(kor::ResourceRef<kor::Buffer>(buffer));
                buffers.push_back(MakeResource(std::move(buffer)));
            }
            auto mesh = builder.SetVertexLayout(std::move(ready.layout)).Build();
            if (!mesh.Valid()) return Fail(mesh.Failure() ? mesh.Failure()->message : std::string("the mesh could not be built"));
            Held held = MakeResource(std::move(mesh));
            held->buffers = std::move(buffers);
            std::copy_n(ready.bounds, 6, held->bounds);
            held->bounded = ready.bounded;
            return held;
        }

    auto Runtime::Impl::Bound(Resource& held, const VertexData& data, const std::uint32_t first, const std::uint32_t count) -> void
    {
            const VertexAttribute* position = data.Find("POSITION");
            if (!position || count == 0) return;
            for (int axis = 0; axis < 3; ++axis) { held.bounds[axis] = INFINITY; held.bounds[axis + 3] = -INFINITY; }
            for (std::uint32_t v = first; v < first + count; ++v)
                for (int axis = 0; axis < static_cast<int>(std::min(position->count, 3u)); ++axis) {
                    const float at = std::bit_cast<float>(position->words[static_cast<std::size_t>(v) * position->count + static_cast<std::size_t>(axis)]);
                    held.bounds[axis] = std::min(held.bounds[axis], at);
                    held.bounds[axis + 3] = std::max(held.bounds[axis + 3], at);
                }
            if (position->count < 3) held.bounds[2] = held.bounds[5] = 0.f;
            held.bounded = true;
        }

    auto Runtime::Impl::DefaultMesh(const std::string_view asked) -> Held
    {
            if (source) return source->DefaultMesh(asked);
            const std::string name = ShapeName(asked, ShapeValues(asked));      // however it was written
            if (name.empty()) return nullptr;
            if (const auto it = defaultMeshes.find(name); it != defaultMeshes.end()) return it->second;
            const auto made = MeshOf(ToVertexData(*ShapeData(name), defaultMeshTangents));
            if (!made) return nullptr;
            (*made)->node = DefaultMeshId(name);   // what it is saved as, and found again by
            return defaultMeshes.emplace(name, *made).first->second;
        }
}
