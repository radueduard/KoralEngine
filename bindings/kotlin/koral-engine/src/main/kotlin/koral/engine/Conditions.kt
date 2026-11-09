// Conditions: what a ForEach or a Filter of the graph keeps an entity by, written in a script.

package koral.engine

import koral.Vec2
import koral.Vec3
import koral.Vec4

/**
 * A class of conditions: tests a ForEach or a Filter keeps an entity by, each named in its `conditions` as
 * `<Class>.<function>`. A condition is a function marked [Condition] — in the class's `companion object`, `@JvmStatic`,
 * or in an `object` — that returns a Boolean: its first N arguments are the entity's components ([EntityMesh] for its
 * Mesh, a [Data] class or a script for one of a script's) and the rest are its inputs, each an input on the node,
 * `<Class>.<function>.<argument>`: a number, a Boolean, a String, a vector, a [MeshLayout], a list of strings, or a
 * component to compare with. It is to read nothing but its arguments: what it said of an entity is asked again only when
 * one of those components changes, or an input does.
 * ```
 * class MeshTests : Conditions() {
 *     companion object {
 *         @Condition(1) @JvmStatic
 *         fun hasNormals(mesh: EntityMesh): Boolean = mesh.layout.has("NORMAL")
 *     }
 * }
 * ```
 */
abstract class Conditions

/** Marks a [Conditions] class's function a condition, whose first [components] arguments are the entity's components, and the rest its inputs. */
@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class Condition(val components: Int)

/** One of a mesh's vertex attributes: what it is (`POSITION`), the type of its numbers (`eFloat`), how many. */
data class MeshLayoutAttribute(val semantic: String, val type: String = "eFloat", val count: Int = 0)

/** What a mesh's vertices are made of: its attributes, by semantic. A condition's input of a vertex layout is one too. */
class MeshLayout(val attributes: List<MeshLayoutAttribute>) {
    /** Whether it has an attribute of [semantic]. */
    fun has(semantic: String): Boolean = attributes.any { it.semantic == semantic }
    /** Whether every attribute of it, by semantic, [other] has too. */
    fun isSubsetOf(other: MeshLayout): Boolean = attributes.all { other.has(it.semantic) }
    override fun toString(): String = attributes.joinToString(", ") { it.semantic }

    internal companion object {
        /** One as the graph gives it: a vertex layout (`{"attributes": [...]}`), a list of attributes, or a list of semantics. */
        fun from(json: Any?): MeshLayout {
            if (json is Map<*, *>) for (name in listOf("attributes", "layout")) if (json.containsKey(name)) return from(json[name])
            val attributes = (json as? List<*>).orEmpty().mapNotNull { item ->
                when (item) {
                    is String -> MeshLayoutAttribute(item)
                    is Map<*, *> -> (item["semantic"] as? String)?.let { MeshLayoutAttribute(it, item["type"] as? String ?: "eFloat", (item["count"] as? Double)?.toInt() ?: 0) }
                    else -> null
                }
            }
            return MeshLayout(attributes)
        }
    }
}

/** An entity's Mesh, as a condition is given it: what its mesh lays its vertices out as (nothing, while it is read). */
class EntityMesh(val layout: MeshLayout)

/** A condition's arguments from what the engine gives: its components', and its inputs', values as JSON. */
internal object ConditionValues {
    fun isComponent(type: Class<*>): Boolean = type == EntityMesh::class.java
        || ((Data::class.java.isAssignableFrom(type) || Setup::class.java.isAssignableFrom(type)) && !java.lang.reflect.Modifier.isAbstract(type.modifiers)
            && !ResourceScript::class.java.isAssignableFrom(type))

    /** What a component argument of [type] is called in the world. */
    fun componentName(type: Class<*>): String = if (type == EntityMesh::class.java) "Mesh" else type.simpleName

    /** What an input of [type] is to the graph, as a port's type; null for one it cannot be. */
    fun portType(type: Class<*>): String? = when (type) {
        java.lang.Float.TYPE, java.lang.Double.TYPE, java.lang.Integer.TYPE, java.lang.Float::class.java, java.lang.Double::class.java, java.lang.Integer::class.java -> "number"
        java.lang.Boolean.TYPE, java.lang.Boolean::class.java -> "bool"
        String::class.java -> "string"
        Vec2::class.java, Vec3::class.java, Vec4::class.java -> "vector"
        MeshLayout::class.java -> "struct KoralVertexLayout"
        List::class.java -> "list"
        else -> if (isComponent(type) && type != EntityMesh::class.java) "bundle" else null
    }

