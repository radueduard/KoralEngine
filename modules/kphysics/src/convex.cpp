// Cones and meshes: the narrowphase for convex polyhedra. A cone is collided as a pyramid of ConeSides sides, a box as
// its six faces, a mesh's triangle as a one-sided face. Between two of those: the separating axis test over every face
// normal and every pair of edges, then clipping, as box–box does (Gregorius, "The Separating Axis Test between Convex
// Polyhedra", GDC 2013), so a cone stands on its base and a box rests on a triangle with four points. A sphere or a
// capsule is a point or a segment with a radius: its contact is the closest points of that core and the polyhedron
// (GJK), with two points for a capsule lying along a face.
//
// A mesh's triangles are one-sided, and a body touching the flat join between two of them takes the face's normal, not
// the edge's: else a box sliding over a flat floor of triangles catches on every join (a "ghost" collision). Only a
// sharp edge, a convex fold or the mesh's own edge, pushes along its own way.

#include <kphysics/collision.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numbers>
#include <utility>

namespace kphysics
{
    namespace
    {
        constexpr float MergeDistance = 2e-3f;
        constexpr float ClipTolerance = 1e-3f;
        constexpr int MaxVertices = 32, MaxFaces = 32, MaxIndices = 128, MaxEdges = 64, MaxPolygon = 72;

        /** A convex polyhedron in the world: its corners, its faces (counter-clockwise about their outward normals) and edges. */
        struct Polyhedron {
            struct Face {
                Vec3 normal;
                float offset = 0.f;         // normal · any of its corners
                int first = 0, count = 0;   // its corners, in index[first, first + count)
            };
            struct Edge {
                int a = 0, b = 0;
            };
            Vec3 vertex[MaxVertices];
            int vertexCount = 0;
            Face face[MaxFaces];
            int faceCount = 0;
            int index[MaxIndices];
            int indexCount = 0;
            Edge edge[MaxEdges];
            int edgeCount = 0;
            Vec3 centre;

            /** A face of @p corners facing @p normal, put in order counter-clockwise about it. */
            void AddFace(const Vec3 normal, const int* corners, const int count)
            {
                Face& f = face[faceCount++];
                f.normal = normal;
                f.first = indexCount;
                f.count = count;
                Vec3 middle;
                for (int i = 0; i < count; ++i) middle += vertex[corners[i]];
                middle = middle * (1.f / static_cast<float>(count));
                const Vec3 u = Perpendicular(normal), w = Cross(normal, u);
                int sorted[MaxVertices];
                std::copy_n(corners, count, sorted);
                std::sort(sorted, sorted + count, [&](const int p, const int q) {
                    const Vec3 dp = vertex[p] - middle, dq = vertex[q] - middle;
                    return std::atan2(Dot(dp, w), Dot(dp, u)) < std::atan2(Dot(dq, w), Dot(dq, u));
                });
                for (int i = 0; i < count; ++i) index[indexCount++] = sorted[i];
                f.offset = Dot(normal, vertex[sorted[0]]);
            }
            [[nodiscard]] Vec3 Corner(const Face& f, const int i) const { return vertex[index[f.first + (i % f.count)]]; }
            /** Least and most of n · every corner. */
            void Project(const Vec3 n, float& least, float& most) const
            {
                least = FLT_MAX;
                most = -FLT_MAX;
                for (int i = 0; i < vertexCount; ++i) {
                    const float d = Dot(n, vertex[i]);
                    least = std::min(least, d);
                    most = std::max(most, d);
                }
            }
        };

        Polyhedron BoxPolyhedron(const Bodies& bodies, const Colliders& colliders, const int b)
        {
            Polyhedron p;
            for (int c = 0; c < 8; ++c) p.vertex[c] = narrowphase::BoxCorner(bodies, colliders, b, c);
            p.vertexCount = 8;
            p.centre = bodies.position[b];
            const Mat3 axes = FromQuat(bodies.orientation[b]);
            for (int axis = 0; axis < 3; ++axis)
                for (const int side : { 1, 0 }) {
                    int corners[4], n = 0;
                    for (int c = 0; c < 8; ++c)
                        if (((c >> axis) & 1) == side) corners[n++] = c;
                    p.AddFace(axes.column[axis] * (side ? 1.f : -1.f), corners, 4);
                }
            for (int c = 0; c < 8; ++c)
                for (int axis = 0; axis < 3; ++axis)
                    if (((c >> axis) & 1) == 0) p.edge[p.edgeCount++] = { c, c | (1 << axis) };
            return p;
        }

