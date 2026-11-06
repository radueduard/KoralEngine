// The bodies of each shape, and what sizes each. Bodies are kept in the order of their shapes — every plane, then every
// cube, then every sphere... — so a shape's bodies are one run of the body buffers: row `r` of a table is body
// `first + r`, and no table needs a list of which bodies are its.

import koral.engine.Entities
import koral.engine.World

/** One shape's bodies: which collider they have, where their run of bodies starts, and how many there are. */
abstract class ShapeTable(val collider: String) {
    var first = 0
        private set
    var count = 0
        private set
    /** Their entities, in the order of their rows. */
    var entities: Entities = Entities.of()
        private set

    /** Takes [entities] as its rows, from body [first] on. */
    fun place(entities: Entities, first: Int) {
        this.entities = entities
        this.first = first
        this.count = entities.size
    }

    /** The body row [r] is. */
    fun body(r: Int) = first + r

    /** Whether [body] is one of this shape's. */
    fun holds(body: Int) = body >= first && body < first + count

    /**
     * Reads each row's sizes from its collider: a buffer a size, filled in place. As the collider says, before its
     * entity's scale. TODO(you): the scale too, if bodies are to be scaled.
     */
    abstract fun read(world: World)
}

class PlaneTable : ShapeTable("PlaneCollider") {
    // Its place and facing are its body's position and orientation: no size.
    override fun read(world: World) {}
}

class CubeTable : ShapeTable("CubeCollider") {
    var halfExtents = FloatArray(0)         // 3 a row.
    override fun read(world: World) {
        halfExtents = world.floatsOf(entities, collider, "halfExtents", halfExtents)
    }
}

class SphereTable : ShapeTable("SphereCollider") {
    var radius = FloatArray(0)              // 1 a row.
    override fun read(world: World) {
        radius = world.floatsOf(entities, collider, "radius", radius)
    }
}

class CylinderTable : ShapeTable("CylinderCollider") {
    var radius = FloatArray(0)              // 1 a row.
    var height = FloatArray(0)              // 1 a row.
    override fun read(world: World) {
        radius = world.floatsOf(entities, collider, "radius", radius)
        height = world.floatsOf(entities, collider, "height", height)
    }
}

class CapsuleTable : ShapeTable("CapsuleCollider") {
    var radius = FloatArray(0)              // 1 a row.
    var height = FloatArray(0)              // 1 a row: of the cylinder between the two ends.
    override fun read(world: World) {
        radius = world.floatsOf(entities, collider, "radius", radius)
        height = world.floatsOf(entities, collider, "height", height)
    }
}

class MeshTable : ShapeTable("MeshCollider") {
    // Its triangles are its entity's Mesh. TODO(you): their vertices and triangles, as float buffers of their own.
    override fun read(world: World) {}
}

/** Every shape's table, in the order their bodies are kept. */
class Shapes {
    val planes = PlaneTable()
    val cubes = CubeTable()
    val spheres = SphereTable()
    val cylinders = CylinderTable()
    val capsules = CapsuleTable()
    val meshes = MeshTable()

    val all: List<ShapeTable> = listOf(planes, cubes, spheres, cylinders, capsules, meshes)
}
