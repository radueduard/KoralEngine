// The physics: put on one entity of a scene. While the scene plays it gathers every top-level entity with a RigidBody and
// a Collider into float buffers once, steps them on each fixed step, and writes where each went back to its entity.
// What a step does is in Stages.kt; what it steps is in Bodies.kt, Shapes.kt and Contacts.kt.

import koral.Vec3
import koral.engine.Entities
import koral.engine.FixedStep
import koral.engine.Min
import koral.engine.Setup

class Physics : Setup(), FixedStep {
    var gravity = Vec3(0f, -9.81f, 0f)                  // In metres a second, each second.
    @Min(1f) var substeps = 4                           // How many smaller steps each fixed step is taken in.
    @Min(1f) var contactCapacity = 4096                 // The most contacts a step keeps.

    private val bodies = Bodies()
    private val shapes = Shapes()
    private var contacts = Contacts(0)
    private var entities: Entities = Entities.of()      // Every body's entity, in the order of the bodies.

    override fun initialize() {
        gather()
    }

    override fun fixedUpdate(step: Float) {
        readMotion()
        val dt = step / substeps
        repeat(substeps) {
            applyForces(bodies, gravity, dt)
            findContacts(bodies, shapes, contacts)
            solveContacts(bodies, contacts, dt)
            integrate(bodies, dt)
        }
        writeBack()
    }

    /** Every body, read from its entity into the buffers: kept in the order of their shapes, a shape's bodies one run. */
    private fun gather() {
        val ordered = mutableListOf<koral.engine.Entity>()
        for (table in shapes.all) {
            val ofShape = world.with("RigidBody", table.collider).filter { world.parentOf(it).isNull }
            table.place(Entities.of(ofShape), ordered.size)
            ordered += ofShape
        }
        entities = Entities.of(ordered)
        bodies.resize(entities.size)
        contacts = Contacts(contactCapacity)

        readMotion()
        bodies.linearDamping = world.floatsOf(entities, "RigidBody", "linearDamping", bodies.linearDamping)
        bodies.angularDamping = world.floatsOf(entities, "RigidBody", "angularDamping", bodies.angularDamping)
        // Read through the Collider kind: from whichever collider each body has.
        bodies.friction = world.floatsOf(entities, "Collider", "friction", bodies.friction)
        bodies.restitution = world.floatsOf(entities, "Collider", "restitution", bodies.restitution)

        // A body's inverse mass: 0 for one nothing moves — static, kinematic, or a plane, whatever its body says.
        bodies.inverseMass = world.floatsOf(entities, "RigidBody", "mass", bodies.inverseMass)
        val motion = world.intsOf(entities, "RigidBody", "motion")      // read once, here: 0 static, 1 dynamic, 2 kinematic
        for (i in 0 until bodies.count) {
            val dynamic = motion[i] == 1 && !shapes.planes.holds(i)
            bodies.inverseMass[i] = if (dynamic) 1f / bodies.inverseMass[i] else 0f
        }

        for (table in shapes.all) table.read(world)
        computeInertia(bodies, shapes)
    }

    /**
     * Where each body is and how it moves, from its entity, straight into the buffers: they are laid out as these calls
     * fill them. Each step starts from it, so a kinematic body is where its script put it, and what the inspector changes
     * while the scene plays is what the step goes on from. A dynamic body reads back what the last step wrote.
     */
    private fun readMotion() {
        bodies.position = world.positionsOf(entities, bodies.position)
        bodies.orientation = world.orientationsOf(entities, bodies.orientation)
        bodies.velocity = world.floatsOf(entities, "RigidBody", "velocity", bodies.velocity)
        bodies.angularVelocity = world.floatsOf(entities, "RigidBody", "angularVelocity", bodies.angularVelocity)
    }

    /** Where each body went, and how fast it goes, back to its entity. */
    private fun writeBack() {
        world.setPositions(entities, bodies.position)
        world.setOrientations(entities, bodies.orientation)
        world.setFloats(entities, "RigidBody", "velocity", bodies.velocity)
        world.setFloats(entities, "RigidBody", "angularVelocity", bodies.angularVelocity)
    }
}