        Polyhedron ConePolyhedron(const Bodies& bodies, const Colliders& colliders, const int b)
        {
            Polyhedron p;
            narrowphase::ConeCorners(bodies, colliders, b, p.vertex);
            p.vertexCount = ConeSides + 1;
            p.centre = bodies.position[b];
            const Vec3 up = FromQuat(bodies.orientation[b]).column[1];
            for (int i = 0; i < ConeSides; ++i) {
                const int j = i + 1, k = (i + 1) % ConeSides + 1;
                Vec3 n = Cross(p.vertex[j] - p.vertex[0], p.vertex[k] - p.vertex[0]);
                Normalize(n);
                if (Dot(n, p.vertex[0] - p.centre) < 0.f) n = -n;
                const int corners[3] { 0, j, k };
                p.AddFace(n, corners, 3);
                p.edge[p.edgeCount++] = { 0, j };
                p.edge[p.edgeCount++] = { j, k };
            }
            int rim[ConeSides];
            for (int i = 0; i < ConeSides; ++i) rim[i] = i + 1;
            p.AddFace(-up, rim, ConeSides);
            return p;
        }

        Polyhedron TrianglePolyhedron(const Vec3 a, const Vec3 b, const Vec3 c, const Vec3 normal)
        {
            Polyhedron p;
            p.vertex[0] = a;
            p.vertex[1] = b;
            p.vertex[2] = c;
            p.vertexCount = 3;
            p.centre = (a + b + c) * (1.f / 3.f);
            const int corners[3] { 0, 1, 2 };
            p.AddFace(normal, corners, 3);
            p.edge[0] = { 0, 1 };
            p.edge[1] = { 1, 2 };
            p.edge[2] = { 2, 0 };
            p.edgeCount = 3;
            return p;
        }

        /** Sutherland–Hodgman against one plane, keeping n · p ≤ d; a crossing's id is 32 · clipId + its edge's start's. */
        int Clip(const Vec3* points, const int* ids, const int count, const Vec3 n, const float d, const int clipId, Vec3* out, int* outIds)
        {
            int kept = 0;
            for (int q = 0; q < count; ++q) {
                const int r = q + 1 == count ? 0 : q + 1;
                const float dp = Dot(n, points[q]) - d, dq = Dot(n, points[r]) - d;
                if (dp <= 0.f) {
                    out[kept] = points[q];
                    outIds[kept++] = ids[q];
                }
                if ((dp <= 0.f) != (dq <= 0.f) && kept < MaxPolygon) {
                    out[kept] = points[q] + (points[r] - points[q]) * (dp / (dp - dq));
                    outIds[kept++] = 32 * clipId + ids[q] % 32;
                }
            }
            return kept;
        }

        void Swap(Vec3* points, float* depths, int* ids, const int x, const int y)
        {
            std::swap(points[x], points[y]);
            std::swap(depths[x], depths[y]);
            std::swap(ids[x], ids[y]);
        }

        /** Points closer than MergeDistance are one, the deeper kept. */
        int Merge(Vec3* points, float* depths, int* ids, const int count)
        {
            int n = count;
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n;) {
                    if (LengthSquared(points[i] - points[j]) < MergeDistance * MergeDistance) {
                        if (depths[j] > depths[i]) Swap(points, depths, ids, i, j);
                        Swap(points, depths, ids, j, n - 1);
                        --n;
                    } else {
                        ++j;
                    }
                }
            return n;
        }

        /** What a contact is before it is a manifold: a normal from A to B and up to four points. */
        struct Patch {
            Vec3 normal;
            Vec3 point[4];
            float depth[4] {};
            int id[4] {};
            int count = 0;
        };

