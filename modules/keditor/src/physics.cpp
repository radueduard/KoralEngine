// The engine's physics scripts, the bridge between the ECS and kphysics.
//
//   RigidBody     a body physics moves: static, dynamic or kinematic; its density (or mass), material and damping.
//   Collider      its shape about the entity's position, before its scale: sphere, box, capsule (along Y), plane, cone
//                 (along Y, about its middle, as $cone is) or mesh (the triangles of the entity's Mesh; static or kinematic).
//   Joint         a ball, hinge, distance or rope joint from this body to another, or to the world.
//   PhysicsSimulation  put on any one entity: while the scene plays it gathers every top-level entity with a RigidBody and a
//                 Collider, steps a kphysics::Simulation on each fixed step, and writes the bodies' positions and
//                 orientations back. RigidBody, Collider and Joint are data only: it reads them all at once.
//
// The simulation is built from the ECS when there is none: at the first fixed step of Play, and when bodies are added or
// removed. A body moved by something else (the gizmo, a script, the inspector) is put where it was put, and stopped; a
// kinematic body is given the velocity that takes it where it is told to be in one step. Velocities are written back to
// each RigidBody every frame.
//
// They were Kotlin scripts of the physics project, over its Kotlin core, which kphysics is the port of; the fields, and
// so the scenes saved with them, are the same.

#include "physics.h"

#include <keditor/document.h>
#include <keditor/play.h>

#include <kecs/kecs.h>
#include <kgraph/entities.h>
#include <kgraph/meshes.h>
#include <kgraph/runtime.h>
#include <kgraph/scene.h>
#include <kphysics/kphysics.h>
#include <kscript/scripts.h>

