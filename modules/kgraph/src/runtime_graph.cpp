// kgraph: the runtime's own, the whole graph: evaluated, ordered, and put in the frame graph.

#include "runtime_impl.h"

namespace kgraph
{
    auto Runtime::Impl::Evaluate(const Graph& graph) -> void
    {
            // Nodes that are gone take what they made with them.
            for (auto it = built.begin(); it != built.end();) {
                if (graph.Find(it->first)) { ++it; continue; }
                Unwatch(it->second);
                Retire(std::move(it->second));
                it = built.erase(it);
            }

            current = &graph;
            ReadFromReferences(graph);
            restsBefore.clear();
            unreadBefore.clear();
            currentById.clear();
            currentById.reserve(graph.nodes.size());
            for (const Node& each : graph.nodes) currentById.emplace(each.id, &each);
            // What is used as what, by nodes that are gone, is not.
            for (auto it = usedAsBy.begin(); it != usedAsBy.end();) {
                std::erase_if(it->second, [&](const auto& by) { return by.first != EntitiesUse && !currentById.contains(by.first); });
                it = currentById.contains(it->first) ? std::next(it) : usedAsBy.erase(it);
            }
            const Using using_(*this, std::string(EntitiesUse));
            struct Evaluating { bool& now; bool was; ~Evaluating() { now = was; } } const evaluatingNow { evaluating, std::exchange(evaluating, true) };
            takenByPipeline.reset();
            // What each node waits for: what it is wired to, and what it stands for.
            std::map<std::string, std::vector<std::string>> needs;
            for (const auto& node : graph.nodes) {
                auto& list = needs[node.id];
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link) list.push_back(input.link->node);
                if (node.type == "Reference")
                    if (const auto it = node.inputs.find("node"); it != node.inputs.end() && it->second.value.String()) list.push_back(*it->second.value.String());
                // A ForEach of a pipeline's component — of it, or `with` it — is after the pipeline that makes the component.
                if (IsEach(node.type)) {
                    std::vector<std::string> named;
                    if (const auto it = node.inputs.find("component"); it != node.inputs.end() && it->second.value.String()) named.push_back(*it->second.value.String());
                    if (const auto it = node.inputs.find("with"); it != node.inputs.end() && it->second.value.Items())
                        for (const auto& each : *it->second.value.Items()) if (each.String()) named.push_back(*each.String());
                    for (const auto& name : named)
                        if (const Node* other = graph.Find(name); other && other->id != node.id && IsPipeline(other->type)) list.push_back(name);
                }
            }
            std::vector<const Node*> order;
            std::set<std::string> done, visiting, circular;
            const std::function<void(const Node&)> visit = [&](const Node& node) {
                if (done.contains(node.id)) return;
                if (!visiting.insert(node.id).second) { circular.insert(node.id); return; }
                for (const auto& other : needs[node.id])
                    if (const Node* found = graph.Find(other)) visit(*found);
                visiting.erase(node.id);
                done.insert(node.id);
                order.push_back(&node);
            };
            for (const auto& node : graph.nodes) visit(node);

            for (const Node* node : order) {
                Built& out = built[node->id];
                const auto fail = [&](std::string why) {
                    // Failed for another reason than last time, or for the first: what was built is let go of.
                    Unwatch(out);
                    Retire(Built { .outputs = std::move(out.outputs), .pipeline = out.pipeline, .pass = std::move(out.pass) });
                    out.outputs.clear();
                    out.pass.reset();
                    out.status.ok = false;
                    out.status.error = std::move(why);
                    out.signature.clear();
                    out.stamp = ++counter;
                };
                if (circular.contains(node->id)) { fail("it is wired, through other nodes, to itself"); continue; }

                // What goes into it, and a signature of that: its own values, and for each wire which build of what.
                Inputs inputs;
                std::string signature = node->type;
                std::string blocked;
                for (const auto& [name, input] : node->inputs) {
                    Resolved resolved;
                    if (input.link) {
                        resolved.linked = true;
                        const auto source = built.find(input.link->node);
                        if (!graph.Find(input.link->node) || source == built.end()) { blocked = std::format("'{}' is wired to '{}', which is not in the graph", name, input.link->node); break; }
                        if (!source->second.status.ok) { blocked = std::format("'{}' comes from '{}', which failed", name, input.link->node); break; }
                        const auto output = source->second.outputs.find(input.link->output);
                        if (output == source->second.outputs.end()) { blocked = std::format("'{}' has no output '{}'", input.link->node, input.link->output); break; }
                        resolved.value = output->second.value;
                        resolved.resource = output->second.resource;
                        signature += std::format("|{}=@{}.{}#{}", name, input.link->node, input.link->output, source->second.stamp);
                    } else {
                        resolved.value = input.value;
                        signature += std::format("|{}={}", name, input.value.ToJson());
                    }
                    inputs.emplace(name, std::move(resolved));
                }
                if (node->type == "Screen") signature += std::format("|{}x{}", screenExtent.x, screenExtent.y);
                // A Random is of the shape of what it is wired to: drawn again when that is another.
                if (random::DistributionOfNode(node->type)) {
                    const Shape shape = RandomShape(*node);
                    randomShapes[node->id] = shape;
                    signature += "|as " + ShapeName(shape);
                }
                // An Image or a Buffer is made again when it is used as what it was neither given nor made for, or no longer is.
                if (MadeForItsUse(node->type)) signature += std::format("|for {}", UsageFor(node->id) & ~GivenUsage(node->type, inputs));
                // What the view looks at the scene by: built again when that changes.
                if (node->type == "DebugView" && node->id == debugView) signature += "|shown";
                if (node->inputs.contains("views")) signature += editorView ? "|editor view" : "|game view";
                if (shaderDebug != 0 && out.pipeline && std::ranges::find(out.pipeline->ports, std::format("constant.{}", ShaderDebugConstant), &Port::name) != out.pipeline->ports.end())
                    signature += std::format("|{}={}", ShaderDebugConstant, shaderDebug);
                // A Script node is the script as it was set: made again when the script is set again — edited, loaded, run.
                if (node->type == "Script")
                    if (const auto it = node->inputs.find("script"); it != node->inputs.end() && it->second.value.String())
                        if (const Scripted* script = ScriptNamed(*it->second.value.String())) signature += std::format("|script#{}", script->generation);
                // A ForEach gives what its components hold: made again when one of them is another shape — a pipeline's, reshaped just now.
                if (IsEach(node->type) && world) {
                    const auto shape = [&](const std::string& name) {
                        if (const kecs::ComponentId id = world->Find(name); id != kecs::NoComponent)
                            for (const auto& field : world->Describe(id).fields) signature += std::format("|{}.{}:{}@{}", name, field.name, field.type, field.offset);
                    };
                    if (const auto it = node->inputs.find("component"); it != node->inputs.end() && it->second.value.String()) shape(*it->second.value.String());
                    if (const auto it = node->inputs.find("with"); it != node->inputs.end() && it->second.value.Items())
                        for (const auto& each : *it->second.value.Items()) if (each.String()) shape(*each.String());
                }
                // A pipeline's component is what its draws take from nobody else: made again when that changes.
                if (IsPipeline(node->type))
                    for (const auto& name : TakenElsewhere(node->id)) signature += "|taken:" + name;
                if (node->type == "Reference")
                    if (const auto it = node->inputs.find("node"); it != node->inputs.end() && it->second.value.String())
                        if (const auto target = built.find(*it->second.value.String()); target != built.end()) signature += std::format("|#{}", target->second.stamp);
                // A buffer the source's is shared: the one it built, taken again whenever it builds another.
                const Built* theirs = source && node->type == "Buffer" && shared.contains(node->id) ? source->Find(node->id) : nullptr;
                if (theirs && !theirs->status.ok) theirs = nullptr;
                if (theirs) signature += std::format("|shared#{}", theirs->stamp);
                if (!blocked.empty()) { fail(std::move(blocked)); continue; }
                if (out.status.ok && out.signature == signature) continue;   // nothing that goes into it changed
                if (const auto parts = InParts(*node, inputs); !parts) { fail(parts.error()); continue; }
                FillDefaults(*node, inputs);

                Unwatch(out);
                Built fresh;
                fresh.status.builds = out.status.builds + 1;
                if (theirs) fresh.outputs = theirs->outputs;
                // What it uses resources as is told again as it is built: what it no longer uses one as, it is not made for.
                for (auto& by : usedAsBy | std::views::values) by.erase(node->id);
                const auto result = [&] {
                    const Using using_(*this, node->id);
                    return theirs ? Expected<void> {} : Build(*node, inputs, fresh);
                }();
                if (!result) {
                    const auto pipeline = fresh.pipeline;   // its inputs are still worth knowing
                    Unwatch(fresh);
                    fail(result.error());
                    out.status.builds = fresh.status.builds;
                    out.pipeline = pipeline;
                    out.ports = std::move(fresh.ports);
                    out.gives = std::move(fresh.gives);
                    continue;
                }
                AddOutputs(*node, inputs, fresh);
                fresh.status.ok = true;
                if (const auto note = BundleNote(*node, fresh); !note.empty()) fresh.status.note = note;
                if (const auto why = unbatched.find(node->id); why != unbatched.end()) fresh.status.note = why->second;
                if (batched.contains(node->id) && fresh.pass && fresh.pass->entities && fresh.pass->entities->instances.empty())
                    fresh.status.note = "batched, and its shader has no [entity(\"PER_INSTANCE\")] buffer: every element is drawn with the same push constants";
                fresh.signature = std::move(signature);
                if (IsMadeResource(node->type)) fresh.madeWith = WiredValues(*node);
                fresh.stamp = ++counter;
                if (!fresh.migration) fresh.migration = std::move(out.migration);
                // What it made before is not what it makes now: an entity holding the old one is led to the new.
                for (const auto& [name, output] : out.outputs) {
                    if (!output.resource || output.resource->node != node->id) continue;
                    const auto now = fresh.outputs.find(name);
                    if (now != fresh.outputs.end() && now->second.resource && now->second.resource != output.resource) { output.resource->replacedBy = now->second.resource; ++replacements; }
                }
                Retire(std::move(out));
                out = std::move(fresh);
            }
            // An Image or a Buffer found used as what it was not made for, or no longer used as what it was: made again
            // now, and what uses it, so that nothing waits a frame on it. Each time round what it is used as only grows,
            // unless what uses it is built otherwise; a few times round is all it takes.
            const auto remade = [&](const Node* node) {
                const auto it = built.find(node->id);
                return MadeForItsUse(node->type) && !shared.contains(node->id) && it != built.end() && it->second.status.ok && it->second.madeFor != (it->second.givenUsage | UsageFor(node->id));
            };
            // A Random built before what it is wired to said what it takes — a pipeline built after it — is drawn again, as that.
            const auto reshaped = [&](const Node* node) {
                if (!random::DistributionOfNode(node->type)) return false;
                const auto was = randomShapes.find(node->id);
                return was != randomShapes.end() && was->second != RandomShape(*node);
            };
            if (remadeRounds < 4 && (std::ranges::any_of(order, remade) || std::ranges::any_of(order, reshaped))) {
                ++remadeRounds;
                Evaluate(graph);
                --remadeRounds;
                return;
            }