        /**
         * The face case: face @p f of @p ref (its outward normal towards @p inc) is the reference; the face of @p inc most
         * against it is clipped by the reference's sides (a millimetre wider, so corners right on its edges stay whole),
         * and its points within the margin, put back onto the face, are the contact. The normal is out of @p ref.
         */
        bool FaceContact(const Polyhedron& ref, const int f, const Polyhedron& inc, const float margin, Patch& out, const int refIsA)
        {
            const Polyhedron::Face& face = ref.face[f];
            const Vec3 n = face.normal;
            int incident = 0;
            float lowest = FLT_MAX;
            for (int g = 0; g < inc.faceCount; ++g) {
                const float along = Dot(inc.face[g].normal, n);
                if (along < lowest) { lowest = along; incident = g; }
            }
            Vec3 bufferA[MaxPolygon], bufferB[MaxPolygon];
            int idsA[MaxPolygon], idsB[MaxPolygon];
            const Polyhedron::Face& in = inc.face[incident];
            int count = in.count;
            for (int i = 0; i < count; ++i) {
                bufferA[i] = inc.Corner(in, i);
                idsA[i] = i;
            }
            Vec3* points = bufferA;
            int* ids = idsA;
            for (int side = 0; side < face.count && count > 0; ++side) {
                const Vec3 p = ref.Corner(face, side), q = ref.Corner(face, side + 1);
                Vec3 outward = Cross(q - p, n);
                if (Normalize(outward) < 1e-9f) continue;
                Vec3* to = points == bufferA ? bufferB : bufferA;
                int* toIds = ids == idsA ? idsB : idsA;
                count = Clip(points, ids, count, outward, Dot(outward, p) + ClipTolerance, side + 1, to, toIds);
                points = to;
                ids = toIds;
            }
            float depths[MaxPolygon];
            int kept = 0;
            for (int i = 0; i < count; ++i) {
                const float s = Dot(n, points[i]) - face.offset;
                if (s > margin) continue;
                Vec3 p = points[i];
                // Back within the face's sides: a corner kept by the tolerance is up to that far outside.
                for (int side = 0; side < face.count; ++side) {
                    const Vec3 a = ref.Corner(face, side), b = ref.Corner(face, side + 1);
                    Vec3 outward = Cross(b - a, n);
                    if (Normalize(outward) < 1e-9f) continue;
                    const float beyond = Dot(outward, p - a);
                    if (beyond > 0.f) p -= outward * beyond;
                }
                points[kept] = p - n * (s / 2.f);
                depths[kept] = -s;
                ids[kept] = refIsA ? Feature(f, ids[i]) : Feature(ids[i], f);
                ++kept;
            }
            if (kept == 0) return false;
            kept = Merge(points, depths, ids, kept);
            if (kept > 4) kept = narrowphase::ReduceToFour(points, depths, ids, kept, n);
            out.normal = refIsA ? n : -n;
            out.count = kept;
            for (int i = 0; i < kept; ++i) {
                out.point[i] = points[i];
                out.depth[i] = depths[i];
                out.id[i] = ids[i];
            }
            return true;
        }

        /** Separation along @p n (pointing from a towards b): the least of b along it less the most of a. */
        float Separation(const Polyhedron& a, const Polyhedron& b, const Vec3 n)
        {
            float leastA, mostA, leastB, mostB;
            a.Project(n, leastA, mostA);
            b.Project(n, leastB, mostB);
            return leastB - mostA;
        }

        /** What the separating axis test found: the best face of each, the best pair of edges. */
        struct Axes {
            float sA = -FLT_MAX, sB = -FLT_MAX, sE = -FLT_MAX;
            int faceA = 0, faceB = 0, edgeA = 0, edgeB = 0;
            Vec3 edgeAxis;
        };

        /** The separating axis test; false when some axis separates them by more than @p margin. */
        bool Separate(const Polyhedron& a, const Polyhedron& b, const float margin, Axes& axes)
        {
            for (int f = 0; f < a.faceCount; ++f) {
                float least, most;
                b.Project(a.face[f].normal, least, most);
                const float s = least - a.face[f].offset;
                if (s > margin) return false;
                if (s > axes.sA) { axes.sA = s; axes.faceA = f; }
            }
            for (int f = 0; f < b.faceCount; ++f) {
                float least, most;
                a.Project(b.face[f].normal, least, most);
                const float s = least - b.face[f].offset;
                if (s > margin) return false;
                if (s > axes.sB) { axes.sB = s; axes.faceB = f; }
            }
            const Vec3 d = b.centre - a.centre;
            for (int i = 0; i < a.edgeCount; ++i) {
                const Vec3 ea = a.vertex[a.edge[i].b] - a.vertex[a.edge[i].a];
                for (int j = 0; j < b.edgeCount; ++j) {
                    Vec3 axis = Cross(ea, b.vertex[b.edge[j].b] - b.vertex[b.edge[j].a]);
                    const float length = Length(axis);
                    if (length < 1e-6f * Length(ea)) continue;          // parallel edges: a face axis covers it
                    axis = axis * (1.f / length);
                    if (Dot(axis, d) < 0.f) axis = -axis;
                    const float s = Separation(a, b, axis);
                    if (s > margin) return false;
                    if (s > axes.sE) { axes.sE = s; axes.edgeA = i; axes.edgeB = j; axes.edgeAxis = axis; }
                }
            }
            return true;
        }