#include <debugDraw.h>
#include <log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace keditor
{
    namespace
    {
        enum Kind : int { Static = 0, Dynamic = 1, Kinematic = 2 };

        struct RigidBody {
            int kind = Dynamic;                 // 0 static, 1 dynamic, 2 kinematic (moved by you; pushes, is not pushed)
            float density = 1000.f;             // kg/m³: with the collider's shape, the mass
            float mass = 0.f;                   // kg; above 0 it replaces what density and shape make
            float friction = 0.6f;
            float restitution = 0.2f;
            float linearDamping = 0.f;
            float angularDamping = 0.05f;
            float gravityScale = 1.f;
            kecs::Vec3 velocity;                // m/s: where it starts, and what it is doing while it plays
            kecs::Vec3 angularVelocity;         // rad/s, about the world's axes
        };

        struct Collider {
            int shape = kphysics::Box;          // 0 sphere, 1 box, 2 capsule (along Y), 3 plane (on XZ, facing +Y; static only),
                                                // 4 cone (along Y, point up), 5 mesh (the entity's Mesh; static or kinematic)
            float radius = 0.5f;                // a sphere's, a capsule's or a cone's (at its base)
            kecs::Vec3 halfExtents { 0.5f, 0.5f, 0.5f };
            float height = 1.f;                 // a capsule's overall height, half spheres included, as $capsule is made; a cone's
            float thickness = 0.f;              // a mesh's: how deep a solid wall its faces are, holding what goes behind them
        };

        struct Joint {
            int kind = 0;                       // 0 ball, 1 hinge, 2 distance (a rod), 3 rope (only pulls)
            kecs::Entity other;                 // the other body; none: the world, fixed where the joint is at Play
            kecs::Vec3 anchor;                  // where it is, in this entity's own frame (before its scale)
            kecs::Vec3 otherAnchor;             // distance and rope: the other end, in the other's frame (the world's, for none)
            kecs::Vec3 axis { 0.f, 1.f, 0.f };  // hinge: what it turns about, in this entity's frame
            bool limited = false;               // hinge: turns only between the two angles, degrees from where it starts
            float lowerDegrees = -45.f;
            float upperDegrees = 45.f;
            float motorSpeed = 0.f;             // hinge: a motor at this speed (rad/s), with at most maxMotorTorque (N·m)
            float maxMotorTorque = 0.f;
            bool collideConnected = false;
        };

        enum Mode : int { Cpu = 0, Gpu = 1 };

        struct PhysicsSimulation {
            int mode = Cpu;                     // CPU: kphysics, here; GPU: the graph's XpbdStep, on the GPU
            kecs::Vec3 gravity { 0.f, -9.81f, 0.f };
            int substeps = 2;                   // two keep a stack of ten standing
            int iterations = 10;                // solver passes each substep
            bool allowSleep = true;
            int broadphase = 3;                 // 0 every pair, 1 sweep and prune, 2 grid, 3 dynamic AABB tree
            int gyroscopic = 0;                 // 0 left out, 1 explicit, 2 implicit
            int threads = 0;                    // solving the contacts: 0 as many as there are cores (up to 8), 1 one
            bool drawColliders = true;
            bool drawVelocities = false;
            bool drawContacts = true;           // each contact point of the last step and its normal
            bool drawJoints = true;
            // Shown while it plays.
            int bodyCount = 0;
            int contactCount = 0;
            int sleepingCount = 0;
            float stepMilliseconds = 0.f;
            float broadphaseMilliseconds = 0.f;
            float narrowphaseMilliseconds = 0.f;
            float solverMilliseconds = 0.f;
        };

        /** What one PhysicsSimulation keeps while the scene plays: its simulation, and the bodies it was built from. */
        struct State {
            std::unique_ptr<kphysics::Simulation> sim;
            int mode = Cpu;                     // how it ran the step before: a switch to the GPU starts it from the world
            std::vector<kecs::Entity> bodies;
            std::size_t found = 0;              // entities with both components when it was built
            std::vector<int> kind, shape;
            std::vector<kphysics::Vec3> size;
            /// From each entity's origin to its body's centre of mass, in the entity's frame: a cone's is below its middle.
            std::vector<kphysics::Vec3> offset;
            std::vector<kphysics::Vec3> placed; // where the last step put each: anything else is something else moving it
            std::vector<kphysics::Quat> turned;
            std::set<std::string> warned;

            void Warn(const std::string& message)
            {
                if (warned.insert(message).second) kor::log::Warn("PhysicsSimulation: {}", message);
            }
        };

        /** The component ids, and each PhysicsSimulation's state, of one document. */
        struct Host {
            Document* document = nullptr;
            kecs::ComponentId rigidBody = kecs::NoComponent, collider = kecs::NoComponent, joint = kecs::NoComponent, world = kecs::NoComponent;
            /// Mesh colliders' triangles, by their mesh's name and the entity's scale: one mesh on many entities is held once.
            std::map<std::tuple<std::string, float, float, float>, std::shared_ptr<const kphysics::TriangleMesh>> meshes;
            std::map<std::pair<std::uint32_t, std::uint32_t>, State> states;
            State shown;        // what a collider drawn in the editor warns of: once each
        };

        /**
         * Every document's host, by its address. A document's scripts call back into its host until the document is
         * destroyed (its Shutdown destroys every PhysicsSimulation, which frees their simulations), so a host is replaced only
         * when a new document has the same address, and the old one is certainly gone: what is left behind is a few ids.
         */
        std::map<const Document*, std::unique_ptr<Host>>& Hosts()
        {
            static std::map<const Document*, std::unique_ptr<Host>> hosts;
            return hosts;
        }

        kphysics::Vec3 ToPhysics(const kecs::Vec3 v) { return { v.x, v.y, v.z }; }
        kphysics::Quat ToPhysics(const kecs::Quat q) { return { q.x, q.y, q.z, q.w }; }
        kecs::Vec3 ToKecs(const kphysics::Vec3 v) { return { v.x, v.y, v.z }; }
        kecs::Quat ToKecs(const kphysics::Quat q) { return { q.x, q.y, q.z, q.w }; }
        kor::Vec3 ToKor(const kphysics::Vec3 v) { return kor::Vec3(v.x, v.y, v.z); }

        std::pair<std::uint32_t, std::uint32_t> Key(const kecs::Entity e) { return { e.index, e.generation }; }

        /** Every top-level entity with a RigidBody and a Collider, by index: the same order every run. */
        std::vector<kecs::Entity> Bodies(const Host& host, kecs::World& world, State& state, std::size_t& found)
        {
            std::vector<kecs::Entity> top;
            kecs::QueryDescription query;
            query.all = { host.rigidBody, host.collider };
            found = 0;
            world.Each(query, [&](const kecs::ChunkView& chunk) {
                for (std::size_t row = 0; row < chunk.Count(); ++row) {
                    if (!chunk.AllMatch() && !chunk.Matches(row)) continue;
                    ++found;
                    const kecs::Entity e = chunk.EntityAt(row);
                    if (world.ParentOf(e).IsNull()) top.push_back(e);
                    else state.Warn(world.NameOf(e) + " is under another entity: physics moves only bodies at the top of the hierarchy, so it is left out");
                }
            });
            std::ranges::sort(top, [](const kecs::Entity a, const kecs::Entity b) { return a.index < b.index; });
            return top;
        }

        /** Each Joint on a body, between its other (or the world) and that body, as the bodies are when it is built. */
        void AddJoints(const Host& host, kecs::World& world, State& state)
        {
            kphysics::Simulation& sim = *state.sim;
            std::map<std::uint32_t, int> number;            // an entity's index: its body's number
            for (int b = 0; b < static_cast<int>(state.bodies.size()); ++b) number[state.bodies[b].index] = b;
            for (int b = 0; b < static_cast<int>(state.bodies.size()); ++b) {
                const auto* joint = static_cast<const Joint*>(world.Get(state.bodies[b], host.joint));
                if (!joint) continue;
                // The other is A and this entity B, so a hinge's angle is this entity's turn relative to its other.
                int other = -1;
                if (!joint->other.IsNull()) {
                    const auto found = number.find(joint->other.index);
                    if (found != number.end()) other = found->second;
                    else state.Warn(world.NameOf(state.bodies[b]) + ": its joint's other is no body here, so it is held to the world");
                }
                const kphysics::Quat turn = sim.bodies.orientation[b];
                const kphysics::Vec3 at = sim.bodies.position[b] + kphysics::Rotate(turn, ToPhysics(joint->anchor) - state.offset[b]);
                int j = 0;
                switch (joint->kind) {
                case 1: {
                    j = sim.joints.AddHinge(sim, other, b, at, kphysics::Rotate(turn, ToPhysics(joint->axis)));
                    constexpr float radians = std::numbers::pi_v<float> / 180.f;
                    if (joint->limited) sim.joints.SetLimits(j, joint->lowerDegrees * radians, joint->upperDegrees * radians);
                    if (joint->maxMotorTorque > 0.f) sim.joints.SetMotor(j, joint->motorSpeed, joint->maxMotorTorque);
                    break;
                }
                case 2:
                case 3: {
                    kphysics::Vec3 end = ToPhysics(joint->otherAnchor);
                    if (other >= 0) end = sim.bodies.position[other] + kphysics::Rotate(sim.bodies.orientation[other], end - state.offset[other]);
                    j = sim.joints.AddDistance(sim, other, b, end, at, joint->kind == 3);
                    break;
                }
                default: j = sim.joints.AddBall(sim, other, b, at); break;
                }
                if (joint->collideConnected) sim.joints.SetCollideConnected(j, true);
            }
        }

        /**
         * The triangles of entity @p e's Mesh, scaled: a default mesh's made here, a model's read back from the device.
         * Kept by name and scale, so that one mesh on many entities is held once. Null, and a warning, when there are none.
         */
        std::shared_ptr<const kphysics::TriangleMesh> MeshOf(Host& host, kecs::World& world, const kecs::Entity e, const kecs::Vec3 scale, State& state)
        {
            const std::string& name = world.NameOf(e);
            const kecs::ComponentId meshId = world.Find("Mesh");
            const auto* drawn = meshId == kecs::NoComponent ? nullptr : static_cast<const kgraph::Mesh*>(world.Get(e, meshId));
            if (!drawn || !drawn->mesh || drawn->mesh->node.empty()) {
                state.Warn(name + ": a mesh collider needs a Mesh on its entity, a default one or a model's: it has none, so it collides with nothing");
                return nullptr;
            }
            const std::string& id = drawn->mesh->node;
            auto& kept = host.meshes[{ id, scale.x, scale.y, scale.z }];
            if (kept) return kept;
            std::vector<kphysics::Vec3> positions;
            std::vector<std::uint32_t> indices;
            // A default mesh ($cube) is made here; any other (a model's, a script's $script/...) read back from the device.
            const auto shape = id.starts_with('$') ? kgraph::ShapeData(std::string_view(id).substr(1)) : std::nullopt;
            if (shape) {
                const auto& data = shape;
                for (const auto& v : data->vertices) positions.push_back({ v.position[0], v.position[1], v.position[2] });
                indices = data->indices;
            } else {
                std::string error;
                const auto data = host.document->Runtime().ReadMesh(id, &error);
                const kgraph::VertexAttribute* position = data ? data->Find("POSITION") : nullptr;
                if (!position || position->count < 3) {
                    state.Warn(name + ": its mesh's triangles could not be read (" + (error.empty() ? "no POSITION" : error) + "), so it collides with nothing");
                    return nullptr;
                }
                for (std::uint32_t v = 0; v < data->vertexCount; ++v) {
                    float p[3];
                    std::memcpy(p, &position->words[static_cast<std::size_t>(v) * position->count], sizeof p);
                    positions.push_back({ p[0], p[1], p[2] });
                }
                indices = data->indices;
                if (indices.empty())
                    for (std::uint32_t v = 0; v < data->vertexCount; ++v) indices.push_back(v);
            }
            for (kphysics::Vec3& p : positions) p = { p.x * scale.x, p.y * scale.y, p.z * scale.z };
            // Mirrored by an odd number of negative scales: its triangles turn the other way, and must be turned back.
            if (((scale.x < 0.f) + (scale.y < 0.f) + (scale.z < 0.f)) % 2 == 1)
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
            kept = std::make_shared<kphysics::TriangleMesh>(positions, indices);
            return kept;
        }

        /** A simulation of the world's bodies as they are now. */
        void Build(Host& host, kecs::World& world, State& state)
        {
            state.bodies = Bodies(host, world, state, state.found);
            const int n = static_cast<int>(state.bodies.size());
            state.sim = std::make_unique<kphysics::Simulation>(std::max(n, 1));
            kphysics::Simulation& sim = *state.sim;
            state.kind.assign(n, Static);
            state.shape.assign(n, kphysics::Box);
            state.size.assign(n, {});
            state.offset.assign(n, {});
            state.placed.assign(n, {});
            state.turned.assign(n, {});
            for (int b = 0; b < n; ++b) {
                const kecs::Entity e = state.bodies[b];
                const auto& body = *static_cast<const RigidBody*>(world.Get(e, host.rigidBody));
                const auto& collider = *static_cast<const Collider*>(world.Get(e, host.collider));
                const std::string& name = world.NameOf(e);
                const kecs::Vec3 scale = world.ScaleOf(e);
                const float sx = std::abs(scale.x), sy = std::abs(scale.y), sz = std::abs(scale.z);
                int kind = std::clamp(body.kind, 0, 2);
                const int shape = std::clamp(collider.shape, 0, 5);
                if (shape == kphysics::Plane && kind != Static) {
                    state.Warn(name + ": a plane can only be static, so it is");
                    kind = Static;
                }
                if (shape == kphysics::Mesh && kind == Dynamic) {
                    state.Warn(name + ": a mesh collider can only be static or kinematic (nothing turns a hollow surface), so it is static");
                    kind = Static;
                }
                std::shared_ptr<const kphysics::TriangleMesh> triangles;
                kphysics::Vec3 size, inertia { 1.f, 1.f, 1.f };
                float made = 0.f;
                switch (shape) {
                case kphysics::Sphere:
                    if (sx != sy || sy != sz) state.Warn(name + ": a sphere scaled unevenly takes its largest scale");
                    size.x = collider.radius * std::max({ sx, sy, sz });
                    made = kphysics::mass::Sphere(body.density, size.x, inertia);
                    break;
                case kphysics::Box:
                    size = { collider.halfExtents.x * sx, collider.halfExtents.y * sy, collider.halfExtents.z * sz };
                    made = kphysics::mass::Box(body.density, size, inertia);
                    break;
                case kphysics::Capsule:
                    if (sx != sz) state.Warn(name + ": a capsule scaled unevenly across its axis takes the larger scale");
                    size.x = collider.radius * std::max(sx, sz);
                    size.y = std::max(collider.height * sy / 2.f - size.x, 0.f);
                    made = kphysics::mass::Capsule(body.density, size.x, size.y, inertia);
                    break;
                case kphysics::Cone:
                    if (sx != sz) state.Warn(name + ": a cone scaled unevenly across its axis takes the larger scale");
                    size = { collider.radius * std::max(sx, sz), collider.height * sy, 0.f };
                    made = kphysics::mass::Cone(body.density, size.x, size.y, inertia);
                    // Its centre of mass is a quarter of its height above its base: below the middle the entity is at.
                    state.offset[b] = { 0.f, -0.25f * size.y, 0.f };
                    break;
                case kphysics::Mesh:
                    triangles = MeshOf(host, world, e, scale, state);
                    break;
                default:
                    size = { 0.5f * sx, 0.f, 0.5f * sz };
                    break;
                }
                float mass = made;
                if (body.mass > 0.f && made > 0.f) {
                    inertia = inertia * (body.mass / made);
                    mass = body.mass;
                }
                if (kind != Dynamic) mass = 0.f;
                const kphysics::Quat turn = ToPhysics(world.OrientationOf(e));
                const kphysics::Vec3 at = ToPhysics(world.PositionOf(e)) + kphysics::Rotate(turn, state.offset[b]);
                sim.Add(at, turn, mass, inertia, { .linearDamping = body.linearDamping, .angularDamping = body.angularDamping,
                                                   .gravityScale = body.gravityScale, .friction = body.friction,
                                                   .restitution = body.restitution, .kinematic = kind == Kinematic });
                if (shape == kphysics::Mesh) sim.colliders.AddMesh(std::move(triangles), std::max(collider.thickness, 0.f));
                else sim.colliders.Add(shape, size);
                if (kind == Dynamic) {
                    sim.bodies.velocity[b] = ToPhysics(body.velocity);
                    sim.bodies.angularVelocity[b] = ToPhysics(body.angularVelocity);
                }
                state.kind[b] = kind;
                state.shape[b] = shape;
                state.size[b] = size;
                state.placed[b] = ToPhysics(world.PositionOf(e));
                state.turned[b] = sim.bodies.orientation[b];
            }
            AddJoints(host, world, state);
        }

        /** Whether body @p b is somewhere other than where the last step put it (the ECS keeps a turn made length 1 again). */
        bool Moved(const State& state, const int b, const kphysics::Vec3 at, const kphysics::Quat turn)
        {
            for (int c = 0; c < 3; ++c)
                if (std::abs(at[c] - state.placed[b][c]) > 1e-5f) return true;
            const kphysics::Quat t = state.turned[b];
            return std::abs(turn.x - t.x) > 1e-5f || std::abs(turn.y - t.y) > 1e-5f || std::abs(turn.z - t.z) > 1e-5f || std::abs(turn.w - t.w) > 1e-5f;
        }

        /**
         * What moved a body since the last step. A static or dynamic body moved is put where it was put (a dynamic one
         * stopped, and woken); a kinematic body is given the velocity that takes it there in one step.
         */
        void Follow(kecs::World& world, State& state, const float step)
        {
            kphysics::Simulation& sim = *state.sim;
            bool turnedAny = false;
            for (int b = 0; b < static_cast<int>(state.bodies.size()); ++b) {
                const kphysics::Vec3 at = ToPhysics(world.PositionOf(state.bodies[b]));
                const kphysics::Quat turn = ToPhysics(world.OrientationOf(state.bodies[b]));
                const kphysics::Vec3 centre = at + kphysics::Rotate(turn, state.offset[b]);
                if (state.kind[b] == Kinematic) {
                    sim.bodies.velocity[b] = (centre - sim.bodies.position[b]) / step;
                    // The turn from where it is to where it is told to be, the short way round.
                    const kphysics::Quat delta = turn * kphysics::Conjugate(sim.bodies.orientation[b]);
                    const float sign = delta.w < 0.f ? -1.f : 1.f;
                    sim.bodies.angularVelocity[b] = kphysics::Vec3 { delta.x, delta.y, delta.z } * (sign * 2.f / step);
                    state.placed[b] = at;
                    state.turned[b] = turn;
                    continue;
                }
                if (!Moved(state, b, at, turn)) continue;
                sim.bodies.position[b] = centre;
                sim.bodies.orientation[b] = turn;
                sim.bodies.velocity[b] = {};
                sim.bodies.angularVelocity[b] = {};
                sim.Wake(b);
                turnedAny = true;
            }
            if (turnedAny) sim.bodies.UpdateWorldInertia();
        }

        kphysics::BroadphaseKind BroadphaseOf(const int setting)
        {
            switch (std::clamp(setting, 0, 3)) {
            case 0: return kphysics::BroadphaseKind::AllPairs;
            case 1: return kphysics::BroadphaseKind::SweepAndPrune;
            case 2: return kphysics::BroadphaseKind::Grid;
            default: return kphysics::BroadphaseKind::Tree;
            }
        }

        bool GiveTheGraphTheGpusPhysics(Document& document);

        void FixedUpdate(void* user, const kecs::Entity entity, void* data, const float step)
        {
            Host& host = *static_cast<Host*>(user);
            kecs::World& world = host.document->World();
            State& state = host.states[Key(entity)];
            const PhysicsSimulation settings = *static_cast<const PhysicsSimulation*>(data);
            // On the GPU: the graph runs the bodies, and nothing here does — the graph is given what does that when it has
            // not got it, and what it made of the world is made again when the step was the CPU's.
            if (settings.mode == Gpu) {
                const bool switched = state.mode != Gpu;
                state.mode = Gpu;
                state.sim.reset();
                const bool added = GiveTheGraphTheGpusPhysics(*host.document);
                if (switched && !added) host.document->Runtime().RunSetupAgain();
                return;
            }
            state.mode = Cpu;
            if (!state.sim) Build(host, world, state);
            if (state.bodies.empty()) return;
            kphysics::Simulation& sim = *state.sim;
            Follow(world, state, step);
            sim.gravity = ToPhysics(settings.gravity);
            sim.substeps = std::max(1, settings.substeps);
            sim.solver.iterations = std::max(1, settings.iterations);
            sim.islands.allowSleep = settings.allowSleep;
            sim.broadphase.kind = BroadphaseOf(settings.broadphase);
            sim.gyroscopic = static_cast<kphysics::Gyroscopic>(std::clamp(settings.gyroscopic, 0, 2));
            sim.threads = std::clamp(settings.threads, 0, 64);

            sim.Step(step);

            for (int b = 0; b < static_cast<int>(state.bodies.size()); ++b) {
                if (state.kind[b] == Kinematic) {
                    // Where it was told to be, exactly: integrating its velocity only gets near.
                    sim.bodies.position[b] = state.placed[b] + kphysics::Rotate(state.turned[b], state.offset[b]);
                    sim.bodies.orientation[b] = state.turned[b];
                    continue;
                }
                if (state.kind[b] != Dynamic) continue;
                world.SetPosition(state.bodies[b], ToKecs(sim.bodies.position[b] - kphysics::Rotate(sim.bodies.orientation[b], state.offset[b])));
                world.SetOrientation(state.bodies[b], ToKecs(sim.bodies.orientation[b]));
                // What the world keeps, made length 1 again: what the next step compares with.
                state.placed[b] = ToPhysics(world.PositionOf(state.bodies[b]));
                state.turned[b] = ToPhysics(world.OrientationOf(state.bodies[b]));
            }
            if (auto* shown = static_cast<PhysicsSimulation*>(world.Mutable(entity, host.world))) {
                shown->bodyCount = static_cast<int>(state.bodies.size());
                shown->stepMilliseconds = sim.timing.step;
                shown->broadphaseMilliseconds = sim.timing.broadphase;
                shown->narrowphaseMilliseconds = sim.timing.narrowphase;
                shown->solverMilliseconds = sim.timing.solve;
            }
        }

        // ---- drawing --------------------------------------------------------------------------------------------------

        const kor::Vec4 Grey(0.6f, 0.6f, 0.6f, 1.f), Green(0.3f, 0.9f, 0.4f, 1.f), Blue(0.4f, 0.6f, 1.f, 1.f), Yellow(1.f, 0.85f, 0.2f, 1.f),
                        Red(1.f, 0.25f, 0.2f, 1.f), White(1.f, 1.f, 1.f, 1.f), Asleep(0.2f, 0.45f, 0.25f, 1.f);

        kor::DebugStyle Coloured(const kor::Vec4& colour, const bool onTop = false)
        {
            kor::DebugStyle style;
            style.color = colour;
            style.onTop = onTop;
            return style;
        }

        void Draw(kor::DebugDraw& debug, const State& state, const PhysicsSimulation& settings)
        {
            const kphysics::Simulation& sim = *state.sim;
            for (int b = 0; b < static_cast<int>(state.bodies.size()); ++b) {
                const kphysics::Vec3 at = sim.bodies.position[b];
                if (settings.drawColliders) {
                    const kor::DebugStyle style = Coloured(state.kind[b] == Static ? Grey : state.kind[b] == Kinematic ? Blue
                                                           : !sim.awake[b] ? Asleep : Green);
                    const kphysics::Vec3 size = state.size[b];
                    if (state.shape[b] == kphysics::Sphere) {
                        debug.Sphere(ToKor(at), size.x, style);
                    } else if (state.shape[b] == kphysics::Cone) {
                        const kphysics::Quat q = sim.bodies.orientation[b];
                        debug.Cone(ToKor(at + kphysics::Rotate(q, { 0.f, -0.25f * size.y, 0.f })), ToKor(at + kphysics::Rotate(q, { 0.f, 0.75f * size.y, 0.f })),
                                   size.x, style);
                    } else if (state.shape[b] == kphysics::Mesh) {
                        // Its sharp edges: what a body is pushed by along their own way, the folds and the mesh's rim.
                        if (const kphysics::TriangleMesh* mesh = sim.colliders.mesh[b].get()) {
                            const kphysics::Quat q = sim.bodies.orientation[b];
                            int lines = 0;
                            for (const auto& t : mesh->Triangles())
                                for (int e = 0; e < 3 && lines < 4096; ++e)
                                    if (t.sharp & (1u << e)) {
                                        debug.Line(ToKor(at + kphysics::Rotate(q, mesh->Vertices()[t.v[e]])),
                                                   ToKor(at + kphysics::Rotate(q, mesh->Vertices()[t.v[(e + 1) % 3]])), style);
                                        ++lines;
                                    }
                        }
                    } else if (state.shape[b] == kphysics::Capsule) {
                        const kphysics::Vec3 axis = kphysics::Rotate(sim.bodies.orientation[b], { 0.f, size.y, 0.f });
                        debug.Capsule(ToKor(at - axis), ToKor(at + axis), size.x, style);
                    } else {
                        // The unit cube (−0.5 to 0.5) turned, stretched to the half-extents twice over, and moved to the body.
                        const kphysics::Mat3 r = kphysics::FromQuat(sim.bodies.orientation[b]);
                        kecs::Mat4 m;
                        for (int column = 0; column < 3; ++column)
                            for (int row = 0; row < 3; ++row) m.At(column, row) = r.column[column][row] * 2.f * size[column];
                        m.At(3, 0) = at.x;
                        m.At(3, 1) = at.y;
                        m.At(3, 2) = at.z;
                        debug.Box(kgraph::ToKor(m), style);
                    }
                }
                if (settings.drawVelocities && state.kind[b] != Static) {
                    const kphysics::Vec3 v = sim.bodies.velocity[b];
                    if (kphysics::LengthSquared(v) > 1e-6f) debug.Arrow(ToKor(at), ToKor(at + v * 0.25f), Coloured(Yellow));
                }
            }
            if (settings.drawContacts) {
                const kor::DebugStyle point = Coloured(Red, true), arrow = Coloured(Red);
                for (const kphysics::Manifold& m : sim.contacts.manifolds)
                    for (int p = 0; p < m.count; ++p) {
                        debug.Point(ToKor(m.points[p].at), 0.06f, point);
                        debug.Arrow(ToKor(m.points[p].at), ToKor(m.points[p].at + m.normal * (0.2f + m.points[p].depth)), arrow);
                    }
            }
            if (settings.drawJoints) {
                const kor::DebugStyle style = Coloured(White, true);
                for (int j = 0; j < sim.joints.Count(); ++j) {
                    kphysics::Vec3 onA, onB;
                    sim.joints.Anchors(sim, j, onA, onB);
                    debug.Point(ToKor(onA), 0.08f, style);
                    const kphysics::Joint& joint = sim.joints.joints[j];
                    if (joint.kind == kphysics::JointKind::Distance) debug.Line(ToKor(onA), ToKor(onB), style);
                    if (joint.kind == kphysics::JointKind::Hinge) {
                        // The axis in B's frame (B is always a body here), turned into the world.
                        const kphysics::Vec3 axis = kphysics::Rotate(sim.bodies.orientation[joint.b], joint.axisB) * 0.5f;
                        debug.Line(ToKor(onA - axis), ToKor(onA + axis), style);
                    }
                }
            }
        }

        void Update(void* user, const kecs::Entity entity, void*, float)
        {
            Host& host = *static_cast<Host*>(user);
            const auto found = host.states.find(Key(entity));
            if (found == host.states.end() || !found->second.sim) return;
            State& state = found->second;
            kecs::World& world = host.document->World();
            kecs::QueryDescription query;
            query.all = { host.rigidBody, host.collider };
            // Bodies added or removed: built again at the next step.
            if (world.Count(query) != state.found) { state.sim.reset(); return; }
            if (state.bodies.empty()) return;
            const kphysics::Simulation& sim = *state.sim;
            for (int b = 0; b < static_cast<int>(state.bodies.size()); ++b) {
                if (state.kind[b] != Dynamic) continue;
                if (auto* body = static_cast<RigidBody*>(world.Mutable(state.bodies[b], host.rigidBody))) {
                    body->velocity = ToKecs(sim.bodies.velocity[b]);
                    body->angularVelocity = ToKecs(sim.bodies.angularVelocity[b]);
                }
            }
            const PhysicsSimulation settings = *static_cast<const PhysicsSimulation*>(world.Get(entity, host.world));
            if (auto* shown = static_cast<PhysicsSimulation*>(world.Mutable(entity, host.world))) {
                shown->contactCount = sim.contacts.Count();
                shown->sleepingCount = sim.SleepingCount();
            }
            if (!settings.drawColliders && !settings.drawVelocities && !settings.drawContacts && !settings.drawJoints) return;
            if (PlayScene* scene = PlayScene::Of(*host.document)) Draw(scene->SceneDebug(), state, settings);
        }

        void Destroyed(void* user, const kecs::Entity entity, void*)
        {
            static_cast<Host*>(user)->states.erase(Key(entity));
        }

        // ---- the GPU's physics, in the graph ------------------------------------------------------------------------

        constexpr const char* GpuPhysics = "gpu physics";

        /**
         * The graph's way to run the bodies, on three pages of its own: setup reads every RigidBody with a Collider
         * (XpbdRigidBodies); the fixed step moves them (XpbdStep, as the PhysicsSimulation's gravity says); every frame
         * writes them back (a WriteBack). All of it idle while the PhysicsSimulation's mode is the CPU; bodies sleep as its
         * allowSleep says. Added the first step it is wanted, when the graph has no `gpu physics` already: what is added
         * is the graph's like anything else — seen, changed, saved with it. True when it was added now.
         */
        bool GiveTheGraphTheGpusPhysics(Document& document)
        {
            using kgraph::Value;
            const auto sleeps = [](kgraph::Graph& graph, const std::string& page) {
                kgraph::Node& node = graph.Add("gpu physics sleeps", "Select");
                node.pass = page;
                node.y = 300;
                node.Wire("condition", "gpu physics settings", "field.allowSleep").Set("a", 0.5).Set("b", 0);
                if (kgraph::Node* physics = graph.Find(GpuPhysics)) physics->Wire("sleepAfter", "gpu physics sleeps");
            };
            // Each entity where its body puts it — a cone's body is at its centre of mass, below its middle — as XpbdStep leaves
            // it in each body's last 16 bytes.
            // A kinematic body follows its entity (its id in w of where it is): not written back, nor taken as moved. The rest
            // sent on by their velocities and spins over this frame's steps, which what is read back has not got yet.
            const auto placed = [](kgraph::Node& back) {
                back.Wire("buffer", GpuPhysics, "bodies").Set("stride", 112).Set("position", 96).Set("rotation", 48).Set("follows", 108)
                    .Set("velocity", 16).Set("spin", 32)
                    .Set("fields", Value::Members { { "RigidBody.velocity", 16 }, { "RigidBody.angularVelocity", 32 } });
            };
            // The kinematic bodies' entities, where they are as each step begins — a script turns one, the gizmo moves it —
            // for XpbdStep to take each there, as kphysics' bridge does.
            const auto driven = [](kgraph::Graph& graph, const std::string& page) {
                const auto add = [&](const std::string& id, const char* type, const double y) -> kgraph::Node& {
                    kgraph::Node& node = graph.Add(id, type);
                    node.pass = page;
                    node.x = -300;
                    node.y = y;
                    return node;
                };
                add("gpu physics kinematic", "ForEach", 0).Set("component", "RigidBody").Set("with", Value::List { Value("Collider") })
                    .Set("where", Value::Members { { "field.kind", 2 } });
                add("gpu physics kinematic at", "Split", 120).Wire("vector", "gpu physics kinematic", "position");
                add("gpu physics kinematic where", "Vec4", 240).Wire("x", "gpu physics kinematic at", "x").Wire("y", "gpu physics kinematic at", "y")
                    .Wire("z", "gpu physics kinematic at", "z").Wire("w", "gpu physics kinematic", "entityId");
                add("gpu physics kinematic bodies", "BufferBuilder", 360)
                    .Set("layout", Value::List { Value::Members { { "name", "at" }, { "type", "eVec4" } }, Value::Members { { "name", "turn" }, { "type", "eVec4" } } })
                    .Wire("field.at", "gpu physics kinematic where").Wire("field.turn", "gpu physics kinematic", "orientation");
                if (kgraph::Node* physics = graph.Find(GpuPhysics))
                    physics->Wire("driven", "gpu physics kinematic bodies").Wire("drivenCount", "gpu physics kinematic bodies", "count")
                        .Wire("steps", "gpu physics step", "steps");
            };
            if (const kgraph::Node* physics = document.Graph().Find(GpuPhysics)) {
                // Given before bodies slept on the GPU, were 112 bytes, followed their entities when kinematic, or were sent on
                // over the frame's steps: as they are now.
                const kgraph::Node* back = document.Graph().Find("gpu physics written back");
                const auto stride = back && back->inputs.contains("stride") ? back->inputs.at("stride").value.Number() : nullptr;
                const bool written = back && (!stride || *stride != 112.0 || !back->inputs.contains("follows") || !back->inputs.contains("velocity"));
                const bool asleep = !document.Graph().Find("gpu physics sleeps") && document.Graph().Find("gpu physics settings");
                const bool kinematic = !document.Graph().Find("gpu physics kinematic") && document.Graph().Find("gpu physics step");
                if (written || asleep || kinematic) {
                    kgraph::Graph graph = document.Graph();
                    if (asleep) sleeps(graph, physics->pass);
                    if (written) placed(*graph.Find("gpu physics written back"));
                    if (kinematic) driven(graph, physics->pass);
                    document.SetGraph(std::move(graph));
                }
                return false;
            }
            kgraph::Graph graph = document.Graph();
            const std::string bodies = "physics: bodies", step = "physics: step", written = "physics: written";
            for (const auto& page : { bodies, step, written })
                if (std::ranges::find(graph.passes, page) == graph.passes.end()) graph.passes.push_back(page);
            graph.SetRate(bodies, kgraph::PassRate::eSetup);
            graph.SetRate(step, kgraph::PassRate::eFixed);
            const auto add = [&](const std::string& id, const char* type, const std::string& page, const double y) -> kgraph::Node& {
                kgraph::Node& node = graph.Add(id, type);
                node.pass = page;
                node.y = y;
                return node;
            };
            add("gpu physics bodies", "Function", bodies, 0).Set("function", "XpbdRigidBodies");
            add("gpu physics settings", "Entity", step, 0).Set("component", "PhysicsSimulation");
            add("gpu physics on the cpu", "Subtract", step, 120).Set("a", 1).Wire("b", "gpu physics settings", "field.mode");
            add("gpu physics step", "FixedStep", step, 240);
            add(GpuPhysics, "Function", step, 360).Set("function", "XpbdStep").Wire("bodies", "gpu physics bodies", "bodies")
                .Wire("materials", "gpu physics bodies", "materials").Wire("count", "gpu physics bodies", "count").Wire("shared", "gpu physics bodies", "shared")
                .Wire("dt", "gpu physics step", "dt").Wire("gravity", "gpu physics settings", "field.gravity").Wire("idle", "gpu physics on the cpu");
            add("gpu physics written back", "WriteBack", written, 0).Set("forEach", "gpu physics bodies/bodies of").Wire("idle", "gpu physics on the cpu");
            placed(*graph.Find("gpu physics written back"));
            sleeps(graph, step);
            driven(graph, step);
            document.SetGraph(std::move(graph));
            kor::log::Info("PhysicsSimulation: the GPU runs the bodies — the graph was given the pages that do it (physics: bodies, step, written)");
            return true;
        }

        // ---- the types ------------------------------------------------------------------------------------------------

        template <typename T>
        kscript::ScriptType TypeOf(const char* name, std::vector<kecs::FieldDescription> fields)
        {
            kscript::ScriptType type;
            type.name = name;
            type.size = sizeof(T);
            type.alignment = alignof(T);
            type.fields = std::move(fields);
            const T initial {};
            type.initial.resize(sizeof(T));
            std::memcpy(type.initial.data(), &initial, sizeof(T));
            return type;
        }

        kecs::FieldDescription Field(const char* name, const char* type, const std::size_t offset, const std::size_t size,
                                     const float min = -std::numeric_limits<float>::infinity(),
                                     const float max = std::numeric_limits<float>::infinity(), const bool logarithmic = false)
        {
            kecs::FieldDescription field { name, type, offset, size, nullptr };
            field.min = min;
            field.max = max;
            field.logarithmic = logarithmic;
            return field;
        }

#define KEDITOR_FIELD(T, name, type, ...) Field(#name, type, offsetof(T, name), sizeof(T::name) __VA_OPT__(,) __VA_ARGS__)

        /** A whole number that is one of so many things: by these names, 0 the first, as an editor shows it and is given it. */
        kecs::FieldDescription Named(kecs::FieldDescription field, std::vector<std::string> names)
        {
            field.min = 0.f;
            field.max = static_cast<float>(names.size() - 1);
            field.choices = std::move(names);
            return field;
        }
    }

    void RegisterPhysics(PlayScene& scene)
    {
        Document& document = scene.Doc();
        auto& host = Hosts()[&document];
        host = std::make_unique<Host>();
        host->document = &document;
        kscript::Scripts& scripts = document.Scripts();

        host->rigidBody = scripts.Register(TypeOf<RigidBody>("RigidBody", {
            Named(KEDITOR_FIELD(RigidBody, kind, "i32"), { "Static", "Dynamic", "Kinematic" }),
            KEDITOR_FIELD(RigidBody, density, "f32", 0.f, std::numeric_limits<float>::infinity(), true),
            KEDITOR_FIELD(RigidBody, mass, "f32", 0.f),
            KEDITOR_FIELD(RigidBody, friction, "f32", 0.f),
            KEDITOR_FIELD(RigidBody, restitution, "f32", 0.f, 1.f),
            KEDITOR_FIELD(RigidBody, linearDamping, "f32", 0.f),
            KEDITOR_FIELD(RigidBody, angularDamping, "f32", 0.f),
            KEDITOR_FIELD(RigidBody, gravityScale, "f32"),
            KEDITOR_FIELD(RigidBody, velocity, "vec3"),
            KEDITOR_FIELD(RigidBody, angularVelocity, "vec3"),
        }));
        host->collider = scripts.Register(TypeOf<Collider>("Collider", {
            Named(KEDITOR_FIELD(Collider, shape, "i32"), { "Sphere", "Box", "Capsule", "Plane", "Cone", "Mesh" }),
            KEDITOR_FIELD(Collider, radius, "f32", 0.f),
            KEDITOR_FIELD(Collider, halfExtents, "vec3", 0.f),
            KEDITOR_FIELD(Collider, height, "f32", 0.f),
            KEDITOR_FIELD(Collider, thickness, "f32", 0.f),
        }));
        host->joint = scripts.Register(TypeOf<Joint>("Joint", {
            Named(KEDITOR_FIELD(Joint, kind, "i32"), { "Ball", "Hinge", "Distance", "Rope" }),
            KEDITOR_FIELD(Joint, other, "entity"),
            KEDITOR_FIELD(Joint, anchor, "vec3"),
            KEDITOR_FIELD(Joint, otherAnchor, "vec3"),
            KEDITOR_FIELD(Joint, axis, "vec3"),
            KEDITOR_FIELD(Joint, limited, "bool"),
            KEDITOR_FIELD(Joint, lowerDegrees, "f32", -180.f, 180.f),
            KEDITOR_FIELD(Joint, upperDegrees, "f32", -180.f, 180.f),
            KEDITOR_FIELD(Joint, motorSpeed, "f32"),
            KEDITOR_FIELD(Joint, maxMotorTorque, "f32", 0.f),
            KEDITOR_FIELD(Joint, collideConnected, "bool"),
        }));
        kscript::ScriptType world = TypeOf<PhysicsSimulation>("PhysicsSimulation", {
            Named(KEDITOR_FIELD(PhysicsSimulation, mode, "i32"), { "CPU", "GPU" }),
            KEDITOR_FIELD(PhysicsSimulation, gravity, "vec3"),
            KEDITOR_FIELD(PhysicsSimulation, substeps, "i32", 1.f, 8.f),
            KEDITOR_FIELD(PhysicsSimulation, iterations, "i32", 1.f, 50.f),
            KEDITOR_FIELD(PhysicsSimulation, allowSleep, "bool"),
            Named(KEDITOR_FIELD(PhysicsSimulation, broadphase, "i32"), { "Every pair", "Sweep and prune", "Grid", "Dynamic AABB tree" }),
            Named(KEDITOR_FIELD(PhysicsSimulation, gyroscopic, "i32"), { "Off", "Explicit", "Implicit" }),
            KEDITOR_FIELD(PhysicsSimulation, threads, "i32", 0.f, 64.f),
            KEDITOR_FIELD(PhysicsSimulation, drawColliders, "bool"),
            KEDITOR_FIELD(PhysicsSimulation, drawVelocities, "bool"),
            KEDITOR_FIELD(PhysicsSimulation, drawContacts, "bool"),
            KEDITOR_FIELD(PhysicsSimulation, drawJoints, "bool"),
            KEDITOR_FIELD(PhysicsSimulation, bodyCount, "i32"),
            KEDITOR_FIELD(PhysicsSimulation, contactCount, "i32"),
            KEDITOR_FIELD(PhysicsSimulation, sleepingCount, "i32"),
            KEDITOR_FIELD(PhysicsSimulation, stepMilliseconds, "f32"),
            KEDITOR_FIELD(PhysicsSimulation, broadphaseMilliseconds, "f32"),
            KEDITOR_FIELD(PhysicsSimulation, narrowphaseMilliseconds, "f32"),
            KEDITOR_FIELD(PhysicsSimulation, solverMilliseconds, "f32"),
        });
        world.callbacks.user = host.get();
        world.callbacks.fixedUpdate = FixedUpdate;
        world.callbacks.update = Update;
        world.callbacks.destroyed = Destroyed;
        host->world = scripts.Register(std::move(world));
    }
}

