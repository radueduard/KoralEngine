// Where bodies touch, found again each step: one row a contact point, a float buffer a property. Made once at
// [capacity] rows; a step fills rows from 0 and says how many with [count], and nothing is made while it steps.
//
// Which two bodies a contact is between is a whole number kept as a float, as everything else is: exact up to
// 16,777,216 bodies, and read the same way by a GPU version of this later.

class Contacts(val capacity: Int) {
    var count = 0

    val bodyA = FloatArray(capacity)        // 1 a contact: the body the normal points away from.
    val bodyB = FloatArray(capacity)        // 1 a contact: the body the normal points towards.
    val point = FloatArray(capacity * 3)    // 3 a contact: where they touch, in the world.
    val normal = FloatArray(capacity * 3)   // 3 a contact: from A towards B, length 1.
    val depth = FloatArray(capacity)        // 1 a contact: how far they are into each other.

    /** Forgets every contact: what each step starts from. */
    fun clear() {
        count = 0
    }

    /**
     * Adds a contact between bodies [a] and [b] at ([px], [py], [pz]), its normal ([nx], [ny], [nz]) from A towards B.
     * False, adding nothing, when there is no room left.
     */
    fun add(a: Int, b: Int, px: Float, py: Float, pz: Float, nx: Float, ny: Float, nz: Float, depth: Float): Boolean {
        if (count == capacity) return false
        val c = count++
        bodyA[c] = a.toFloat()
        bodyB[c] = b.toFloat()
        point[3 * c] = px; point[3 * c + 1] = py; point[3 * c + 2] = pz
        normal[3 * c] = nx; normal[3 * c + 1] = ny; normal[3 * c + 2] = nz
        this.depth[c] = depth
        return true
    }
}