        /**
         * The edge case: of the edges parallel to the axis's, the one of a furthest along it and the one of b furthest
         * against it; the point halfway between their closest points.
         */
        void EdgeContact(const Polyhedron& a, const Polyhedron& b, const Axes& axes, Patch& out)
        {
            const Vec3 n = axes.edgeAxis;
            const Vec3 da = a.vertex[a.edge[axes.edgeA].b] - a.vertex[a.edge[axes.edgeA].a];
            const Vec3 db = b.vertex[b.edge[axes.edgeB].b] - b.vertex[b.edge[axes.edgeB].a];
            const auto support = [n](const Polyhedron& p, const Vec3 dir, const float sign) {
                int best = 0;
                float most = -FLT_MAX;
                for (int e = 0; e < p.edgeCount; ++e) {
                    const Vec3 x = p.vertex[p.edge[e].a], y = p.vertex[p.edge[e].b];
                    if (LengthSquared(Cross(y - x, dir)) > 1e-6f * LengthSquared(y - x) * LengthSquared(dir)) continue;
                    const float along = sign * Dot(n, x + y);
                    if (along > most) { most = along; best = e; }
                }
                return best;
            };
            const int ea = support(a, da, 1.f), eb = support(b, db, -1.f);
            const Vec3 a0 = a.vertex[a.edge[ea].a], a1 = a.vertex[a.edge[ea].b];
            const Vec3 b0 = b.vertex[b.edge[eb].a], b1 = b.vertex[b.edge[eb].b];
            float s = 0.f, t = 0.f;
            narrowphase::ClosestBetweenSegments(a0, a1, b0, b1, s, t);
            out.normal = n;
            out.count = 1;
            out.point[0] = ((a0 + (a1 - a0) * s) + (b0 + (b1 - b0) * t)) * 0.5f;
            out.depth[0] = -axes.sE;
            out.id[0] = Feature(64 + ea, 64 + eb);
        }

        /** Two convex polyhedra, a towards b: faces first (a's on a near tie), an edge pair only when clearly better. */
        bool PolyPoly(const Polyhedron& a, const Polyhedron& b, const float margin, Patch& out)
        {
            Axes axes;
            if (!Separate(a, b, margin, axes)) return false;
            const bool referenceIsA = !(axes.sB > 0.95f * axes.sA + 0.005f);
            const float reference = referenceIsA ? axes.sA : axes.sB;
            if (axes.sE > 0.95f * reference + 0.005f) {
                EdgeContact(a, b, axes, out);
                return true;
            }
            return referenceIsA ? FaceContact(a, axes.faceA, b, margin, out, true) : FaceContact(b, axes.faceB, a, margin, out, false);
        }

        /**
         * A sphere or a capsule (its core: a point or a segment, @p core and @p cores of them, with @p radius) against @p
         * poly, the normal out of the polyhedron. @p face, when not −1, is the face the normal must be (a mesh's triangle
         * whose join the round thing is at).
         */
        bool RoundPoly(const Polyhedron& poly, const Vec3* core, const int cores, const float radius, const float margin, Patch& out,
                       const int face = -1)
        {
            Vec3 onRound, onPoly;
            const float distance = face < 0 ? narrowphase::ClosestPoints(core, cores, poly.vertex, poly.vertexCount, onRound, onPoly) : 0.f;
            if (distance > radius + margin) return false;
            // Against one face: the face's normal; each end of a segment lying along it a point, clipped to the face.
            const auto againstFace = [&](const int f) {
                const Polyhedron::Face& pf = poly.face[f];
                const Vec3 n = pf.normal;
                Vec3 ends[2] { core[0], core[cores - 1] };
                int count = cores;
                if (cores == 2) {
                    // The segment clipped to the face's sides, as a polygon of two corners.
                    float t0 = 0.f, t1 = 1.f;
                    const Vec3 p = core[0], d = core[1] - core[0];
                    for (int side = 0; side < pf.count; ++side) {
                        const Vec3 x = poly.Corner(pf, side), y = poly.Corner(pf, side + 1);
                        Vec3 outward = Cross(y - x, n);
                        if (Normalize(outward) < 1e-9f) continue;
                        const float at = Dot(outward, p - x), rate = Dot(outward, d);
                        if (std::abs(rate) < 1e-12f) { if (at > ClipTolerance) t1 = -1.f; continue; }
                        const float t = (ClipTolerance - at) / rate;
                        if (rate > 0.f) t1 = std::min(t1, t); else t0 = std::max(t0, t);
                    }
                    if (t0 <= t1) {
                        ends[0] = p + d * t0;
                        ends[1] = p + d * t1;
                        if (LengthSquared(ends[1] - ends[0]) < MergeDistance * MergeDistance) count = 1;
                    } else {
                        // Off the face: its end nearest the face, alone.
                        count = 1;
                        ends[0] = Dot(n, core[0]) <= Dot(n, core[1]) ? core[0] : core[1];
                    }
                }
                out.normal = n;
                out.count = 0;
                for (int i = 0; i < count; ++i) {
                    if (cores == 1) {
                        // A sphere only over the face: beside it, it is another face's (or no face's) to push.
                        bool over = true;
                        for (int side = 0; side < pf.count && over; ++side) {
                            const Vec3 x = poly.Corner(pf, side), y = poly.Corner(pf, side + 1);
                            Vec3 outward = Cross(y - x, n);
                            if (Normalize(outward) < 1e-9f) continue;
                            over = Dot(outward, ends[i] - x) <= radius + ClipTolerance;
                        }
                        if (!over) continue;
                    }
                    const float s = Dot(n, ends[i]) - pf.offset - radius;
                    if (s > margin) continue;
                    out.point[out.count] = ends[i] - n * (radius + s / 2.f);
                    out.depth[out.count] = -s;
                    out.id[out.count] = Feature(f, i);
                    ++out.count;
                }
                return out.count > 0;
            };
            if (face >= 0) return againstFace(face);
            if (distance > 1e-5f) {
                const Vec3 n = (onRound - onPoly) * (1.f / distance);
                if (cores == 2) {
                    Vec3 along = core[1] - core[0];
                    if (Normalize(along) > 0.f && std::abs(Dot(along, n)) < 0.05f)
                        for (int f = 0; f < poly.faceCount; ++f)
                            if (Dot(poly.face[f].normal, n) > 0.995f) return againstFace(f);
                }
                out.normal = n;
                out.count = 1;
                out.point[0] = ((onRound - n * radius) + onPoly) * 0.5f;
                out.depth[0] = radius - distance;
                out.id[0] = Feature(255, 0);
                return true;
            }
            // The core inside the polyhedron: out through the face it is least deep behind.
            int best = 0;
            float most = -FLT_MAX;
            for (int f = 0; f < poly.faceCount; ++f) {
                float s = FLT_MAX;
                for (int i = 0; i < cores; ++i) s = std::min(s, Dot(poly.face[f].normal, core[i]) - poly.face[f].offset);
                if (s > most) { most = s; best = f; }
            }
            return againstFace(best);
        }