namespace keditor
{
    namespace
    {
        Host* HostOf(const Document& document)
        {
            const auto it = Hosts().find(&document);
            return it == Hosts().end() || !it->second || it->second->collider == kecs::NoComponent ? nullptr : it->second.get();
        }

        /** A collider as it will be in the world: its kind, where it is, how it is turned, and how big it is each way, whole. */
        struct Placed {
            int shape = kphysics::Box;
            kor::Vec3 at { 0.f };
            kor::Quat turn {};
            kor::Vec3 extent { 1.f };   // across, whole: a box's sides, a sphere's or a capsule's width and height
            float radius = 0.f;
        };

        std::optional<Placed> PlacedOf(const Host& host, const kecs::World& world, const kecs::Entity entity)
        {
            if (!world.Alive(entity) || !world.Has(entity, host.collider)) return std::nullopt;
            const auto& collider = *static_cast<const Collider*>(world.Get(entity, host.collider));
            Placed placed;
            placed.shape = std::clamp(collider.shape, 0, 5);
            // As the physics takes them: the entity's position, rotation and scale (it moves only those at the top).
            const kecs::Vec3 p = world.PositionOf(entity), s = world.ScaleOf(entity);
            const kecs::Quat q = world.OrientationOf(entity);
            placed.at = kor::Vec3(p.x, p.y, p.z);
            placed.turn = kor::Quat(q.x, q.y, q.z, q.w);
            const float sx = std::abs(s.x), sy = std::abs(s.y), sz = std::abs(s.z);
            switch (placed.shape) {
            case kphysics::Sphere:
                placed.radius = collider.radius * std::max({ sx, sy, sz });
                placed.extent = kor::Vec3(2.f * placed.radius);
                break;
            case kphysics::Box:
                placed.extent = kor::Vec3(2.f * collider.halfExtents.x * sx, 2.f * collider.halfExtents.y * sy, 2.f * collider.halfExtents.z * sz);
                break;
            case kphysics::Capsule:
                placed.radius = collider.radius * std::max(sx, sz);
                placed.extent = kor::Vec3(2.f * placed.radius, std::max(collider.height * sy, 2.f * placed.radius), 2.f * placed.radius);
                break;
            case kphysics::Cone:
                placed.radius = collider.radius * std::max(sx, sz);
                placed.extent = kor::Vec3(2.f * placed.radius, collider.height * sy, 2.f * placed.radius);
                break;
            default:
                placed.extent = kor::Vec3(sx, 0.f, sz);
                break;
            }
            return placed;
        }
    }