            // What follows the world from here on, each frame: the nodes that read it, and what is worked out of them.
            live.clear();
            sized.clear();
            std::set<std::string> following;
            for (const Node* node : order) {
                const NodeType* type = FindType(node->type);
                // The main view follows whoever runs the graph: an editor's view moves every frame.
                const auto main = node->inputs.find("main");
                const bool lens = node->type == "View" || node->type == "Perspective" || node->type == "Orthographic" || node->type == "Frustum";
                bool follows = ReadsTheWorld(node->type) || (lens && main != node->inputs.end() && main->second.value.Bool() && *main->second.value.Bool());
                const bool carries = type && !type->builder && (IsMapped(node->type) || node->type == "Reference"
                                                                || node->type == "MeshPack" || node->type == "IndirectDraws"
                                                                || (type->category != "Passes" && type->category != "Resources" && type->category != "Scene"));
                const bool given = std::ranges::any_of(needs[node->id], [&](const std::string& other) { return following.contains(other); });
                if (!follows && carries) follows = given;
                // A buffer or an image is made once, of what it is given then: one given what follows the world — a count
                // of entities — is watched, and made again when that changes.
                if (!follows && given && IsMadeResource(node->type)) sized.push_back(node->id);
                if (!follows) continue;
                following.insert(node->id);
                live.push_back(node->id);
            }
            // What follows the world for setup alone, from the buffers on setup pages back: what nothing but those
            // takes. Thousands of entities each worked out every frame, for a buffer setup copies now and then, is
            // most of a frame.
            setupOnly.clear();
            {
                std::map<std::string, std::vector<std::string>, std::less<>> takers;
                for (const auto& [id, list] : needs)
                    for (const auto& other : list) takers[other].push_back(id);
                for (auto it = live.rbegin(); it != live.rend(); ++it) {
                    const Node* node = graph.Find(*it);
                    if (!node) continue;
                    const bool laidOut = IsMapped(node->type);
                    const auto taken = takers.find(*it);
                    const bool forSetup = laidOut ? graph.IsSetup(node->pass)
                        : taken != takers.end() && !taken->second.empty()
                          && std::ranges::all_of(taken->second, [&](const std::string& taker) { return setupOnly.contains(taker); });
                    if (forSetup) setupOnly.insert(*it);
                }
            }
            structureSeen = ~0ull;
            pointShadowsDrawn.clear();
            Install(graph);
        }

    auto Runtime::Impl::PackPush(const PushMember& push, const Value& value, std::byte* at) -> void
    {
            std::vector<double> numbers;
            if (const bool* flag = value.Bool()) numbers = { *flag ? 1.0 : 0.0 };
            else if (auto read = NumbersOf(value, "push")) numbers = std::move(*read);
            const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(push.rows) * push.columns, push.size / 4);
            const std::size_t stride = push.columns > 1 && push.size >= push.columns * 16 && push.rows < 4 ? 16 : push.rows * 4;     // a matrix's columns
            for (std::size_t i = 0; i < count; ++i) {
                const double number = i < numbers.size() ? numbers[i] : 0.0;
                std::byte* to = at + (push.columns > 1 ? (i / push.rows) * stride + (i % push.rows) * 4 : i * 4);
                if (push.scalar == 0) { const auto v = static_cast<float>(number); std::memcpy(to, &v, 4); }
                else if (push.scalar == 1) { const auto v = static_cast<std::int32_t>(std::llround(number)); std::memcpy(to, &v, 4); }
                else { const auto v = static_cast<std::uint32_t>(std::llround(std::max(number, 0.0))); std::memcpy(to, &v, 4); }
            }
        }

    auto Runtime::Impl::Lacks(const Source& source) -> std::string
    {
            return std::format("'{}' has nothing in it", source.kind == Source::Kind::eField ? source.field.name : source.input);
        }

    auto Runtime::Impl::InstancesFor(const Node& draw, const PipelineInfo& info, const Slot& slot, EntityRun& run, const Node* loop, std::string* missing) -> Expected<Source>
    {
            const std::string port = "param." + slot.field;
            if (!world) return Fail(std::format("'{}' is each instance's, and there is no world for there to be instances of", port));
            // Drawn an element of a ForEach at a time, of the same ForEach's instances: each draw reads its own element.
            if (loop && !run.each.empty() && run.each == loop->id) run.instancedRows = true;
            else if (!run.each.empty())
                return Fail(loop ? std::format("'{}' is '{}''s instances, and this draws one element of '{}' at a time: wire the instances of the loop it draws", port, loop->id, run.each)
                            : draw.type == "Dispatch" ? std::format("'{}' is each instance's, and this runs once an element of '{}': wire 'entities' a Filter's instead, for one dispatch of them all, or wire it", port, run.each)
                                                      : std::format("'{}' is each instance's, and this draws one element of '{}' at a time: wire '{}''s instances to it, set 'batched', or draw a MeshPack's mesh", port, run.each, run.each));
            const bool rays = draw.type == "TraceRays" || draw.type == "Dispatch";     // of the Filter wired to `entities`
            const Node* filter = loop;
            if (!loop && rays) {
                if (const auto entities = draw.inputs.find("entities"); entities != draw.inputs.end() && entities->second.link) filter = Origin(entities->second.link->node);
            } else if (!loop) {
                const auto mesh = draw.inputs.find("mesh");
                const Node* pack = mesh != draw.inputs.end() && mesh->second.link ? Origin(mesh->second.link->node) : nullptr;
                if (pack && pack->type == "MeshPack")
                    if (const auto meshes = pack->inputs.find("meshes"); meshes != pack->inputs.end() && meshes->second.link) filter = Origin(meshes->second.link->node);
            }
            const auto found = filter ? built.find(filter->id) : built.end();
            if (loop && (found == built.end() || !found->second.status.ok || !found->second.listed)) return Fail(std::format("'{}', whose instances fill '{}', is not built", loop->id, port));
            if (found == built.end() || !found->second.status.ok || !found->second.listed)
                return Fail(rays ? std::format("'{}' is each instance's: they are the entities wired to 'entities', a Filter's — or wire a ForEach's or a Filter's instances to it", port)
                                 : std::format("'{}' is each instance's: wire a ForEach's or a Filter's instances to it — or draw the mesh of a MeshPack of a Filter's Mesh.mesh, whose entities it then is", port));

            // Of a loop wired: filled from what its entities are sure to have — its components, and `with`'s, of every
            // Filter it keeps some of — as best it can be. A field the shader does not mark is the same-named field of one
            // of them; a material's, of a kind none of them is, is said with the rest none of them has.
            std::shared_ptr<Instancing> instancing = slot.instancing;
            if (loop) {
                std::vector<kecs::ComponentId> has;
                for (const Node* at = loop; at; ) {
                    const auto add = [&](const Value& name) { if (const std::string* text = name.String(); text && !text->empty()) if (const kecs::ComponentId id = world->Find(*text); id != kecs::NoComponent && !std::ranges::contains(has, id)) has.push_back(id); };
                    if (const auto component = at->inputs.find("component"); component != at->inputs.end()) add(component->second.value);
                    if (const auto with = at->inputs.find("with"); with != at->inputs.end() && with->second.value.Items())
                        for (const auto& name : *with->second.value.Items()) add(name);
                    const auto upstream = at->inputs.find("entities");
                    at = upstream != at->inputs.end() && upstream->second.link ? Origin(upstream->second.link->node) : nullptr;
                }
                // A component is of the kinds it derives from too: a PointLight is a Light.
                const auto kindOf = [&](const kecs::ComponentId id) {
                    for (const kecs::ComponentId one : has) {
                        if (one == id) return true;
                        if (std::ranges::contains(world->Describe(one).kinds, world->Describe(id).name)) return true;
                    }
                    return false;
                };
                auto filled = std::make_shared<Instancing>(*slot.instancing);
                std::string lacks;
                const auto lack = [&](const std::string& what) { lacks += std::format("{}{}", lacks.empty() ? "" : ", ", what); };
                for (auto& field : filled->fields) {
                    using From = Instancing::Field::From;
                    if (field.from == From::eComponent || field.from == From::eTexture) {
                        const kecs::FieldDescription* match = nullptr;
                        kecs::ComponentId of = kecs::NoComponent;
                        for (const kecs::ComponentId one : has)
                            if (const kecs::FieldDescription* named = world->Describe(one).FindField(field.name)) {
                                const bool picture = named->type == "Image";
                                if (field.from == From::eTexture ? picture : !picture && MaterialFieldMisfit(*named, field.type).empty()) { match = named; of = one; break; }
                            }
                        if (!match) { lack(std::format("{} ({})", field.name, field.from == From::eTexture ? "a picture" : field.type)); continue; }
                        field.from = field.from == From::eTexture ? From::eMaterialTexture : From::eMaterial;
                        field.kind = world->Describe(of).name;
                        field.source = *match;
                        field.at = match->offset;
                        field.component = of;
                    } else if (field.Material() && (field.component == kecs::NoComponent || !kindOf(field.component))) {
                        lack(std::format("{}.{} (only an entity with a {} is drawn)", field.kind, field.source.name, field.kind));
                    }
                }
                if (missing) *missing = std::move(lacks);
                instancing = std::move(filled);
            }

            // Another Draw's, when it would write the same: one buffer, written once a frame, for all of them.
            const std::string key = InstancesKey(filter->id, *instancing, info.component);
            std::erase_if(sharedInstances, [](const auto& entry) { return entry.second.expired(); });
            // A runtime that follows another — an editor's Game panel, the same graph over the same world — has the one
            // that runtime made: the same entities, written once a frame for both.
            std::shared_ptr<Instances> theirs;
            for (const Impl* owner : { source, this })
                if (owner && !theirs)
                    if (const auto shared = owner->sharedInstances.find(key); shared != owner->sharedInstances.end())
                        if (auto found = shared->second.lock(); found && !found->full) theirs = std::move(found);
            // Compacted: what its shader reads is another buffer as big, of what is in sight one after another, which a
            // cull before it writes (CullInSight's `drawn`); the buffer of instances is filled all the same, for the cull.
            const auto compacted = draw.inputs.find("compacted");
            const bool compacts = !rays && compacted != draw.inputs.end() && compacted->second.value.Bool() && *compacted->second.value.Bool();
            const auto bound = [&](const Instances& instances) -> Expected<Source> {
                Source source;
                source.kind = Source::Kind::eWired;
                source.input = port;
                source.resource = instances.held;
                if (compacts) {
                    auto drawn = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(instances.capacity * instances.instancing->stride))
                        .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc | kor::Buffer::Usage::eTransferDst)
                        .SetType(kor::Buffer::Type::eDeviceLocal).Build();
                    if (!drawn.Valid()) return Fail(drawn.Failure() ? drawn.Failure()->message : std::format("'{}' could not be made a buffer of what is in sight", port));
                    run.drawn = MakeResource(std::move(drawn));
                    run.drawn->node = draw.id;
                    source.resource = run.drawn;
                }
                return source;
            };
            if (theirs) {
                if (std::ranges::find(theirs->draws, draw.id) == theirs->draws.end()) theirs->draws.push_back(draw.id);
                run.instances.push_back(theirs);
                return bound(*theirs);
            }

            auto instances = std::make_shared<Instances>();
            instances->instancing = instancing;
            instances->source = filter->id;
            instances->draws = { draw.id };
            instances->component = info.component;
            for (const auto& field : instancing->fields)
                if (field.Material() && field.component != kecs::NoComponent && !std::ranges::contains(instances->materials, field.component))
                    instances->materials.push_back(field.component);
            instances->clock = frames;
            instances->capacity = std::bit_ceil(std::max<std::size_t>(found->second.listed->Rows(), 64));
            auto buffer = kor::Buffer::RawBuilder {}.SetRawSize(static_cast<kor::i64>(instances->capacity * instancing->stride))
                .SetUsage(kor::Flags<kor::Buffer::Usage>(kor::Buffer::Usage::eStorage) | kor::Buffer::Usage::eTransferSrc)
                .SetType(kor::Buffer::Type::eDynamic).SetIsPerFrame(true).Build();
            if (!buffer.Valid()) return Fail(buffer.Failure() ? buffer.Failure()->message : std::format("'{}' could not be made a buffer", port));
            instances->buffer = kor::ResourceRef<kor::Buffer>(buffer);
            instances->held = MakeResource(std::move(buffer));
            instances->held->node = draw.id;
            FillInstances(*instances, true);
            sharedInstances[key] = instances;
            auto source = bound(*instances);
            run.instances.push_back(std::move(instances));
            return source;
        }

    auto Runtime::Impl::FillInstances(Instances& instances, const bool now) -> void
    {
            const std::uint64_t frame = instances.clock ? *instances.clock : placedFrame;
            if (!now && instances.filled == frame) return;    // another Draw that shares it wrote it this frame
            instances.filled = frame;
            const std::size_t stride = instances.instancing->stride;
            const std::size_t capacity = instances.capacity;
            const auto& fields = instances.instancing->fields;
            // Every element there is room for is written, not only the list's: a GPU cull reads the buffer to its end,
            // and an element left as it was holds what the memory held before — of entities gone since, or of another
            // resource altogether, its pictures' indices among it. One no entity is in is nothing: nowhere, and no picture.
            const auto empty = [&](std::byte* element) {
                std::memset(element, 0, stride);
                for (const auto& field : fields)
                    if (field.Pictured()) std::memset(element + field.offset, 0xFF, 4);
            };
            const auto pictured = static_cast<std::size_t>(std::ranges::count_if(fields, [](const auto& field) { return field.Pictured(); }));
            // Laid out here, between frames, and kept: what changed is written into the copy the frame reads as it begins
            // (WriteStaged).
            const bool fresh = instances.version == 0;
            if (fresh) {
                instances.staged.assign(capacity * stride, std::byte {});
                instances.rowKey.assign(capacity, 0);
                instances.rowOwned.assign(capacity, 0);
                instances.rowChanged.assign(capacity, 0);
                instances.rowPictures.assign(capacity * pictured, nullptr);
                instances.pictureCount.clear();
            }
            const auto found = built.find(instances.source);
            const EntityList* list = found != built.end() && found->second.status.ok && found->second.listed && world ? &*found->second.listed : nullptr;
            const std::size_t rows = list ? list->Rows() : 0;
            // More than there is room for: another buffer, and every Draw that shares this one built again with it. Until
            // then, as many as there is room for: this frame's draws read this buffer still.
            if (rows > capacity) {
                instances.full = true;
                for (const auto& id : instances.draws)
                    if (const auto draw = built.find(id); draw != built.end()) draw->second.signature.clear();
                stale = true;
            }
            const std::size_t laid = std::min(rows, capacity);

            // What could have changed since it was last written. Nothing: none of them is looked at.
            const std::uint64_t moves = world ? world->MoveClock() : 0;
            const bool owned = world && instances.component != kecs::NoComponent;
            const bool materialed = world && !instances.materials.empty();
            // What it reads of each entity, changed: its own component, or one of its materials.
            std::uint64_t owns = owned ? world->ChangesOf(instances.component) : 0;
            if (materialed)
                for (const kecs::ComponentId kind : instances.materials) owns += world->ChangesOf(kind);
            // An entity is drawn only with every material kind its fields are of: without one, it is laid out as no one's.
            const std::uint32_t needed = ((1u << instances.materials.size()) - 1u) << 1;
            const std::uint64_t listed = list ? list->version : 0;
            const bool replaced = pictured > 0 && replacements != instances.seenReplacements;
            if (!fresh && !replaced && list == instances.seenList && listed == instances.seenListed && moves == instances.seenMoves
                && owns == instances.seenOwns && laid == instances.laid)
                return;
            const bool lookMoved = moves != instances.seenMoves;
            const bool lookOwn = (owned || materialed) && owns != instances.seenOwns;
            const kecs::Tick ownSince = instances.seenTick;
            // What is written from here on is written after this: at a tick of its own.
            if ((owned || materialed) && (fresh || lookOwn)) {
                instances.seenTick = world->CurrentTick();
                world->AdvanceTick();
            }

            const std::uint64_t version = instances.version + 1;
            bool changed = fresh, picturesChanged = false;
            // What an element's TEXTURE and STORAGE fields name, counted: the pictures the passes read are those named at
            // least once by a TEXTURE, and the images they write those named at least once by a STORAGE.
            std::vector<bool> writes;
            for (const auto& field : fields)
                if (field.Pictured()) writes.push_back(field.from == Instancing::Field::From::eStorage);
            const auto picture = [&](const std::size_t slot, Member member) {
                const void*& was = instances.rowPictures[slot];
                if (was == member.id) return;
                const bool written = writes[slot % pictured];
                if (was)
                    if (const auto counted = instances.pictureCount.find(was); counted != instances.pictureCount.end()) {
                        std::size_t& uses = written ? counted->second.writes : counted->second.reads;
                        if (uses > 0 && --uses == 0) picturesChanged = true;
                        if (counted->second.reads == 0 && counted->second.writes == 0) instances.pictureCount.erase(counted);
                    }
                was = member.id;
                if (member.id) {
                    auto& counted = instances.pictureCount.try_emplace(member.id, Instances::Named { std::move(member) }).first->second;
                    std::size_t& uses = written ? counted.writes : counted.reads;
                    picturesChanged = picturesChanged || uses == 0;
                    ++uses;
                }
            };
            // An element no entity is in: nowhere, and no picture.
            const auto unlay = [&](const std::size_t row) {
                if (!fresh && instances.rowKey[row] == 0) return;
                empty(instances.staged.data() + row * stride);
                for (std::size_t p = 0; p < pictured; ++p) picture(row * pictured + p, {});
                instances.rowKey[row] = 0;
                instances.rowOwned[row] = 0;
                instances.rowChanged[row] = version;
                changed = true;
            };
            for (std::size_t row = laid; row < (fresh ? capacity : instances.laid); ++row) unlay(row);

            // Which elements are to be laid out again, looked for on every core: what is looked at is only read. One whose
            // entity is gone is marked so (Gone), to be laid out as no one's. And of theirs, whose place in the world is
            // still to be worked out this frame: of those under nothing, on every core, each its own; of the rest, one at
            // a time, since working one out works out what is above it, which another may share.
            constexpr std::uint32_t Gone = 1u << 31;
            constexpr std::size_t Grain = 8192;
            const bool placing = std::ranges::any_of(fields, [](const auto& field) {
                return field.from == Instancing::Field::From::eMatrix || field.from == Instancing::Field::From::eNormalMatrix || field.from == Instancing::Field::From::ePosition;
            });
            struct Looked {
                std::vector<std::uint32_t> again;
                std::vector<kecs::Entity> roots, nested;
            };
            const std::size_t parts = (laid + Grain - 1) / Grain;
            std::vector<Looked> looked(parts);
            const kecs::World& reading = *world;
            // Which of what it reads an entity has: its own component (bit 0), each material kind (from bit 1).
            const auto had = [&](const kecs::Entity entity) {
                std::uint32_t mask = owned && reading.Get(entity, instances.component) ? 1u : 0u;
                for (std::size_t m = 0; m < instances.materials.size(); ++m)
                    if (reading.Get(entity, instances.materials[m])) mask |= 2u << m;
                return mask;
            };
            const auto look = [&](const std::size_t index) {
                Looked& part = looked[index];
                for (std::size_t row = index * Grain; row < std::min(laid, (index + 1) * Grain); ++row) {
                    const kecs::Entity entity = list->entities[row / list->repeat];
                    if (!reading.Alive(entity)) { if (fresh || instances.rowKey[row] != 0) part.again.push_back(static_cast<std::uint32_t>(row) | Gone); continue; }
                    bool due = fresh || replaced || instances.rowKey[row] != KeyOf(entity);
                    if (!due && lookMoved) due = reading.MovedAt(entity) > instances.seenMoves;
                    if (!due && lookOwn) {
                        const std::uint32_t mask = had(entity);
                        due = mask != instances.rowOwned[row];
                        if (!due && (mask & 1u)) due = reading.ChangedTickOf(entity, instances.component) > ownSince;
                        for (std::size_t m = 0; !due && m < instances.materials.size(); ++m)
                            if (mask & (2u << m)) due = reading.ChangedTickOf(entity, instances.materials[m]) > ownSince;
                    }
                    if (!due) continue;
                    part.again.push_back(static_cast<std::uint32_t>(row));
                    if (placing && (entity.index >= placedAt.size() || placedAt[entity.index] != placedFrame))
                        (list->repeat == 1 && reading.ParentOf(entity).IsNull() ? part.roots : part.nested).push_back(entity);
                }
            };
            if (parts > 1) kor::ParallelFor(0, parts, look, 1).Wait();
            else if (parts == 1) look(0);
            std::vector<std::uint32_t> again;
            std::vector<kecs::Entity> roots, nested;
            for (Looked& part : looked) {
                for (const std::uint32_t row : part.again)
                    if (row & Gone) unlay(row & ~Gone);
                    else again.push_back(row);
                roots.insert(roots.end(), part.roots.begin(), part.roots.end());
                nested.insert(nested.end(), part.nested.begin(), part.nested.end());
                part = {};
            }
            if (!again.empty()) changed = true;
            // Room is made for all of them first: Placed grows its lists.
            std::size_t room = placedAt.size();
            for (const auto* placed : { &roots, &nested })
                for (const kecs::Entity entity : *placed) room = std::max<std::size_t>(room, std::size_t { entity.index } + 1);
            if (room > placedAt.size()) { placedAt.resize(room, NeverPlaced); placedMatrices.resize(room); }
            for (const kecs::Entity entity : nested) (void)Placed(entity);
            const auto place = [&](const std::size_t first, const std::size_t last) { for (std::size_t i = first; i < last; ++i) (void)Placed(roots[i]); };
            if (roots.size() > Grain) kor::ParallelForRanges(0, roots.size(), place, Grain).Wait();
            else place(0, roots.size());
            // Laid out on every core: each element is its own.
            const auto layOut = [&](const std::size_t first, const std::size_t last) {
                for (std::size_t i = first; i < last; ++i) {
                    const std::uint32_t row = again[i];
                    const kecs::Entity entity = list->entities[row / list->repeat];
                    const auto* own = owned ? static_cast<const std::byte*>(reading.Get(entity, instances.component)) : nullptr;
                    std::byte* element = instances.staged.data() + std::size_t { row } * stride;
                    empty(element);
                    instances.rowKey[row] = KeyOf(entity);
                    const std::uint32_t mask = had(entity);
                    instances.rowOwned[row] = mask;
                    instances.rowChanged[row] = version;
                    if ((mask & needed) != needed) continue;    // without one of its materials: no one's, and not drawn
                    static const kecs::Mat4 nowhere {};
                    const kecs::Mat4& placed = placing ? placedMatrices[entity.index] : nowhere;
                    for (const auto& field : fields) {
                        std::byte* at = element + field.offset;
                        switch (field.from) {
                        case Instancing::Field::From::eComponent:
                            if (own) std::memcpy(at, own + field.at, field.size);
                            break;
                        case Instancing::Field::From::eMatrix: std::memcpy(at, placed.m.data(), 64); break;
                        case Instancing::Field::From::eNormalMatrix: {
                            const kecs::Mat4 inverse = kecs::Inverse(placed);
                            kecs::Mat4 normal;
                            for (int column = 0; column < 4; ++column)
                                for (int r = 0; r < 4; ++r) normal.At(column, r) = inverse.At(r, column);
                            std::memcpy(at, normal.m.data(), 64);
                            break;
                        }
                        case Instancing::Field::From::ePosition: {
                            const float position[4] { placed.At(3, 0), placed.At(3, 1), placed.At(3, 2), 1.f };
                            std::memcpy(at, position, std::min<std::size_t>(field.size, 16));
                            break;
                        }
                        case Instancing::Field::From::eId: {
                            const std::uint32_t id = entity.index + 1;
                            std::memcpy(at, &id, 4);
                            break;
                        }
                        case Instancing::Field::From::eMaterial:
                            if (const auto* material = static_cast<const std::byte*>(reading.Get(entity, field.component)))
                                WriteMaterialField(field.source, material + field.at, field.type, at, field.size);
                            break;
                        case Instancing::Field::From::eTexture:
                        case Instancing::Field::From::eStorage:
                        case Instancing::Field::From::eMaterialTexture: break;     // below, one at a time: what they name is counted
                        }
                    }
                }
            };
            if (again.size() > Grain) kor::ParallelForRanges(0, again.size(), layOut, Grain).Wait();
            else layOut(0, again.size());

            // The pictures they hold, by the index a shader reaches each at: ~0 for none.
            if (pictured > 0)
                for (const std::uint32_t row : again) {
                    const kecs::Entity entity = list->entities[row / list->repeat];
                    const auto* own = owned ? static_cast<const std::byte*>(reading.Get(entity, instances.component)) : nullptr;
                    std::byte* element = instances.staged.data() + std::size_t { row } * stride;
                    const bool drawn = (instances.rowOwned[row] & needed) == needed;
                    std::size_t texture = 0;
                    for (const auto& field : fields) {
                        if (!field.Pictured()) continue;
                        std::uint32_t index = ~0u;
                        const std::byte* from = !drawn ? nullptr : field.Material() ? static_cast<const std::byte*>(reading.Get(entity, field.component)) : own;
                        const Held* held = from ? reinterpret_cast<const Held*>(from + field.at) : nullptr;
                        // A picture read since it was given is what was made in its place: its stand-in is what it was.
                        const Resource* now = held ? held->get() : nullptr;
                        while (now && now->replacedBy) now = now->replacedBy.get();
                        Member member;
                        if (now && now->handle) {
                            const Resource& given = *now;
                            kor::DescriptorIndex handle;
                            const bool written = field.from == Instancing::Field::From::eStorage;
                            MadeWithout(given, written ? kor::Image::Usage::eStorage : kor::Image::Usage::eSampled);    // said, for its image to be made for it
                            if (given.kind == "Image") handle = written ? kor::Bindless::Storage(kor::capi::ImageOf(given.handle)) : kor::Bindless::Sampled(kor::capi::ImageOf(given.handle));
                            else if (given.kind == "ImageView") handle = written ? kor::Bindless::Storage(kor::capi::ImageViewOf(given.handle)) : kor::Bindless::Sampled(kor::capi::ImageViewOf(given.handle));
                            if (handle.Valid()) {
                                index = handle.index;
                                member = MemberOf(now);
                            }
                        }
                        picture(std::size_t { row } * pictured + texture++, std::move(member));
                        std::memcpy(element + field.offset, &index, 4);
                    }
                }
            if (changed && materialed) {
                std::size_t skipped = 0;
                for (std::size_t row = 0; row < laid; ++row) skipped += instances.rowKey[row] != 0 && (instances.rowOwned[row] & needed) != needed;
                instances.skipped = skipped;
            }
            instances.laid = laid;
            instances.seenList = list;
            instances.seenListed = listed;
            instances.seenMoves = moves;
            instances.seenOwns = owns;
            instances.seenReplacements = replacements;
            if (changed) instances.written = instances.version = version;
            if (picturesChanged) {
                std::vector<Member> pictures, storages;
                for (const auto& counted : instances.pictureCount | std::views::values) {
                    if (counted.reads > 0) pictures.push_back(counted.member);
                    if (counted.writes > 0) storages.push_back(counted.member);
                }
                std::ranges::sort(pictures, {}, &Member::id);
                std::ranges::sort(storages, {}, &Member::id);
                instances.pictures = std::move(pictures);
                instances.storages = std::move(storages);
                ++instances.named;
            }
        }

    auto Runtime::Impl::Unchanged(EntityRun& run, const std::optional<kor::Mat4>& sight) -> void
    {
            if (!run.whenChanged) return;
            std::uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](const void* bytes, const std::size_t size) { Mix(hash, bytes, size); };
            const auto mixed = [&](const auto& value) { mix(&value, sizeof value); };
            if (sight) for (int column = 0; column < 4; ++column) mixed((*sight)[column]);
            for (const Item& item : run.items) {
                const std::uint64_t key = item.entity.IsNull() ? 0 : KeyOf(item.entity);
                mixed(key);
                if (!item.entity.IsNull()) mix(Placed(item.entity).m.data(), 64);
                const void* handles[3] { item.mesh.Get(), item.pipeline.Get(), item.set.Get() };
                mixed(handles);
                const std::uint32_t range[6] { item.part, item.indexed, item.firstIndex, item.indexCount, static_cast<std::uint32_t>(item.vertexOffset), item.groups[0] };
                mixed(range);
            }
            mix(run.pushed.data(), run.pushed.size());
            // What each instance is given, where it draws instances: written from the entities, and not in its items.
            for (const auto& instances : run.instances) mixed(instances->written);
            run.drawingAs = hash;
            run.due = hash != run.drawnAs || run.drawnTimes < run.copies;
        }

    auto Runtime::Impl::WhichDrawAgain() -> void
    {
            std::vector<EntityRun*> runs;
            std::unordered_map<const void*, bool> drawnInto;     // a view, and whether everything drawing into it draws
            for (const auto& id : wanted) {
                EntityRun* run = built.at(id).pass->entities.get();
                if (!run || !run->draws || run->idle) continue;
                for (const void* view : run->targets) drawnInto[view] = drawnInto[view] || !run->whenChanged || run->due;
                if (run->whenChanged) runs.push_back(run);
            }
            for (bool more = true; more;) {
                more = false;
                for (EntityRun* run : runs) {
                    if (run->due || std::ranges::none_of(run->targets, [&](const void* view) { return drawnInto[view]; })) continue;
                    run->due = more = true;
                    for (const void* view : run->targets) drawnInto[view] = true;
                }
            }
            for (EntityRun* run : runs) {
                if (!run->due) {
                    run->idle = true;
                    run->report.unchanged = true;
                    continue;
                }
                run->drawnTimes = run->drawingAs == run->drawnAs ? run->drawnTimes + 1 : 1;
                run->drawnAs = run->drawingAs;
            }
        }

    auto Runtime::Impl::Prepare(EntityRun& run) -> void
    {
            // Idle this frame: what it draws into stays as it is, nothing is readied for it, and what it last said stands.
            run.idle = false;
            run.due = true;     // unless what it draws is found to be what it drew (Unchanged)
            if (!run.idleNode.empty())
                if (const auto source = built.find(run.idleNode); source != built.end())
                    if (const auto output = source->second.outputs.find(run.idleOutput); output != source->second.outputs.end()) {
                        const Value& said = output->second.value;
                        if (const auto* numbers = said.Items(); numbers && numbers->size() > static_cast<std::size_t>(run.cullIndex)) {
                            if (const double* number = (*numbers)[static_cast<std::size_t>(run.cullIndex)].Number()) run.idle = *number != 0.0;
                        } else if (const double* number = said.Number()) run.idle = *number != 0.0;     // one, for every turn
                        else if (const bool* flag = said.Bool()) run.idle = *flag;
                    }
            run.items.clear();
            run.holding.clear();
            run.pushed.clear();
            // Its buffer of instances follows the entities, idle or not: another pass may read it while this one rests
            // (the meshlets read the forward Draw's). Whether it is due again is said by what it last drew, not by this.
            for (const auto& instances : run.instances) FillInstances(*instances);
            if (run.idle) return;
            // The pictures they name by index are read by the draws all the same: the frame graph is told of them. So are
            // those of another Draw's buffer of instances, wired here: what samples them is this pass as much.
            std::vector<const Instances*> named;
            for (const auto& instances : run.instances) named.push_back(instances.get());
            for (const Source& slot : run.slots)
                if (slot.kind == Source::Kind::eWired && slot.resource)
                    for (const Impl* owner : { static_cast<const Impl*>(source), static_cast<const Impl*>(this) })
                        if (owner)
                            for (const auto& entry : owner->sharedInstances | std::views::values)
                                if (const auto shared = entry.lock(); shared && shared->held == slot.resource) named.push_back(shared.get());
            // Gathered again only when one of them names others than it did: thousands of images, sorted, each frame otherwise.
            std::vector<std::pair<const void*, std::uint64_t>> seen;
            for (const Instances* instances : named) seen.emplace_back(instances, instances->named);
            if (!named.empty() && seen != run.namedSeen) {
                run.namedSeen = std::move(seen);
                // And the images they write by index — a STORAGE field's — are written by it.
                for (auto [list, held] : { std::pair { &Instances::pictures, &run.pictures }, std::pair { &Instances::storages, &run.storages } }) {
                    std::vector<Member> members;
                    for (const Instances* instances : named) members.insert(members.end(), (instances->*list).begin(), (instances->*list).end());
                    std::ranges::sort(members, {}, &Member::id);
                    members.erase(std::ranges::unique(members, {}, &Member::id).begin(), members.end());
                    if (!std::ranges::equal(members, *held, {}, &Member::id, &Member::id)) {
                        *held = std::move(members);
                        run.changed = true;
                    }
                }
            }
            run.report = {};
            const PipelineInfo& info = *run.info;
            const std::size_t slots = info.slots.size();
            for (auto& cached : run.cache | std::views::values) cached.seen = false;

            // Whose elements: its ForEach's, as of this frame; or the one draw.
            Built* each = nullptr;
            if (!run.each.empty()) {
                const auto found = built.find(run.each);
                if (found == built.end() || !found->second.status.ok || !found->second.listed) return;
                each = &found->second;
            }
            const std::size_t rows = each ? each->listed->Rows() : 1;
            const std::size_t repeat = each ? std::max<std::size_t>(each->listed->repeat, 1) : 1;

            // What is out of sight: outside the box the Draw's `cull` matrix makes of what it sees. Read as it is now.
            std::optional<kor::Mat4> sight;
            if (run.draws && !run.cullNode.empty())
                if (const auto source = built.find(run.cullNode); source != built.end())
                    if (const auto output = source->second.outputs.find(run.cullOutput); output != source->second.outputs.end())
                        if (const auto* numbers = output->second.value.Items(); numbers && numbers->size() >= static_cast<std::size_t>(run.cullIndex + 1) * 16) {
                            kor::Mat4 matrix;
                            for (int i = 0; i < 16; ++i) matrix[i / 4][i % 4] = static_cast<float>((*numbers)[static_cast<std::size_t>(run.cullIndex * 16 + i)].Number() ? *(*numbers)[static_cast<std::size_t>(run.cullIndex * 16 + i)].Number() : 0.0);
                            sight = matrix;
                        }

            const auto graphics = run.draws ? kor::ResourceRef<const kor::GraphicsPipeline>(kor::capi::GraphicsPipelineOf(info.resource->handle)) : kor::ResourceRef<const kor::GraphicsPipeline>();

            // What one of them is, for the element at hand: a resource, or a value.
            const auto output = [&](const Source& source) -> const Output* {
                const auto from = built.find(source.node);
                if (from == built.end()) return nullptr;
                const auto it = from->second.outputs.find(source.output);
                return it == from->second.outputs.end() ? nullptr : &it->second;
            };
            // Where it is held rather than a copy of it, for each element of every pass every frame: what holds it — the
            // node, the entity's component, the output — stays as it is while the element is readied.
            static const Held none;
            const auto resource = [&](const Source& source, const kecs::Entity entity) -> const Held& {
                const Held* held = &none;
                switch (source.kind) {
                case Source::Kind::eGiven:
                case Source::Kind::eWired: held = &source.resource; break;
                case Source::Kind::eField:
                    if (const auto* bytes = entity.IsNull() ? nullptr : static_cast<const std::byte*>(world->Get(entity, source.component)))
                        held = reinterpret_cast<const Held*>(bytes + source.field.offset);
                    break;
                case Source::Kind::eWhere:
                case Source::Kind::eChain:
                    if (const auto* given = output(source)) held = &given->resource;
                    break;
                default: break;
                }
                // A resource whose node was built again since: the new one, from here on.
                while (*held && (*held)->replacedBy) held = &(*held)->replacedBy;
                return *held;
            };
            std::vector<const Held*> held(slots);

            for (std::size_t row = 0; row < rows; ++row) {
                const kecs::Entity entity = each && row / repeat < each->listed->entities.size() ? each->listed->entities[row / repeat] : kecs::NullEntity;
                if (each && run.rows) {
                    SetRow(*each, row, &run.asked);
                    for (const auto& id : run.chain) Refresh(id);
                }
                const auto skip = [&](const std::string& why) {
                    ++run.report.skipped;
                    if (run.report.why.empty()) run.report.why = entity.IsNull() ? why : std::format("entity {}: {}", entity.index, why);
                    if (run.report.passedBy.size() < 512) run.report.passedBy.emplace_back(entity, why);
                };
                Item item;
                item.entity = entity;
                if (run.instancedRows) item.firstInstance = static_cast<kor::u32>(row);
                item.pipeline = graphics;

                if (run.draws && run.mesh.kind != Source::Kind::eNone) {
                    const Held& mesh = resource(run.mesh, entity);
                    // A model's mesh is a part of the one mesh the model is: that is what is bound.
                    const Resource* whole = mesh && mesh->part ? mesh->source.get() : mesh.get();
                    if (!whole || !whole->handle || mesh->kind != "Mesh") { skip(mesh && mesh->node.starts_with('@') ? "its mesh is still being read" : "it has no mesh to draw"); continue; }
                    item.mesh = kor::capi::MeshOf(whole->handle);
                    if (run.holding.empty() || run.holding.back() != mesh) run.holding.push_back(mesh);
                    item.part = mesh->part;
                    item.firstIndex = mesh->firstIndex;
                    item.indexCount = mesh->indexCount;
                    item.vertexOffset = mesh->vertexOffset;
                    // Drawn with the pipeline made for its mesh's layout: any mesh that has what the vertex shader reads.
                    auto& known = run.fits[item.mesh.Get()];
                    if (!known.mesh.Alive() || known.mesh.Get() != item.mesh.Get() || (known.why.empty() && !known.pipeline.Alive())) {
                        auto pipeline = PipelineFor(info, *item.mesh);
                        known = pipeline ? EntityRun::Fit { item.mesh, *pipeline, {} } : EntityRun::Fit { item.mesh, {}, pipeline.error() };
                    }
                    if (!known.why.empty()) { skip(known.why); continue; }
                    item.pipeline = known.pipeline;
                    const bool indices = item.mesh->IndexCount().has_value();
                    if (run.command == DrawWith::eDrawIndexed && !indices) { skip("its mesh has no indices, and the node draws indexed"); continue; }
                    item.indexed = run.command == DrawWith::eDrawIndexed || (run.command == DrawWith::eAuto && indices);
                    if (sight && mesh->bounded) {
                        // Its box's eight corners, where the matrix puts them — placed as its entity is: all past one side is out of sight.
                        const kor::Mat4 placed = entity.IsNull() ? *sight : *sight * ToKor(Placed(entity));
                        int past[6] { 0, 0, 0, 0, 0, 0 };
                        // The least corner where the matrix puts it, and each edge of the box from there: every corner is
                        // the one and some of the others, which is four products rather than eight.
                        const kor::Vec4 least = placed * kor::Vec4(mesh->bounds[0], mesh->bounds[1], mesh->bounds[2], 1.f);
                        const kor::Vec4 edges[3] { placed * kor::Vec4(mesh->bounds[3] - mesh->bounds[0], 0.f, 0.f, 0.f),
                                                   placed * kor::Vec4(0.f, mesh->bounds[4] - mesh->bounds[1], 0.f, 0.f),
                                                   placed * kor::Vec4(0.f, 0.f, mesh->bounds[5] - mesh->bounds[2], 0.f) };
                        for (int corner = 0; corner < 8; ++corner) {
                            kor::Vec4 at = least;
                            for (int axis = 0; axis < 3; ++axis) if (corner & (1 << axis)) at += edges[axis];
                            past[0] += at.x < -at.w; past[1] += at.x > at.w; past[2] += at.y < -at.w; past[3] += at.y > at.w; past[4] += at.z < 0.f; past[5] += at.z > at.w;
                        }
                        if (std::ranges::any_of(past, [](const int count) { return count == 8; })) {
                            // Out of sight — but one that could not be drawn in sight either says so all the same: what is
                            // wrong with an entity is not a matter of where the camera is.
                            std::string lacking;
                            for (std::size_t i = 0; i < slots && lacking.empty(); ++i)
                                if (const Held& given = resource(run.slots[i], entity); !given || (!given->handle && !given->part)) lacking = Lacks(run.slots[i]);
                            if (!lacking.empty()) skip(lacking);
                            else ++run.report.culled;
                            continue;
                        }
                    }
                }

                if (slots > 0) {
                    std::string lacking;
                    for (std::size_t i = 0; i < slots && lacking.empty(); ++i) {
                        const Slot& slot = info.slots[i];
                        held[i] = &resource(run.slots[i], entity);
                        if (!*held[i] || !(*held[i])->handle) lacking = Lacks(run.slots[i]);
                        else if (!Takes(slot.type, (*held[i])->kind)) lacking = std::format("'param.{}' takes a {}, and was given a {}", slot.field, KindOf(slot.type), (*held[i])->kind);
                    }
                    if (!lacking.empty()) { skip(lacking); continue; }

                    auto& cached = run.cache[{ entity.IsNull() ? 0 : KeyOf(entity), row % repeat }];
                    bool same = cached.set.Valid() && cached.key.size() == slots;
                    for (std::size_t i = 0; same && i < slots; ++i) same = cached.key[i] == held[i]->get();
                    if (!same) {
                        kor::DescriptorSet::Builder set(info.pipeline, 1);
                        std::string failed;
                        for (std::size_t i = 0; i < slots && failed.empty(); ++i) {
                            const Slot& slot = info.slots[i];
                            if (slot.sampler) continue;
                            const Resource* sampler = nullptr;
                            if (slot.type == kor::DescriptorType::eCombinedImageSampler) {
                                if (i + 1 < slots && info.slots[i + 1].sampler) sampler = held[i + 1]->get();
                                else if (const auto shared = info.samplers.find(slot.parameter); shared != info.samplers.end()) sampler = shared->second.get();
                            }
                            if (const auto written = WriteParameter(set, slot.parameter, slot.type, **held[i], sampler, "param." + slot.field); !written) failed = written.error();
                        }
                        kor::Resource<kor::DescriptorSet> made;
                        if (failed.empty()) {
                            made = set.Build();
                            if (!made.Valid()) failed = made.Failure() ? made.Failure()->message : std::string("its descriptor set could not be built");
                        }
                        // The set it had may be in what this frame has recorded: kept until that frame is done.
                        retiredSets.push_back(std::exchange(cached, EntityRun::Cached {}));
                        if (!failed.empty()) { run.cache.erase({ entity.IsNull() ? 0 : KeyOf(entity), row % repeat }); run.changed = true; skip(failed); continue; }
                        cached.set = std::move(made);
                        cached.key.assign(slots, nullptr);
                        cached.held.clear();
                        for (const Held* given : held) cached.held.push_back(*given);
                        cached.members.assign(slots, Member {});
                        for (std::size_t i = 0; i < slots; ++i) {
                            cached.key[i] = held[i]->get();
                            if (!info.slots[i].sampler) cached.members[i] = MemberOf(held[i]->get());
                        }
                        run.changed = true;
                    }
                    cached.seen = true;
                    item.set = cached.set;
                    if (run.cover >= 0 && cached.members[static_cast<std::size_t>(run.cover)].image.Get()) {
                        const auto extent = cached.members[static_cast<std::size_t>(run.cover)].image->Extent();
                        for (int axis = 0; axis < 3; ++axis)
                            item.groups[axis] = std::max<kor::u32>((static_cast<kor::u32>(extent[axis]) + run.groupSize[axis] - 1) / run.groupSize[axis], 1);
                    } else {
                        std::ranges::copy(run.groups, item.groups);
                    }
                } else {
                    std::ranges::copy(run.groups, item.groups);
                }

                // Its push constants, each from where it comes: zero, for one given nothing.
                if (info.pushSize) {
                    item.push = run.pushed.size();
                    run.pushed.resize(run.pushed.size() + info.pushSize, std::byte { 0 });
                    std::byte* const pushed = run.pushed.data() + item.push;
                    for (std::size_t i = 0; i < info.pushes.size(); ++i) {
                        const PushMember& push = info.pushes[i];
                        const Source& source = run.pushes[i];
                        std::byte* const at = pushed + push.offset;
                        const bool matrix = push.scalar == 0 && push.rows == 4 && push.columns == 4 && push.size == 64;
                        switch (source.kind) {
                        case Source::Kind::eNone: break;
                        case Source::Kind::eGiven: PackPush(push, source.value, at); break;
                        case Source::Kind::eField: {
                            const auto* bytes = entity.IsNull() ? nullptr : static_cast<const std::byte*>(world->Get(entity, source.component));
                            if (!bytes) break;
                            if (source.raw) std::memcpy(at, bytes + source.field.offset, std::min<std::size_t>(source.field.size, push.size));
                            else PackPush(push, FieldValue(source.field, bytes + source.field.offset), at);
                            break;
                        }
                        case Source::Kind::eWhere:
                            // Where it is and which it is, straight from the world.
                            if (source.output == "matrix" && matrix) { if (!entity.IsNull()) std::memcpy(at, Placed(entity).m.data(), 64); break; }
                            if (source.output == "normalMatrix" && matrix) {
                                if (entity.IsNull()) break;
                                const kecs::Mat4 inverse = kecs::Inverse(Placed(entity));
                                kecs::Mat4 normal;
                                for (int column = 0; column < 4; ++column)
                                    for (int r = 0; r < 4; ++r) normal.At(column, r) = inverse.At(r, column);
                                std::memcpy(at, normal.m.data(), 64);
                                break;
                            }
                            if (source.output == "entityId" && !run.asked.contains("entityId")) { PackPush(push, Value(entity.IsNull() ? 0.0 : entity.index + 1.0), at); break; }
                            if (const auto* given = output(source)) PackPush(push, given->value, at);
                            break;
                        case Source::Kind::eWired:
                        case Source::Kind::eChain:
                            if (const auto* given = output(source)) PackPush(push, given->value, at);
                            break;
                        }
                    }
                }
                run.items.push_back(std::move(item));
            }
            // Anything else wired to the ForEach has the first element, as it does without a draw.
            if (each && run.rows && rows > 1) {
                SetRow(*each, 0);
                for (const auto& id : run.chain) Refresh(id);
            }
            // Those of one mesh one after another: a mesh is then bound once for all of them.
            if (run.draws) std::ranges::stable_sort(run.items, {}, [](const Item& item) { return item.mesh.Get(); });
            // A Dispatch of a Filter's entities: its groups along z as many times over as there are of them, the element
            // SV_GroupID.z / groups.z. None of them: nothing to run. More than a dispatch counts along z: said, and not run —
            // a dispatch from a base counts no further (its base and its count together are within the device's limit).
            if (!run.batchOf.empty()) {
                const auto found = built.find(run.batchOf);
                const std::size_t elements = found != built.end() && found->second.listed ? found->second.listed->Rows() : 0;
                const kor::u64 most = 65535;
                for (Item& item : run.items) {
                    if (static_cast<kor::u64>(item.groups[2]) * elements > most) {
                        ++run.report.skipped;
                        if (run.report.why.empty())
                            run.report.why = std::format("{} of '{}' at {} groups along z each is {} groups along z, past the {} a dispatch can count", elements, run.batchOf, item.groups[2],
                                                         static_cast<kor::u64>(item.groups[2]) * elements, most);
                        item.groups[2] = 0;
                        continue;
                    }
                    item.groups[2] *= static_cast<kor::u32>(elements);
                }
                std::erase_if(run.items, [](const Item& item) { return item.groups[2] == 0; });
            }
            run.report.ran = run.items.size();
            if (run.draws) Unchanged(run, sight);

            // Elements that are gone let go of what was made for them.
            for (auto it = run.cache.begin(); it != run.cache.end();) {
                if (it->second.seen) { ++it; continue; }
                retiredSets.push_back(std::move(it->second));
                it = run.cache.erase(it);
                run.changed = true;
            }
            if (!run.changed) return;
            // Every element's resources, a slot at a time and once each: what the frame graph orders this pass by.
            run.members.resize(slots);
            for (std::size_t i = 0; i < slots; ++i) {
                run.members[i].clear();
                if (info.slots[i].sampler) continue;
                std::set<const void*> had;
                for (const auto& cached : run.cache | std::views::values)
                    if (cached.members[i].id && had.insert(cached.members[i].id).second) run.members[i].push_back(cached.members[i]);
                std::ranges::sort(run.members[i], {}, &Member::id);
            }
        }

    auto Runtime::Impl::Gather(const Node& node) const -> std::optional<Inputs>
    {
            Inputs inputs;
            for (const auto& [name, input] : node.inputs) {
                Resolved resolved;
                if (input.link) {
                    const auto source = built.find(input.link->node);
                    if (source == built.end() || !source->second.status.ok) return std::nullopt;
                    const auto output = source->second.outputs.find(input.link->output);
                    if (output == source->second.outputs.end()) return std::nullopt;
                    resolved.linked = true;
                    resolved.value = output->second.value;
                    resolved.resource = output->second.resource;
                } else {
                    resolved.value = input.value;
                }
                inputs.emplace(name, std::move(resolved));
            }
            return inputs;
        }

    auto Runtime::Impl::RunLive() -> void
    {
            // What is for setup alone, on a frame setup records — or one when entities or components came or went, so
            // that what is counted of them (a buffer's size) follows.
            bool all = setupRecords;
            if (world && world->StructureVersion() != structureSeen) { structureSeen = world->StructureVersion(); all = true; }
            for (const std::string& id : live)
                if (all || !setupOnly.contains(id)) Refresh(id);
            for (const std::string& id : sized) {
                const Node* node = current ? CurrentNode(id) : nullptr;
                const auto found = built.find(id);
                if (!node || found == built.end() || !found->second.status.ok) continue;
                if (WiredValues(*node) != found->second.madeWith) { found->second.signature.clear(); stale = true; }
            }
        }

    auto Runtime::Impl::IsMadeResource(const std::string_view type) const -> bool
    {
            const NodeType* found = FindType(type);
            return found && found->builder && !IsPipeline(found->name);
        }

    auto Runtime::Impl::WiredValues(const Node& node) const -> std::string
    {
            std::string values;
            for (const auto& [name, input] : node.inputs) {
                if (!input.link) continue;
                const auto source = built.find(input.link->node);
                if (source == built.end()) continue;
                const auto output = source->second.outputs.find(input.link->output);
                if (output != source->second.outputs.end() && !output->second.resource) values += std::format("{}={};", name, output->second.value.ToJson());
            }
            return values;
        }

    auto Runtime::Impl::ReadFromReferences(const Graph& graph) -> void
    {
            referencesRead.clear();
            std::map<std::string, std::string, std::less<>> standsFor;      // a Reference, what it stands for
            for (const Node& node : graph.nodes)
                if (node.type == "Reference")
                    if (const auto target = node.inputs.find("node"); target != node.inputs.end() && target->second.value.String())
                        standsFor[node.id] = *target->second.value.String();
            for (const Node& node : graph.nodes)
                for (const auto& input : node.inputs | std::views::values)
                    if (input.link && standsFor.contains(input.link->node)) referencesRead[input.link->node].insert(input.link->output);
            // A Reference to a Reference: the one it stands for keeps what it is read for.
            for (std::size_t hops = 0; hops < 16; ++hops) {
                bool more = false;
                for (const auto& [reference, target] : standsFor) {
                    const auto read = referencesRead.find(reference);
                    if (read == referencesRead.end() || !standsFor.contains(target)) continue;
                    const std::set<std::string> names = read->second;
                    for (const auto& name : names) more = referencesRead[target].insert(name).second || more;
                }
                if (!more) break;
            }
        }

    auto Runtime::Impl::Refresh(const std::string& id) -> void
    {
            const Node* node = current ? CurrentNode(id) : nullptr;
            const auto found = built.find(id);
            if (!node || found == built.end() || !found->second.status.ok) return;
            Built& out = found->second;
            if (IsMapped(node->type)) {
                if (out.mapped) Fill(*node, out);
                return;
            }
            if (node->type == "Collect") { CollectAll(*node, out); return; }
            if (node->type == "Reference") {
                // Nothing to gather, and in a ForEach's chain this is done for each entity. Only what is wired from it: of a
                // Filter there is a list of every entity for each field of each component.
                const auto target = node->inputs.find("node");
                const auto source = target != node->inputs.end() && target->second.value.String() ? built.find(*target->second.value.String()) : built.end();
                const auto wired = referencesRead.find(id);
                if (source == built.end() || wired == referencesRead.end()) return;
                for (const auto& name : wired->second) {
                    const auto output = source->second.outputs.find(name);
                    if (output == source->second.outputs.end() || output->second.resource) continue;
                    if (Value& kept = out.outputs[name].value; !(kept == output->second.value)) kept = output->second.value;
                }
                return;
            }
            auto inputs = Gather(*node);
            if (!inputs || !InParts(*node, *inputs)) return;
            if (ReadsTheWorld(node->type)) {
                (void)BuildLive(*node, *inputs, out);
            } else if (node->type == "MeshPack") {
                // Other meshes than it packed — an entity given another, one arrived from its file: packed again.
                if (!out.packing) return;
                // Looked at again only when something they are found from could have changed: the entities, their meshes,
                // a resource made again in its place. Otherwise thousands of entities are walked each frame for nothing.
                const auto wired = node->inputs.find("meshes");
                const Node* origin = wired != node->inputs.end() && wired->second.link ? Origin(wired->second.link->node) : nullptr;
                const auto source = origin ? built.find(origin->id) : built.end();
                const EntityList* list = source != built.end() && source->second.listed ? &*source->second.listed : nullptr;
                const auto read = list ? std::ranges::find(list->fields, wired->second.link->output, &EntityList::Read::output) : std::vector<EntityList::Read>::const_iterator {};
                const bool listed = list && read != list->fields.end() && read->resource && world;
                const MeshPacking::Seen seen { list, list ? list->version : 0, listed ? world->ChangesOf(read->component) : 0, replacements };
                if (listed && out.packing->seen == seen) return;
                if (PackedNow(*node) != out.packing->of) { out.signature.clear(); stale = true; return; }
                if (listed) out.packing->seen = seen;
            } else if (node->type == "IndirectDraws") {
                if (!out.packed) return;
                auto commands = IndirectCommands(*inputs, OnTheGpu(*inputs));
                if (!commands) { out.signature.clear(); stale = true; return; }
                auto& [bytes, instances] = *commands;
                // More commands, or more instances than `visible` has room for: another buffer.
                if (bytes.size() != out.packed->bytes.size() || (out.packed->everyFrame && instances > out.packed->room)) { out.signature.clear(); stale = true; return; }
                if (!out.packed->everyFrame && bytes == out.packed->bytes) return;
                // On the GPU's, its pass writes them in each frame: what it writes is kept here.
                if (!bytes.empty() && !out.packed->everyFrame) out.packed->buffer->Write(std::span<const std::byte>(bytes));
                out.packed->bytes = std::move(bytes);
            } else {
                FillDefaults(*node, *inputs);
                Built fresh;
                if (!BuildValue(*node, *inputs, fresh)) return;
                for (auto& [name, output] : fresh.outputs)
                    if (!output.resource) out.outputs[name].value = std::move(output.value);
            }
        }

    auto Runtime::Impl::RunSetup() -> void
    {
            // A copy of what a setup page begins with is made while setup is: a new one makes setup be made again. As does
            // the scene starting or stopping.
            bool again = std::exchange(setupAgain, false);
            for (const auto& id : before)
                if (const auto copy = built.find(id); copy != built.end() && copy->second.pass) {
                    const Node* node = current ? CurrentNode(id) : nullptr;
                    if (!node || !current->IsSetup(node->pass) || copy->second.ranFor == copy->second.stamp) continue;
                    copy->second.ranFor = copy->second.stamp;
                    again = true;
                }
            // Setup is one sequence: a pass of it built again runs again with all the rest of it, not alone — what one
            // adds up, another clears first.
            for (const auto& id : wanted) {
                if (again) break;
                if (id.starts_with('$')) continue;
                const Built& out = built.at(id);
                const Node* node = current ? CurrentNode(id) : nullptr;
                if (node && current->IsSetup(node->pass) && (!out.setup || out.ranFor != out.stamp)) again = true;
            }
            setupRecords = false;
            std::uint32_t runs = 0;
            for (const auto& id : wanted) {
                if (id.starts_with('$')) continue;      // a watch's copy, which is copied as its watch says
                Built& out = built.at(id);
                const Node* node = current ? CurrentNode(id) : nullptr;
                const bool setup = node && current->IsSetup(node->pass);
                if (!setup) {
                    out.setup = false;
                    // A page of the fixed step runs on a frame with a step in it, and rests on one without.
                    out.pass->resting->store(node && current->IsFixed(node->pass) && fixedSteps == 0);
                    continue;
                }
                if (!out.setup || out.ranFor != out.stamp || again) {
                    out.setup = true;
                    out.ranFor = out.stamp;
                    out.runsLeft = 1;
                    for (const auto& use : out.pass->uses)
                        if (use.write && ((use.what.image.Get() && use.what.image->IsPerFrame()) || (use.what.buffer.Get() && use.what.buffer->IsPerFrame())))
                            out.runsLeft = std::max(out.runsLeft, kor::Context::HasScheduler() ? kor::Context::Scheduler().ImageCount() : 1u);
                    runs = std::max(runs, out.runsLeft);
                }
            }
            // And every pass of it as many times as the one that runs most: each time, all of it, in its order.
            for (const auto& id : wanted) {
                if (id.starts_with('$')) continue;
                Built& out = built.at(id);
                if (!out.setup) continue;
                if (again) out.runsLeft = runs;
                out.pass->resting->store(out.runsLeft == 0);
                if (out.runsLeft > 0) { --out.runsLeft; setupRecords = true; }
            }
        }

    auto Runtime::Impl::EachFrame() -> void
    {
            // As many as the graph runs at most: the rest are lost.
            const auto most = static_cast<std::uint32_t>(std::max(current ? current->fixedStepsAtMost : 4, 1));
            const std::uint32_t taken = std::exchange(fixedPending, 0);
            fixedSteps = std::min(taken, most);
            fixedLost += taken - fixedSteps;
            fixedNow->store(fixedSteps);
            fixedCount += fixedSteps;
            // The time goes on by what a frame took — at most a quarter of a second, so that a frame held up does not jump.
            const auto now = std::chrono::steady_clock::now();
            timeDelta = timeLast ? std::min(std::chrono::duration<double>(now - *timeLast).count(), 0.25) : 0.0;
            timeLast = now;
            timeSeconds += timeDelta;
            if (std::exchange(timeBegun, true)) ++timeFrame;
            RunSetup();
            // A watch that is copied now and then rests on the frames between, each on frames of its own.
            ++watchFrame;
            for (auto& [id, watching] : watched) {
                const auto copy = built.find(std::format("$peek {}", id));
                if (copy == built.end() || !copy->second.pass) continue;
                if (watching.since == 0) watching.since = watchFrame;
                const std::uint64_t phase = std::hash<std::string>{}(id);
                const bool fresh = watchFrame - watching.since < 2;     // nothing copied yet: copied now, whatever its turn
                bool rests = !fresh && watching.every > 1 && (watchFrame + phase) % watching.every != 0;
                // What a setup page begins with is there only while setup is made.
                if (before.contains(copy->first))
                    if (const Node* node = current ? CurrentNode(copy->first) : nullptr; node && current->IsSetup(node->pass)) rests = !setupRecords;
                copy->second.pass->resting->store(rests);
            }
            RunLive();
            // The scenes rays are traced through, made again where an entity of theirs moved, came or went.
            for (auto& entry : built)
                if (entry.second.rays && entry.second.status.ok) UpdateRays(entry.first, entry.second);
            for (const auto& id : wanted)
                if (const auto& plan = built.at(id).pass; plan && plan->prepare) plan->prepare();
        }

    auto Runtime::Impl::Placed(const kecs::Entity entity) -> const kecs::Mat4&
    {
            if (entity.index >= placedAt.size()) { placedAt.resize(entity.index + 1, NeverPlaced); placedMatrices.resize(entity.index + 1); }
            if (placedAt[entity.index] != placedFrame) { placedMatrices[entity.index] = world->WorldMatrix(entity); placedAt[entity.index] = placedFrame; }
            return placedMatrices[entity.index];
        }

    auto Runtime::Impl::PrepareAll() -> bool
    {
            bool changed = false;
            ++placedFrame;
            for (const auto& id : wanted) {
                const auto& plan = built.at(id).pass;
                if (!plan->entities) continue;
                Prepare(*plan->entities);
                changed = changed || plan->entities->changed;
                plan->entities->changed = false;
            }
            RestUnread();
            WhichDrawAgain();
            return changed;
        }

    auto Runtime::Impl::RestUnread() -> void
    {
            restingUnread.clear();
            if (usesOf.empty()) return;
            const auto rests = [&](const std::string& id) {
                const auto& plan = *built.at(id).pass;
                return plan.resting->load() || (plan.idle && plan.idle->load()) || (plan.entities && plan.entities->idle);
            };
            // Worked out again only when which passes rest has changed: most frames it has not, and this is maps of strings.
            std::vector<bool> resting;
            resting.reserve(wanted.size());
            for (const auto& id : wanted) resting.push_back(rests(id));
            if (!restsBefore.empty() && resting == restsBefore && unreadOf == usesOf.size()) {
                for (const std::string* id : unreadBefore) {
                    const auto& plan = *built.at(*id).pass;
                    plan.resting->store(true);
                    if (plan.entities) plan.entities->idle = true;
                    restingUnread.insert(*id);
                }
                return;
            }
            restsBefore = std::move(resting);
            unreadOf = usesOf.size();
            unreadBefore.clear();
            // Each resource, and the passes that read it, resting or not.
            std::map<std::string_view, std::vector<std::string_view>> readers;
            for (const auto& [id, uses] : usesOf)
                for (const Declared& use : uses)
                    if (use.read) readers[use.name].push_back(id);
            const auto readByAnother = [&](const std::string& id, const Declared& use) {
                const auto by = readers.find(use.name);
                return by != readers.end() && std::ranges::any_of(by->second, [&](const std::string_view reader) { return reader != id; });
            };
            std::vector<const std::string*> awake;
            std::map<std::string_view, std::vector<const std::string*>> writers;     // a resource, and the passes that run and write it
            std::set<std::string_view> needed;
            std::vector<std::string_view> next;
            for (const auto& id : wanted) {
                // A watch's copy keeps what it copies going on the frames between its copies too: one that goes on from
                // what it made — a simulation looked at — would otherwise go on only when looked at.
                if (id.starts_with('$') && needed.insert(id).second) next.push_back(id);
                if (rests(id)) continue;
                awake.push_back(&id);
                const auto uses = usesOf.find(id);
                if (uses != usesOf.end())
                    for (const Declared& use : uses->second)
                        if (use.write) writers[use.name].push_back(&id);
                const Node* node = current ? CurrentNode(id) : nullptr;
                const bool kept = uses == usesOf.end() || id.starts_with('$') || built.at(id).pass->writesScreen
                                  || (node && (current->IsSetup(node->pass) || current->IsFixed(node->pass)))
                                  || std::ranges::none_of(uses->second, &Declared::write)
                                  || std::ranges::any_of(uses->second, [&](const Declared& use) {
                                         return use.write && (seenByTheCpu.contains(use.name) || !readByAnother(id, use));
                                     });
                if (kept && needed.insert(id).second) next.push_back(id);
            }
            // What a pass that is needed reads, the passes that write it are needed for.
            while (!next.empty()) {
                const std::string_view id = next.back();
                next.pop_back();
                const auto uses = usesOf.find(id);
                if (uses == usesOf.end()) continue;
                for (const Declared& use : uses->second) {
                    if (!use.read) continue;
                    const auto by = writers.find(use.name);
                    if (by == writers.end()) continue;
                    for (const std::string* writer : by->second)
                        if (needed.insert(*writer).second) next.push_back(*writer);
                }
            }
            for (const std::string* id : awake) {
                if (needed.contains(*id)) continue;
                const auto& plan = *built.at(*id).pass;
                plan.resting->store(true);
                if (plan.entities) plan.entities->idle = true;
                restingUnread.insert(*id);
                unreadBefore.push_back(id);
            }
        }

    auto Runtime::Impl::OverwritesFirst(const std::vector<std::vector<Declared>>& declared, const std::vector<double>& pages) -> std::vector<std::size_t>
    {
            const std::size_t count = declared.size();
            std::vector<std::vector<std::size_t>> after(count);
            std::vector<std::size_t> waits(count, 0);
            for (std::size_t first = 0; first < count; ++first)
                for (std::size_t second = 0; second < count; ++second) {
                    // Of one page, or an earlier one: what a later page overwrites, it overwrites after what is before it.
                    if (first == second || pages[first] > pages[second]) continue;
                    const bool ordered = std::ranges::any_of(declared[first], [&](const Declared& made) {
                        return made.write && !made.read && std::ranges::any_of(declared[second], [&](const Declared& used) { return used.name == made.name && used.read && used.write; });
                    });
                    if (ordered) { after[first].push_back(second); ++waits[second]; }
                }
            std::vector<std::size_t> order;
            std::vector<bool> placed(count, false);
            while (order.size() < count) {
                // The earliest in the graph that waits for nothing; in a circle of them, simply the earliest.
                std::size_t next = count;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i] && waits[i] == 0) next = i;
                for (std::size_t i = 0; i < count && next == count; ++i) if (!placed[i]) next = i;
                placed[next] = true;
                order.push_back(next);
                for (const std::size_t follower : after[next]) if (waits[follower] > 0) --waits[follower];
            }
            return order;
        }

    auto Runtime::Impl::Resolve() -> void
    {
            struct Used {
                std::size_t pass = 0;
                bool read = false, write = false, dynamic = false;
                std::vector<Member> members;
                std::string key;
            };
            std::vector<Used> uses;
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) {
                const auto& plan = built.at(wanted[pass]).pass;
                for (const auto& use : plan->uses) uses.push_back({ pass, use.read, use.write, false, { use.what }, {} });
                if (!plan->entities) continue;
                const auto& slots = plan->entities->info->slots;
                for (std::size_t i = 0; i < slots.size() && i < plan->entities->members.size(); ++i)
                    if (!slots[i].sampler) uses.push_back({ pass, slots[i].read, slots[i].write, true, plan->entities->members[i], wanted[pass] + "/" + slots[i].field });
                if (!plan->entities->pictures.empty()) uses.push_back({ pass, true, false, true, plan->entities->pictures, wanted[pass] + "/pictures" });
                if (!plan->entities->storages.empty()) uses.push_back({ pass, true, true, true, plan->entities->storages, wanted[pass] + "/storages" });
            }
            // A transient's, by which object it is — what stood in for it, or the frame graph's — wherever it is used; and
            // which of those each pass was built with, for it to record nothing while that is what stood in.
            // Two the frame graph shares are one object: which of them a pass uses is which it is wired from.
            std::unordered_map<const void*, std::vector<std::string>> transientOf;
            for (const auto& [node, transient] : transients) {
                if (transient.standIn) transientOf[transient.standIn].push_back(node);
                if (transient.made && transient.made != transient.standIn) transientOf[transient.made].push_back(node);
            }
            holdsOf.clear();
            std::map<std::string, std::set<std::string>, std::less<>> upstream;
            for (auto& use : uses) {
                if (use.dynamic) continue;     // an entity's: imported as the rest of its set is
                for (auto& member : use.members) {
                    member.transient.clear();
                    const auto found = transientOf.find(member.id);
                    if (found == transientOf.end()) continue;
                    const std::string* which = found->second.size() == 1 ? &found->second.front() : nullptr;
                    if (!which) {
                        const auto& wired = TransientsUpstream(wanted[use.pass], upstream);
                        for (const auto& node : found->second) if (wired.contains(node)) { which = &node; break; }
                    }
                    if (!which) continue;
                    member.transient = *which;
                    holdsOf[wanted[use.pass]].emplace_back(*which, member.id);
                }
            }

            // Uses with a resource in common, together.
            // A transient by its node: two the frame graph shares are one object, and two things all the same.
            const auto KeyOf = [&](const Member& member) -> const void* { return member.transient.empty() ? member.id : static_cast<const void*>(&transients.at(member.transient)); };
            std::vector<std::size_t> parent(uses.size());
            for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = i;
            const std::function<std::size_t(std::size_t)> root = [&](const std::size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
            std::unordered_map<const void*, std::size_t> firstUse;
            for (std::size_t i = 0; i < uses.size(); ++i)
                for (const auto& member : uses[i].members) {
                    const auto [known, fresh] = firstUse.try_emplace(KeyOf(member), i);
                    if (!fresh) parent[root(i)] = root(known->second);
                }

            struct Group {
                bool dynamic = false;
                std::string name;
                std::vector<Member> members;
                std::set<const void*> had;
            };
            std::map<std::size_t, Group> groups;
            for (std::size_t i = 0; i < uses.size(); ++i) {
                Group& group = groups[root(i)];
                if (uses[i].dynamic && !group.dynamic) { group.dynamic = true; group.name = std::format("kgraph.set.{:x}.{}", reinterpret_cast<std::uintptr_t>(this), uses[i].key); }
                for (const auto& member : uses[i].members)
                    if (group.had.insert(KeyOf(member)).second) group.members.push_back(member);
            }
            std::set<std::string> live;
            for (auto& group : groups | std::views::values) {
                if (!group.dynamic) {
                    const Member& member = group.members.front();
                    // A transient is the frame graph's to make, by its node: in whichever of its passes is given how, below.
                    if (!member.transient.empty() && transients.contains(member.transient)) { group.name = std::format("kgraph.transient.{:x}.{}", reinterpret_cast<std::uintptr_t>(this), member.transient); continue; }
                    group.name = std::format("kgraph.{:x}", reinterpret_cast<std::uintptr_t>(member.id));
                    if (member.image.Get()) frameGraph.Import(group.name, member.image);
                    else frameGraph.Import(group.name, member.buffer);
                    continue;
                }
                live.insert(group.name);
                Set& set = sets[group.name];
                if (!set.set) {
                    set.set = std::make_shared<kor::ResourceSet>();
                    frameGraph.ImportSet(group.name, set.set);
                }
                std::vector<const void*> ids(group.had.begin(), group.had.end());
                if (ids == set.members) continue;
                set.set->Clear();
                for (const auto& member : group.members) {
                    if (member.image.Get()) set.set->Add(member.image);
                    else set.set->Add(member.buffer);
                }
                set.members = std::move(ids);
            }
            // A set nothing uses any more holds on to nothing.
            for (auto& [name, set] : sets)
                if (!live.contains(name) && !set.members.empty()) { set.set->Clear(); set.members.clear(); }

            std::vector<std::vector<Declared>> declared(wanted.size());
            for (std::size_t i = 0; i < uses.size(); ++i) {
                const Group& group = groups.at(root(i));
                const std::string& name = group.name;
                auto& list = declared[uses[i].pass];
                const auto known = std::ranges::find(list, name, &Declared::name);
                const std::string transient = name.starts_with("kgraph.transient.") ? group.members.front().transient : std::string();
                if (known == list.end()) list.push_back({ name, uses[i].read, uses[i].write, transient });
                else { known->read = known->read || uses[i].read; known->write = known->write || uses[i].write; }
            }
            usesOf.clear();
            restsBefore.clear();
            unreadBefore.clear();
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) usesOf[wanted[pass]] = declared[pass];
            seenByTheCpu.clear();
            for (const auto& group : groups | std::views::values)
                if (std::ranges::any_of(group.members, [](const Member& member) { return member.buffer.Get() && member.buffer->IsHostVisible(); }))
                    seenByTheCpu.insert(group.name);

            // The frame graph runs everything that writes a resource before anything that only reads it. But a pass
            // may read one so as to make what a later writer of it takes — the picture, traced for reflections that
            // are then added to the picture — and after every writer is then after itself. Such a reader takes its
            // place among the writers, which run in the order the graph has them: after those before it.
            {
                const std::size_t count = declared.size();
                // What each pass feeds: the passes that read something it writes.
                std::vector<std::vector<std::size_t>> feeds(count);
                for (std::size_t from = 0; from < count; ++from)
                    for (std::size_t to = 0; to < count; ++to) {
                        if (from == to) continue;
                        const bool fed = std::ranges::any_of(declared[from], [&](const Declared& made) {
                            return made.write && std::ranges::any_of(declared[to], [&](const Declared& used) { return used.name == made.name && used.read; });
                        });
                        if (fed) feeds[from].push_back(to);
                    }
                std::vector<std::pair<std::size_t, std::size_t>> among;     // a pass, and which of its uses
                for (std::size_t pass = 0; pass < count; ++pass)
                    for (std::size_t use = 0; use < declared[pass].size(); ++use) {
                        if (declared[pass][use].write) continue;
                        std::vector<bool> reached(count, false);
                        std::vector<std::size_t> next { pass };
                        reached[pass] = true;
                        bool feedsAWriter = false;
                        while (!next.empty() && !feedsAWriter) {
                            const std::size_t from = next.back();
                            next.pop_back();
                            for (const std::size_t to : feeds[from]) {
                                if (reached[to]) continue;
                                reached[to] = true;
                                next.push_back(to);
                                if (std::ranges::any_of(declared[to], [&](const Declared& theirs) { return theirs.name == declared[pass][use].name && theirs.write; })) { feedsAWriter = true; break; }
                            }
                        }
                        if (feedsAWriter) among.emplace_back(pass, use);
                    }
                for (const auto& [pass, use] : among) declared[pass][use].write = true;
            }

            // A copy of what a page begins with takes its place among the writers, before the page's own: as a reader it
            // would be after every one of them.
            std::vector<double> pages(wanted.size(), current ? static_cast<double>(current->passes.size()) : 0.0);   // in none: after them all
            for (std::size_t pass = 0; pass < wanted.size(); ++pass) {
                if (const Node* node = current ? CurrentNode(wanted[pass]) : nullptr)
                    if (const auto at = std::ranges::find(current->passes, node->pass); at != current->passes.end())
                        pages[pass] = static_cast<double>(at - current->passes.begin());
                if (before.contains(wanted[pass])) {
                    pages[pass] -= 0.5;
                    for (auto& use : declared[pass]) use.write = true;
                }
            }
            std::vector<std::string> order;
            std::vector<std::vector<Declared>> ordered;
            for (const std::size_t index : OverwritesFirst(declared, pages)) {
                order.push_back(wanted[index]);
                ordered.push_back(std::move(declared[index]));
            }
            StepAgain(order, ordered);
            // Each transient made by one pass: the first of them, in the order they go in, that writes it — or, written by
            // none, the first that uses it. The frame graph runs that one first of them.
            {
                std::set<std::string, std::less<>> made;
                for (const bool writer : { true, false })
                    for (auto& uses : ordered)
                        for (Declared& use : uses) {
                            if (use.transient.empty() || (writer && !use.write) || made.contains(use.transient)) continue;
                            const auto how = transients.find(use.transient);
                            if (how == transients.end()) continue;
                            use.image = how->second.image;
                            use.buffer = how->second.buffer;
                            made.insert(use.transient);
                        }
            }
            if (order != passOrder) {
                // Which passes there are, or their order, changed: the order they are added in breaks ties, so all go in again.
                for (auto* pass : passes | std::views::values) frameGraph.Remove(*pass);
                passes.clear();
                for (std::size_t i = 0; i < order.size(); ++i) {
                    GraphPass& pass = frameGraph.Add<GraphPass>(order[i], built.at(StepOf(order[i]).first).pass, std::move(ordered[i]));
                    pass.board = board;
                    if (const auto holds = holdsOf.find(StepOf(order[i]).first); holds != holdsOf.end()) pass.holds = holds->second;
                    if (const std::uint32_t step = StepOf(order[i]).second; step > 0) { pass.step = step; pass.steps = fixedNow; }
                    passes.emplace(order[i], &pass);
                }
                passOrder = std::move(order);
                return;
            }
            bool declarations = false;
            for (std::size_t i = 0; i < order.size(); ++i) {
                GraphPass* pass = passes.at(order[i]);
                const auto& plan = built.at(StepOf(order[i]).first).pass;
                const auto holds = holdsOf.find(StepOf(order[i]).first);
                pass->holds = holds != holdsOf.end() ? holds->second : std::vector<std::pair<std::string, const void*>> {};
                if (pass->plan != plan) {
                    declarations = declarations || pass->plan->readsScreen != plan->readsScreen || pass->plan->writesScreen != plan->writesScreen
                                || pass->plan->async != plan->async;   // which queue it is on is said in its Setup
                    pass->plan = plan;
                }
                if (pass->declared != ordered[i]) {
                    pass->declared = std::move(ordered[i]);
                    declarations = true;
                }
            }
            if (declarations) frameGraph.Invalidate();
        }

    auto Runtime::Impl::StepAgain(std::vector<std::string>& order, std::vector<std::vector<Declared>>& ordered) const -> void
    {
            if (!current || current->fixed.empty() || current->fixedStepsAtMost < 1) return;
            std::vector<std::size_t> fixedAt;
            for (std::size_t i = 0; i < order.size(); ++i)
                if (const Node* node = CurrentNode(order[i]); node && !order[i].starts_with('$') && current->IsFixed(node->pass)) fixedAt.push_back(i);
            if (fixedAt.empty()) return;
            std::set<std::string> written;
            for (const std::size_t i : fixedAt)
                for (const auto& use : ordered[i]) if (use.write) written.insert(use.name);
            for (const std::size_t i : fixedAt)
                for (auto& use : ordered[i]) if (written.contains(use.name)) use.write = true;
            std::vector<std::string> steppedOrder;
            std::vector<std::vector<Declared>> stepped;
            for (std::size_t i = 0; i < order.size(); ++i) {
                steppedOrder.push_back(order[i]);
                stepped.push_back(ordered[i]);
                if (i != fixedAt.back()) continue;
                for (int step = 1; step < current->fixedStepsAtMost; ++step)
                    for (const std::size_t j : fixedAt) {
                        steppedOrder.push_back(std::format("{}\nstep {}", order[j], step + 1));
                        stepped.push_back(ordered[j]);
                    }
            }
            order = std::move(steppedOrder);
            ordered = std::move(stepped);
        }

    auto Runtime::Impl::StepOf(const std::string& name) -> std::pair<std::string, std::uint32_t>
    {
            const auto at = name.find("\nstep ");
            if (at == std::string::npos) return { name, 0 };
            return { name.substr(0, at), static_cast<std::uint32_t>(std::stoul(name.substr(at + 6)) - 1) };
        }

    auto Runtime::Impl::Install(const Graph& graph) -> void
    {
            wanted.clear();
            restsBefore.clear();    // (what RestUnread keeps points into it)
            unreadBefore.clear();
            for (const auto& node : graph.nodes)
                if (const auto it = built.find(node.id); it != built.end() && it->second.status.ok && it->second.pass) wanted.push_back(node.id);
            // In the order of the passes they are part of: what is drawn first where what is used does not say. One
            // that is in no pass keeps its place among the rest of those.
            const auto place = [&](const std::string& id) {
                const Node* node = graph.Find(id);
                const auto at = node ? std::ranges::find(graph.passes, node->pass) : graph.passes.end();
                return at == graph.passes.end() ? graph.passes.size() : static_cast<std::size_t>(at - graph.passes.begin());
            };
            std::ranges::stable_sort(wanted, {}, place);
            // A pass of the fixed step rests until a frame says it has a step: the frame it is put in is not readied as
            // others are (EachFrame), and would run it once whether or not it had one.
            for (const auto& id : wanted)
                if (const Node* node = graph.Find(id); node && graph.IsFixed(node->pass)) built.at(id).pass->resting->store(true);
            // What the passes used is told again below: what was told of the graph before is of other resources, and
            // nothing rests for want of a reader until it is.
            usesOf.clear();
            PrepareAll();
            Resolve();
        }

    auto Runtime::Impl::ResetStale(Graph& graph) -> void
    {
            // Each pipeline's parameters and push constants as its shaders have them now: what its draws can be given.
            std::map<std::string, std::pair<Interface, std::set<std::string>>> now;
            for (auto& node : graph.nodes) {
                const NodeType* type = FindType(node.type);
                if (!type || !type->builder || !IsPipeline(type->name)) continue;
                std::vector<kor::ResourceRef<const kor::Shader>> shaders;
                const auto outputOf = [&](const Input& input) -> Held {
                    if (!input.link) return nullptr;
                    const auto source = built.find(input.link->node);
                    if (source == built.end()) return nullptr;
                    const auto output = source->second.outputs.find(input.link->output);
                    return output == source->second.outputs.end() ? nullptr : output->second.resource;
                };
                for (const auto& input : node.inputs | std::views::values)
                    if (const Held resource = outputOf(input); resource && resource->kind == "Shader" && resource->handle)
                        shaders.push_back(kor::capi::ShaderOf(resource->handle));
                // And those it names itself, as they were when it was last built.
                if (const auto it = built.find(node.id); it != built.end() && it->second.pipeline)
                    for (const Held& named : it->second.pipeline->named)
                        if (named && named->handle) shaders.push_back(kor::capi::ShaderOf(named->handle));
                if (shaders.empty() || !std::ranges::all_of(shaders, [](const auto& shader) { return shader.Valid(); })) continue;   // one that does not compile says nothing yet
                const Interface interface = Reflect(shaders, Kinds(), world);
                std::set<std::string> pushes;
                for (const auto& shader : shaders)
                    for (const auto& block : shader->BlockLayout().pushConstants | std::views::values)
                        for (const auto& member : block.members) pushes.insert("push." + member.name);
                now[node.id] = { interface, std::move(pushes) };
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    const std::string& name = it->first;
                    bool stale = false;
                    if (name.starts_with("param.") || name.starts_with("constant.")) {
                        const Port* port = interface.Find(name);
                        stale = !port;
                        if (port && port->kind == PortKind::eResource)
                            if (const Held given = outputOf(it->second); given && !given->screen)
                                stale = port->type == "Image" ? given->kind != "Image" && given->kind != "ImageView" : given->kind != port->type;
                    }
                    if (!stale) { ++it; continue; }
                    reset.push_back(std::format("{}.{}", node.id, name));
                    it = node.inputs.erase(it);
                }
            }
            // A draw's own, the same way: what its pipeline's shaders no longer have is taken off it.
            for (auto& node : graph.nodes) {
                if (node.type != "Draw" && node.type != "Dispatch" && node.type != "TraceRays") continue;
                const auto pipeline = node.inputs.find("pipeline");
                const Node* of = pipeline != node.inputs.end() && pipeline->second.link ? Origin(pipeline->second.link->node) : nullptr;
                const auto found = of ? now.find(of->id) : now.end();
                if (found == now.end()) continue;
                const auto& [interface, pushes] = found->second;
                for (auto it = node.inputs.begin(); it != node.inputs.end();) {
                    const std::string& name = it->first;
                    const bool stale = (name.starts_with("param.") && !interface.Find(name)) || (name.starts_with("push.") && !pushes.contains(name));
                    if (!stale) { ++it; continue; }
                    reset.push_back(std::format("{}.{}", node.id, name));
                    it = node.inputs.erase(it);
                }
            }
        }
}