        /** A sphere's or a capsule's core and radius. */
        int RoundCore(const Bodies& bodies, const Colliders& colliders, const int b, Vec3* core, float& radius)
        {
            radius = colliders.size[b].x;
            if (colliders.shape[b] == Sphere) {
                core[0] = bodies.position[b];
                return 1;
            }
            narrowphase::CapsuleEnds(bodies, colliders, b, core[0], core[1]);
            return 2;
        }

        bool IsRound(const int shape) { return shape == Sphere || shape == Capsule; }

        Polyhedron PolyhedronOf(const Bodies& bodies, const Colliders& colliders, const int b)
        {
            return colliders.shape[b] == Box ? BoxPolyhedron(bodies, colliders, b) : ConePolyhedron(bodies, colliders, b);
        }

        void Write(const Patch& patch, const int a, const int b, Contacts& out)
        {
            Manifold& m = out.Begin(a, b, patch.normal);
            for (int i = 0; i < patch.count; ++i) Contacts::AddPoint(m, patch.point[i], patch.depth[i], patch.id[i]);
            out.End();
        }

        // ---- the simplex of GJK ------------------------------------------------------------------------------------------

        struct SimplexVertex {
            Vec3 w, a, b;                   // w = a − b
        };

        /**
         * The point of the simplex (1 to 4 vertices) closest to the origin, its vertices reduced to those it is made of and
         * their weights. False when the origin is inside a tetrahedron: the hulls overlap.
         */
        bool Closest(SimplexVertex* s, int& n, float* weight, Vec3& v)
        {
            const auto keep = [&](std::initializer_list<int> which, std::initializer_list<float> weights) {
                SimplexVertex kept[4];
                int i = 0;
                for (const int k : which) kept[i++] = s[k];
                n = i;
                std::copy_n(kept, n, s);
                i = 0;
                for (const float w : weights) weight[i++] = w;
                v = {};
                for (int k = 0; k < n; ++k) v += s[k].w * weight[k];
            };
            // The closest point of triangle (p, q, r) to the origin, by its Voronoi regions (Ericson 5.1.5).
            const auto triangle = [&](const int p, const int q, const int r) {
                const Vec3 a = s[p].w, b = s[q].w, c = s[r].w;
                const Vec3 ab = b - a, ac = c - a, ap = -a;
                const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
                if (d1 <= 0.f && d2 <= 0.f) return keep({ p }, { 1.f });
                const Vec3 bp = -b;
                const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
                if (d3 >= 0.f && d4 <= d3) return keep({ q }, { 1.f });
                const float vc = d1 * d4 - d3 * d2;
                if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) { const float t = d1 / (d1 - d3); return keep({ p, q }, { 1.f - t, t }); }
                const Vec3 cp = -c;
                const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
                if (d6 >= 0.f && d5 <= d6) return keep({ r }, { 1.f });
                const float vb = d5 * d2 - d1 * d6;
                if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) { const float t = d2 / (d2 - d6); return keep({ p, r }, { 1.f - t, t }); }
                const float va = d3 * d6 - d5 * d4;
                if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
                    const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                    return keep({ q, r }, { 1.f - t, t });
                }
                const float denominator = 1.f / (va + vb + vc);
                const float wb = vb * denominator, wc = vc * denominator;
                return keep({ p, q, r }, { 1.f - wb - wc, wb, wc });
            };
            switch (n) {
            case 1:
                weight[0] = 1.f;
                v = s[0].w;
                return true;
            case 2: {
                const Vec3 a = s[0].w, ab = s[1].w - a;
                const float length = Dot(ab, ab);
                const float t = length > 0.f ? std::clamp(-Dot(a, ab) / length, 0.f, 1.f) : 0.f;
                if (t <= 0.f) keep({ 0 }, { 1.f });
                else if (t >= 1.f) keep({ 1 }, { 1.f });
                else keep({ 0, 1 }, { 1.f - t, t });
                return true;
            }
            case 3:
                triangle(0, 1, 2);
                return true;
            default: {
                // Which faces of the tetrahedron the origin is outside of; the closest point of the nearest such face.
                static constexpr int faces[4][4] { { 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 3, 1 }, { 1, 2, 3, 0 } };
                SimplexVertex all[4];
                std::copy_n(s, 4, all);
                float bestDistance = FLT_MAX;
                SimplexVertex best[4];
                float bestWeight[4] {};
                int bestCount = 0;
                Vec3 bestV;
                bool outside = false;
                for (const auto& f : faces) {
                    const Vec3 a = all[f[0]].w, b = all[f[1]].w, c = all[f[2]].w, d = all[f[3]].w;
                    const Vec3 normal = Cross(b - a, c - a);
                    const float origin = Dot(normal, -a), other = Dot(normal, d - a);
                    if (origin * other >= 0.f) continue;                    // the origin on the tetrahedron's side of it
                    outside = true;
                    std::copy_n(all, 4, s);
                    s[0] = all[f[0]];
                    s[1] = all[f[1]];
                    s[2] = all[f[2]];
                    n = 3;
                    triangle(0, 1, 2);
                    const float distance = Dot(v, v);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestCount = n;
                        std::copy_n(s, n, best);
                        std::copy_n(weight, n, bestWeight);
                        bestV = v;
                    }
                }
                if (!outside) return false;
                n = bestCount;
                std::copy_n(best, n, s);
                std::copy_n(bestWeight, n, weight);
                v = bestV;
                return true;
            }
            }
        }
    }

    namespace narrowphase
    {
        float ClosestPoints(const Vec3* a, const int countA, const Vec3* b, const int countB, Vec3& onA, Vec3& onB)
        {
            const auto support = [&](const Vec3 d) {
                int ia = 0, ib = 0;
                float ma = -FLT_MAX, mb = FLT_MAX;
                for (int i = 0; i < countA; ++i)
                    if (const float x = Dot(d, a[i]); x > ma) { ma = x; ia = i; }
                for (int i = 0; i < countB; ++i)
                    if (const float x = Dot(d, b[i]); x < mb) { mb = x; ib = i; }
                return SimplexVertex { a[ia] - b[ib], a[ia], b[ib] };
            };
            SimplexVertex simplex[4];
            float weight[4] { 1.f };
            int n = 1;
            simplex[0] = { a[0] - b[0], a[0], b[0] };
            Vec3 v = simplex[0].w;
            float best = Dot(v, v);
            for (int iteration = 0; iteration < 64; ++iteration) {
                if (best < 1e-12f) return 0.f;
                const SimplexVertex w = support(-v);
                // No nearer point of the difference along −v: v is the closest.
                if (best - Dot(v, w.w) <= 1e-6f * best) break;
                bool known = false;
                for (int i = 0; i < n; ++i) known |= LengthSquared(simplex[i].w - w.w) < 1e-12f;
                if (known) break;
                simplex[n++] = w;
                SimplexVertex before[4];
                float beforeWeight[4];
                const int beforeCount = n - 1;
                std::copy_n(simplex, beforeCount, before);
                std::copy_n(weight, beforeCount, beforeWeight);
                Vec3 next;
                if (!Closest(simplex, n, weight, next)) return 0.f;
                const float distance = Dot(next, next);
                if (distance >= best) {
                    // No progress, by rounding: keep the last simplex.
                    n = beforeCount;
                    std::copy_n(before, n, simplex);
                    std::copy_n(beforeWeight, n, weight);
                    break;
                }
                best = distance;
                v = next;
            }
            onA = {};
            onB = {};
            for (int i = 0; i < n; ++i) {
                onA += simplex[i].a * weight[i];
                onB += simplex[i].b * weight[i];
            }
            return std::sqrt(best);
        }

        void ConeCorners(const Bodies& bodies, const Colliders& colliders, const int b, Vec3* corners)
        {
            const Mat3 r = FromQuat(bodies.orientation[b]);
            const float radius = colliders.size[b].x, height = colliders.size[b].y;
            const Vec3 at = bodies.position[b];
            corners[0] = at + r * Vec3 { 0.f, 0.75f * height, 0.f };
            for (int i = 0; i < ConeSides; ++i) {
                const double angle = 2.0 * std::numbers::pi * i / ConeSides;
                const Vec3 local { radius * static_cast<float>(std::cos(angle)), -0.25f * height, radius * static_cast<float>(std::sin(angle)) };
                corners[i + 1] = at + r * local;
            }
        }
    }

    // ---- the pair functions, called by Collide ---------------------------------------------------------------------------

    namespace convex
    {
        /** A cone (A) on a plane (B): each of its corners within the margin, four of them spread over its base kept. */
        void ConePlane(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            const Vec3 n = FromQuat(bodies.orientation[b]).column[1];
            Vec3 corners[ConeSides + 1];
            narrowphase::ConeCorners(bodies, colliders, a, corners);
            Vec3 points[ConeSides + 1];
            float depths[ConeSides + 1];
            int ids[ConeSides + 1], count = 0;
            for (int c = 0; c <= ConeSides; ++c) {
                const float s = Dot(n, corners[c] - bodies.position[b]);
                if (s > margin) continue;
                points[count] = corners[c] - n * (s / 2.f);
                depths[count] = -s;
                ids[count++] = c;
            }
            if (count == 0) return;
            if (count > 4) count = narrowphase::ReduceToFour(points, depths, ids, count, n);
            Manifold& m = out.Begin(a, b, -n);
            for (int i = 0; i < count; ++i) Contacts::AddPoint(m, points[i], depths[i], Feature(ids[i], 0));
            out.End();
        }

        /** A cone (A) and a sphere or capsule (B). */
        void ConeRound(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Vec3 core[2];
            float radius = 0.f;
            const int cores = RoundCore(bodies, colliders, b, core, radius);
            Patch patch;
            if (RoundPoly(ConePolyhedron(bodies, colliders, a), core, cores, radius, margin, patch)) Write(patch, a, b, out);
        }

        /** A cone (A) and a box or a cone (B). */
        void ConePoly(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Patch patch;
            if (PolyPoly(ConePolyhedron(bodies, colliders, a), PolyhedronOf(bodies, colliders, b), margin, patch)) Write(patch, a, b, out);
        }

        /**
         * A mesh (A) and anything that moves but a plane or a mesh (B): each triangle near B, as a polyhedron of one
         * face; then the contacts grouped by the way they push, one manifold each, four points at most.
         */
        void MeshAny(const Bodies& bodies, const Colliders& colliders, const int a, const int b, float margin, Contacts& out, const float sweep)
        {
            const TriangleMesh* mesh = colliders.mesh[a].get();
            if (!mesh) return;
            const Mat3 r = FromQuat(bodies.orientation[a]);
            const Vec3 at = bodies.position[a];
            const bool round = IsRound(colliders.shape[b]);
            Vec3 core[2];
            float radius = 0.f;
            int cores = 0;
            Polyhedron body;
            Vec3 lower { FLT_MAX, FLT_MAX, FLT_MAX }, upper { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            const auto around = [&](const Vec3 p, const float grow) {
                for (int c = 0; c < 3; ++c) {
                    lower[c] = std::min(lower[c], p[c] - grow);
                    upper[c] = std::max(upper[c], p[c] + grow);
                }
            };
            if (round) {
                cores = RoundCore(bodies, colliders, b, core, radius);
                for (int i = 0; i < cores; ++i) around(core[i], radius);
            } else {
                body = PolyhedronOf(bodies, colliders, b);
                for (int i = 0; i < body.vertexCount; ++i) around(body.vertex[i], 0.f);
            }
            // A thin wall has no inside to catch a body that has passed it: contacts as far as the two close in the step, and
            // a body that has gone through by no more than that is still pushed back out (see `behind`).
            float travel = 0.f;
            if (sweep > 0.f) {
                const float spread = 0.5f * Length(upper - lower);
                const float closing = Length(bodies.velocity[b] - bodies.velocity[a]) + Length(bodies.angularVelocity[b]) * spread
                                    + Length(bodies.angularVelocity[a]) * Length(bodies.position[b] - bodies.position[a]);
                travel = closing * sweep;
                margin += travel;
            }
            // B's box, into the mesh's frame: around the box turned back.
            const Vec3 centre = Rotate(Conjugate(bodies.orientation[a]), (lower + upper) * 0.5f - at);
            const Vec3 half = (upper - lower) * 0.5f;
            const Mat3 back = Transpose(r);
            Vec3 reach;
            for (int c = 0; c < 3; ++c)
                reach[c] = std::abs(back.column[0][c]) * half.x + std::abs(back.column[1][c]) * half.y + std::abs(back.column[2][c]) * half.z + margin
                         + colliders.size[a].x;

            const Vec3 bodyCentre = bodies.position[b];
            // How far behind a triangle B's centre may be and still be pushed back out by it: half its own least size. A
            // body pressed into a thin wall (a crowd against it) has its centre go behind before the solver has it out.
            const Vec3 extent = upper - lower;
            // A mesh given a thickness is a solid wall that deep: anything less far behind than that is pushed back out.
            const float behind = std::max(0.25f * std::min({ extent.x, extent.y, extent.z }) + travel, colliders.size[a].x);

            Patch patches[64];
            int patchCount = 0;
            mesh->Query(centre - reach, centre + reach, [&](const int t) {
                if (patchCount == 64) return;
                const TriangleMesh::Triangle& tri = mesh->Triangles()[t];
                const Vec3 p0 = at + r * mesh->Vertices()[tri.v[0]], p1 = at + r * mesh->Vertices()[tri.v[1]], p2 = at + r * mesh->Vertices()[tri.v[2]];
                const Vec3 n = r * tri.normal;
                const float side = Dot(n, bodyCentre - p0);
                if (side < -behind) return;                                 // behind it: one-sided
                const Polyhedron triangle = TrianglePolyhedron(p0, p1, p2, n);
                Patch patch;
                if (side < 0.f) {
                    // Its centre behind the face, in the wall: out the way the face faces, whatever else.
                    if (!(round ? RoundPoly(triangle, core, cores, radius, margin, patch, 0) : FaceContact(triangle, 0, body, margin, patch, true))) return;
                    for (int i = 0; i < patch.count; ++i) patch.id[i] = Feature(t & 0xFFFF, FeatureOfB(patch.id[i]));
                    patches[patchCount++] = patch;
                    return;
                }
                bool touching = round ? RoundPoly(triangle, core, cores, radius, margin, patch) : PolyPoly(triangle, body, margin, patch);
                if (!touching) return;
                // Pushed out across an edge that is not sharp (a flat join, a hollow): a ghost of the next triangle's face.
                // The face's own normal instead.
                const Vec3 across = patch.normal - n * Dot(patch.normal, n);
                if (Dot(patch.normal, n) < 0.9999f) {
                    bool sharp = false;
                    const Vec3 corner[3] { p0, p1, p2 };
                    for (int e = 0; e < 3; ++e) {
                        const Vec3 outward = Cross(corner[(e + 1) % 3] - corner[e], n);
                        if (Dot(outward, across) > 1e-4f * Length(outward) && (tri.sharp & (1u << e))) sharp = true;
                    }
                    if (!sharp) {
                        touching = round ? RoundPoly(triangle, core, cores, radius, margin, patch, 0) : FaceContact(triangle, 0, body, margin, patch, true);
                        if (!touching) return;
                    }
                }
                for (int i = 0; i < patch.count; ++i) patch.id[i] = Feature(t & 0xFFFF, FeatureOfB(patch.id[i]));
                patches[patchCount++] = patch;
            });
            // Grouped by the way they push: each group one manifold.
            bool used[64] {};
            for (int i = 0; i < patchCount; ++i) {
                if (used[i]) continue;
                Vec3 points[64 * 4];
                float depths[64 * 4];
                int ids[64 * 4], count = 0;
                const Vec3 normal = patches[i].normal;
                for (int j = i; j < patchCount; ++j) {
                    if (used[j] || Dot(patches[j].normal, normal) < 0.98f) continue;
                    used[j] = true;
                    for (int k = 0; k < patches[j].count; ++k) {
                        points[count] = patches[j].point[k];
                        depths[count] = patches[j].depth[k];
                        ids[count++] = patches[j].id[k];
                    }
                }
                count = Merge(points, depths, ids, count);
                if (count > 4) count = narrowphase::ReduceToFour(points, depths, ids, count, normal);
                Manifold& m = out.Begin(a, b, normal);
                for (int k = 0; k < count; ++k) Contacts::AddPoint(m, points[k], depths[k], ids[k]);
                out.End();
            }
        }
    }
}