    bool HasCollider(const Document& document, const kecs::Entity entity)
    {
        const Host* host = HostOf(document);
        return host && document.World().Alive(entity) && document.World().Has(entity, host->collider);
    }

    bool ColliderResizable(const Document& document, const kecs::Entity entity)
    {
        const Host* host = HostOf(document);
        const auto placed = host ? PlacedOf(*host, document.World(), entity) : std::nullopt;
        return placed && placed->shape != kphysics::Plane && placed->shape != kphysics::Mesh;
    }

    void DrawCollider(kor::DebugDraw& debug, Document& document, const kecs::Entity entity, const bool editing)
    {
        Host* host = HostOf(document);
        if (!host) return;
        kecs::World& world = document.World();
        const auto placed = PlacedOf(*host, world, entity);
        if (!placed) return;
        int kind = Dynamic;
        if (world.Has(entity, host->rigidBody)) kind = std::clamp(static_cast<const RigidBody*>(world.Get(entity, host->rigidBody))->kind, 0, 2);
        const kor::DebugStyle style = Coloured(editing ? Yellow : kind == Static ? Grey : kind == Kinematic ? Blue : Green, true);
        const kor::Vec3 at = placed->at, up = placed->turn * kor::Vec3(0.f, 1.f, 0.f);
        switch (placed->shape) {
        case kphysics::Sphere: debug.Sphere(at, placed->radius, style); break;
        case kphysics::Box: debug.Box(kor::Compose(at, placed->turn, placed->extent), style); break;
        case kphysics::Capsule: {
            const kor::Vec3 axis = up * (placed->extent.y / 2.f - placed->radius);
            debug.Capsule(at - axis, at + axis, placed->radius, style);
            break;
        }
        case kphysics::Cone: debug.Cone(at - up * (placed->extent.y / 2.f), at + up * (placed->extent.y / 2.f), placed->radius, style); break;
        case kphysics::Plane: {
            // A plane goes on for ever: its middle, a unit by its scale, and which way it faces.
            const kor::Vec3 x = placed->turn * kor::Vec3(placed->extent.x / 2.f, 0.f, 0.f), z = placed->turn * kor::Vec3(0.f, 0.f, placed->extent.z / 2.f);
            debug.Quad(at - x - z, at + x - z, at + x + z, at - x + z, style);
            debug.Arrow(at, at + up * 0.5f, style);
            break;
        }
        default: {
            // A mesh: its sharp edges — the folds and its rim — as the physics pushes along them.
            const auto triangles = MeshOf(*host, world, entity, world.ScaleOf(entity), host->shown);
            if (!triangles) break;
            int lines = 0;
            for (const auto& t : triangles->Triangles())
                for (int e = 0; e < 3 && lines < 8192; ++e)
                    if (t.sharp & (1u << e)) {
                        const kphysics::Vec3 a = triangles->Vertices()[t.v[e]], b = triangles->Vertices()[t.v[(e + 1) % 3]];
                        debug.Line(at + placed->turn * ToKor(a), at + placed->turn * ToKor(b), style);
                        ++lines;
                    }
            break;
        }
        }
    }

