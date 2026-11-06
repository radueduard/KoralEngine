# Physics

The engine's rigid-body physics, written in Kotlin: a module, which a project imports to have its components. Open this
folder in `koral-editor` to work on it, and edit `scripts/` while it runs — a script saved is compiled and loaded again.
Its scene, `test`, is only a test bed: a ground plane and one body of each shape above it, and the `Physics` entity that
steps them. A project that imports the module never sees it.

| File | What is in it |
|---|---|
| `RigidBody.kt` | The body: static, dynamic or kinematic; mass, damping, velocity and angular velocity. Data. |
| `Colliders.kt` | `Collider`, a kind (friction, restitution), and the shapes: `PlaneCollider`, `CubeCollider`, `SphereCollider`, `CylinderCollider`, `CapsuleCollider`, `MeshCollider`. Data. |
| `Bodies.kt` | Every body, a `FloatArray` a property: vectors 3 floats a body, quaternions 4. |
| `Shapes.kt` | A table a shape, of its sizes. Bodies are kept in the order of their shapes, so a shape's bodies are one run of the body buffers. |
| `Contacts.kt` | Where bodies touch, a `FloatArray` a property, made once at a capacity. |
| `Stages.kt` | What a step does, a function a stage: forces, contacts found, contacts solved, integration. Empty, to be written. |
| `Physics.kt` | The script on the `Physics` entity: gathers the bodies once, then on each fixed step reads their motion from their entities, runs the stages, and writes them back. |

Nothing is made while it steps: every buffer is made when the bodies are gathered, and the engine's calls that read
many entities at once (`world.positionsOf`, `world.floatsOf`) fill them in place.

What is left to write is marked `TODO(you)`: the stages, mesh triangles, and a collider's size under its entity's scale.