    fun from(json: Any?, type: Class<*>): Any? {
        fun number(value: Any?): Float = when (value) { is Double -> value.toFloat(); true -> 1f; else -> 0f }
        fun numbers(value: Any?, count: Int): FloatArray {
            val out = FloatArray(count)
            if (value is List<*>) value.take(count).forEachIndexed { i, v -> out[i] = number(v) } else out[0] = number(value)
            return out
        }
        return when (type) {
            java.lang.Float.TYPE, java.lang.Float::class.java -> number(json)
            java.lang.Double.TYPE, java.lang.Double::class.java -> number(json).toDouble()
            java.lang.Integer.TYPE, java.lang.Integer::class.java -> Math.round(number(json))
            java.lang.Boolean.TYPE, java.lang.Boolean::class.java -> json == true || (json is Double && json != 0.0)
            String::class.java -> json as? String ?: json?.toString()
            Vec2::class.java -> numbers(json, 2).let { Vec2(it[0], it[1]) }
            Vec3::class.java -> numbers(json, 3).let { Vec3(it[0], it[1], it[2]) }
            Vec4::class.java -> numbers(json, 4).let { Vec4(it[0], it[1], it[2], it[3]) }
            MeshLayout::class.java -> MeshLayout.from(json)
            List::class.java -> (json as? List<*>).orEmpty().map { it as? String ?: it.toString() }
            EntityMesh::class.java -> {
                // The component's mesh field, as its layout.
                val mesh = (json as? Map<*, *>)?.values?.firstOrNull { it is Map<*, *> && it.containsKey("layout") } as? Map<*, *>
                EntityMesh(MeshLayout.from(mesh?.get("layout")))
            }
            else -> if (isComponent(type)) {
                // One of a script's: a new one, each property the JSON has set from it.
                val made = type.getDeclaredConstructor().newInstance()
                val fields = json as? Map<*, *> ?: emptyMap<Any, Any>()
                var at: Class<*>? = type
                while (at != null && at != Any::class.java) {
                    for (field in at.declaredFields) {
                        if (java.lang.reflect.Modifier.isStatic(field.modifiers) || !fields.containsKey(field.name)) continue
                        val value = fields[field.name] ?: continue
                        if (portType(field.type) == null) continue
                        field.isAccessible = true
                        field.set(made, from(value, field.type))
                    }
                    at = at.superclass
                }
                made
            } else null
        }
    }

    /** JSON as maps, lists, doubles, Booleans, strings and nulls: what the engine gives a condition. */
    fun parse(text: String): Any? {
        var i = 0
        fun space() { while (i < text.length && text[i].isWhitespace()) ++i }
        fun value(): Any? {
            space()
            if (i >= text.length) return null
            return when (val c = text[i]) {
                '{' -> {
                    ++i
                    val map = LinkedHashMap<String, Any?>()
                    space()
                    if (text[i] == '}') { ++i; return map }
                    while (true) {
                        space()
                        val key = value() as String
                        space(); ++i     // ':'
                        map[key] = value()
                        space()
                        if (text[i++] == '}') break
                    }
                    map
                }
                '[' -> {
                    ++i
                    val list = ArrayList<Any?>()
                    space()
                    if (text[i] == ']') { ++i; return list }
                    while (true) {
                        list += value()
                        space()
                        if (text[i++] == ']') break
                    }
                    list
                }
                '"' -> {
                    ++i
                    val out = StringBuilder()
                    while (text[i] != '"') {
                        if (text[i] == '\\') {
                            ++i
                            when (val e = text[i]) {
                                'n' -> out.append('\n'); 't' -> out.append('\t'); 'r' -> out.append('\r'); 'b' -> out.append('\b'); 'f' -> out.append('\u000c')
                                'u' -> { out.append(text.substring(i + 1, i + 5).toInt(16).toChar()); i += 4 }
                                else -> out.append(e)
                            }
                        } else out.append(text[i])
                        ++i
                    }
                    ++i
                    out.toString()
                }
                't' -> { i += 4; true }
                'f' -> { i += 5; false }
                'n' -> { i += 4; null }
                else -> {
                    val start = i
                    while (i < text.length && (text[i].isDigit() || text[i] in "+-.eE")) ++i
                    if (start == i) throw IllegalArgumentException("not JSON at $i: '$c'")
                    text.substring(start, i).toDouble()
                }
            }
        }
        return value()
    }
}