    bool EditCollider(kor::DebugDraw& debug, Document& document, const kecs::Entity entity, const kor::Mat4& viewProjection, const kor::GizmoPointer& pointer)
    {
        Host* host = HostOf(document);
        if (!host) return false;
        kecs::World& world = document.World();
        const auto placed = PlacedOf(*host, world, entity);
        if (!placed || placed->shape == kphysics::Plane || placed->shape == kphysics::Mesh) return false;
        const kor::Vec3 was = kor::Vec3(std::max(placed->extent.x, 1e-3f), std::max(placed->extent.y, 1e-3f), std::max(placed->extent.z, 1e-3f));
        kor::Mat4 frame = kor::Compose(placed->at, placed->turn, was);
        kor::GizmoOptions options;
        options.space = kor::GizmoSpace::eLocal;
        if (!debug.Gizmo(kor::GizmoMode::eScale, frame, viewProjection, pointer, options, 0xC0111DE5ull)) return false;
        kor::Vec3 at, now;
        kor::Quat turn;
        if (!kor::Decompose(frame, at, turn, now)) return true;
        const kor::Vec3 by(now.x / was.x, now.y / was.y, now.z / was.z);
        auto& collider = *static_cast<Collider*>(const_cast<void*>(std::as_const(world).Get(entity, host->collider)));
        const auto least = [](const float v) { return std::max(v, 1e-3f); };
        // The way it was dragged most: what a size that is the same every way, or across, is changed by.
        const auto most = [](std::initializer_list<float> ratios) {
            float chosen = 1.f;
            for (const float r : ratios) if (std::abs(r - 1.f) > std::abs(chosen - 1.f)) chosen = r;
            return chosen;
        };
        switch (placed->shape) {
        case kphysics::Sphere:
            collider.radius = least(collider.radius * most({ by.x, by.y, by.z }));
            document.Moved(entity, "Collider.radius");
            break;
        case kphysics::Box:
            collider.halfExtents = { least(collider.halfExtents.x * by.x), least(collider.halfExtents.y * by.y), least(collider.halfExtents.z * by.z) };
            document.Moved(entity, "Collider.halfExtents");
            break;
        default:        // a capsule, a cone: across, and along
            collider.radius = least(collider.radius * most({ by.x, by.z }));
            collider.height = least(collider.height * by.y);
            document.Moved(entity, "Collider.radius");
            document.Moved(entity, "Collider.height");
            break;
        }
        // Playing, its bodies are built again with it: what was built had the shape it had.
        for (auto& state : host->states | std::views::values) state.sim.reset();
        return true;
    }
}
