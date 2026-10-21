// The narrowphase: for one pair of bodies, whether they touch, and if so a manifold. Each pair function exists for one
// order only (named A–B, the higher rank first); Collide swaps the bodies, then turns the manifold round, when they come
// the other way. Every pair function writes nothing when the shapes are further apart than the margin, and otherwise
// exactly one manifold. Each point is halfway between the surfaces: (pA + pB) / 2.
//
// The frames, as the engine draws them: a box's faces are its local ±X, ±Y, ±Z at its half-extents; a capsule's
// segment runs along its local Y; a plane faces its local +Y through its position. Local axes in the world are the
// columns of FromQuat(orientation).

#include <kphysics/collision.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace kphysics
{
    // Cones and meshes (convex.cpp).
    namespace convex
    {
        void ConePlane(const Bodies& bodies, const Colliders& colliders, int a, int b, float margin, Contacts& out);
        void ConeRound(const Bodies& bodies, const Colliders& colliders, int a, int b, float margin, Contacts& out);
        void ConePoly(const Bodies& bodies, const Colliders& colliders, int a, int b, float margin, Contacts& out);
        void MeshAny(const Bodies& bodies, const Colliders& colliders, int a, int b, float margin, Contacts& out, float sweep);
    }

    namespace
    {
        /** Contact points of one face closer than this are one point. */
        constexpr float MergeDistance = 2e-3f;
        /** How far outside the reference face's sides an incident corner may be and still be kept whole. */
        constexpr float ClipTolerance = 1e-3f;

        /** Which shape goes first in a pair function: the higher rank is A. */
        int Rank(const int shape)
        {
            switch (shape) {
            case Plane: return 0;
            case Sphere: return 1;
            case Capsule: return 2;
            case Box: return 3;
            case Cone: return 4;
            default: return 5;
            }
        }

        /** A contact as the helpers work one out: a normal from A to B, a point and a depth. */
        struct Touch {
            Vec3 normal, at;
            float depth = 0.f;
        };

        /** Two spheres, by centre and radius: false when further apart than @p margin. */
        bool SphereSphere(const Vec3 centreA, const float radiusA, const Vec3 centreB, const float radiusB, const float margin, Touch& out)
        {
            Vec3 d = centreB - centreA;
            const float distance = Length(d);
            if (distance > radiusA + radiusB + margin) return false;
            const Vec3 normal = distance < Epsilon ? Vec3 { 0.f, 1.f, 0.f } : d * (1.f / distance);
            out.normal = normal;
            out.at = ((centreA + normal * radiusA) + (centreB + normal * -radiusB)) * 0.5f;
            out.depth = radiusA + radiusB - distance;
            return true;
        }

        /**
         * A sphere against @p box: from the box's closest point out to the centre when outside; when inside, out through
         * the nearest face (the lowest axis on a tie). False when further than @p margin.
         */
        bool SphereBox(const Bodies& bodies, const Colliders& colliders, const int box, const Vec3 centre, const float radius,
                       const float margin, Touch& out)
        {
            Vec3 closest;
            if (!narrowphase::ClosestOnBox(bodies, colliders, box, centre, closest)) {
                const Vec3 d = centre - closest;
                const float distance = Length(d);
                if (distance > radius + margin) return false;
                out.normal = d * (1.f / distance);
                out.at = (closest + (centre + out.normal * -radius)) * 0.5f;
                out.depth = radius - distance;
                return true;
            }
            const Mat3 axes = FromQuat(bodies.orientation[box]);
            const Vec3 local = centre - bodies.position[box];
            int axis = 0;
            float gap = FLT_MAX;
            float side = 1.f;
            for (int k = 0; k < 3; ++k) {
                const float x = Dot(axes.column[k], local);
                const float g = colliders.size[box][k] - std::abs(x);
                if (g < gap) { gap = g; axis = k; side = x >= 0.f ? 1.f : -1.f; }
            }
            out.normal = axes.column[axis] * side;                          // that face's outward normal
            out.at = ((centre + out.normal * gap) + (centre + out.normal * -radius)) * 0.5f;
            out.depth = radius + gap;
            return true;
        }

        /** A sphere against @p plane (its normal @p n): a point of manifold @p m when within @p margin. */
        void SpherePlanePoint(const Bodies& bodies, const int plane, const Vec3 n, const Vec3 centre, const float radius,
                              const float margin, Manifold& m, const int id)
        {
            const float s = Dot(n, centre - bodies.position[plane]) - radius;
            if (s > margin) return;
            Contacts::AddPoint(m, centre + n * -(radius + s / 2.f), -s, id);
        }

        // ---- spheres ------------------------------------------------------------------------------------------------

        /** Sphere A on plane B: n_p the plane's normal, s = n_p · (centre − plane) − r; normal −n_p, depth −s. */
        void SpherePlane(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            const float r = colliders.size[a].x;
            const Vec3 n = FromQuat(bodies.orientation[b]).column[1];
            const float s = Dot(n, bodies.position[a] - bodies.position[b]) - r;
            if (s > margin) return;
            Manifold& m = out.Begin(a, b, -n);
            Contacts::AddPoint(m, bodies.position[a] + n * -(r + s / 2.f), -s, Feature(0, 0));
            out.End();
        }

        void SphereSphere(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Touch t;
            if (!SphereSphere(bodies.position[a], colliders.size[a].x, bodies.position[b], colliders.size[b].x, margin, t)) return;
            Manifold& m = out.Begin(a, b, t.normal);
            Contacts::AddPoint(m, t.at, t.depth, Feature(0, 0));
            out.End();
        }

        // ---- capsules -----------------------------------------------------------------------------------------------

        /** Capsule A on plane B: each end a sphere against the plane; feature(0, 0) the −half end, feature(1, 0) the +half. */
        void CapsulePlane(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Vec3 lower, upper;
            narrowphase::CapsuleEnds(bodies, colliders, a, lower, upper);
            const Vec3 n = FromQuat(bodies.orientation[b]).column[1];
            const float r = colliders.size[a].x;
            Manifold& m = out.Begin(a, b, -n);
            SpherePlanePoint(bodies, b, n, lower, r, margin, m, Feature(0, 0));
            SpherePlanePoint(bodies, b, n, upper, r, margin, m, Feature(1, 0));
            out.End();                                                      // neither end close: no manifold
        }

        /** Capsule A and sphere B: the closest point of A's segment to B's centre, as two spheres. feature(2, 0). */
        void CapsuleSphere(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Vec3 lower, upper;
            narrowphase::CapsuleEnds(bodies, colliders, a, lower, upper);
            const float t = narrowphase::ClosestOnSegment(bodies.position[b], lower, upper);
            const Vec3 q = lower + (upper - lower) * t;
            Touch touch;
            if (!SphereSphere(q, colliders.size[a].x, bodies.position[b], colliders.size[b].x, margin, touch)) return;
            Manifold& m = out.Begin(a, b, touch.normal);
            Contacts::AddPoint(m, touch.at, touch.depth, Feature(2, 0));
            out.End();
        }

        /**
         * Capsule A and capsule B: the closest points of the segments as two spheres (feature(2, 2)), except when the
         * segments are parallel and side by side, where one point would let a lying capsule rock: then a point at each
         * end of where they overlap (feature(0, 0) and feature(1, 1)), the normal the first's.
         */
        void CapsuleCapsule(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Vec3 a0, a1, b0, b1;
            narrowphase::CapsuleEnds(bodies, colliders, a, a0, a1);
            narrowphase::CapsuleEnds(bodies, colliders, b, b0, b1);
            const float ra = colliders.size[a].x, rb = colliders.size[b].x;
            const Vec3 dA = a1 - a0, dB = b1 - b0;
            const float lengthA = Dot(dA, dA), lengthB = Dot(dB, dB);
            const Vec3 crossed = Cross(dA, dB);

            if (lengthA > 1e-12f && lengthB > 1e-12f && Dot(crossed, crossed) <= 1e-6f * lengthA * lengthB) {
                const float t0 = Dot(b0 - a0, dA) / lengthA;                // B's ends, as t along A
                const float t1 = Dot(b1 - a0, dA) / lengthA;
                const float lo = std::max(0.f, std::min(t0, t1));
                const float hi = std::min(1.f, std::max(t0, t1));
                if (hi - lo > 1e-4f) {
                    // A's point at t against B's segment, as two spheres.
                    const auto overlap = [&](const float t, Touch& touch) {
                        const Vec3 p = a0 + dA * t;
                        const float u = narrowphase::ClosestOnSegment(p, b0, b1);
                        return SphereSphere(p, ra, b0 + dB * u, rb, margin, touch);
                    };
                    Touch first, second;
                    const bool hasFirst = overlap(lo, first), hasSecond = overlap(hi, second);
                    if (!hasFirst && !hasSecond) return;
                    Manifold& m = out.Begin(a, b, hasFirst ? first.normal : second.normal);
                    if (hasFirst) Contacts::AddPoint(m, first.at, first.depth, Feature(0, 0));
                    if (hasSecond) Contacts::AddPoint(m, second.at, second.depth, Feature(1, 1));
                    out.End();
                    return;
                }
            }

            float s = 0.f, t = 0.f;
            narrowphase::ClosestBetweenSegments(a0, a1, b0, b1, s, t);
            Touch touch;
            if (!SphereSphere(a0 + dA * s, ra, b0 + dB * t, rb, margin, touch)) return;
            Manifold& m = out.Begin(a, b, touch.normal);
            Contacts::AddPoint(m, touch.at, touch.depth, Feature(2, 2));
            out.End();
        }

        // ---- boxes on planes ----------------------------------------------------------------------------------------

        /**
         * Box A on plane B: every corner within the margin is a point (feature(corner, 0)), halfway to the plane. A box
         * sunk deeper than its own size could have more than four corners below: the four deepest are kept.
         */
        void BoxPlane(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            const Vec3 n = FromQuat(bodies.orientation[b]).column[1];
            Vec3 points[8];
            float gaps[8];
            int ids[8];
            int count = 0;
            for (int c = 0; c < 8; ++c) {
                const Vec3 corner = narrowphase::BoxCorner(bodies, colliders, a, c);
                const float s = Dot(n, corner - bodies.position[b]);
                if (s > margin) continue;
                points[count] = corner + n * (-s / 2.f);
                gaps[count] = s;
                ids[count] = c;
                ++count;
            }
            if (count == 0) return;
            // The four deepest (the lowest s) first.
            const int kept = std::min(count, 4);
            for (int i = 0; i < kept; ++i) {
                int deepest = i;
                for (int q = i + 1; q < count; ++q)
                    if (gaps[q] < gaps[deepest]) deepest = q;
                if (deepest == i) continue;
                std::swap(points[i], points[deepest]);
                std::swap(gaps[i], gaps[deepest]);
                std::swap(ids[i], ids[deepest]);
            }
            Manifold& m = out.Begin(a, b, -n);
            for (int i = 0; i < kept; ++i) Contacts::AddPoint(m, points[i], -gaps[i], Feature(ids[i], 0));
            out.End();
        }

        // ---- boxes and spheres --------------------------------------------------------------------------------------

        void BoxSphere(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Touch t;
            if (!SphereBox(bodies, colliders, a, bodies.position[b], colliders.size[b].x, margin, t)) return;
            Manifold& m = out.Begin(a, b, t.normal);
            Contacts::AddPoint(m, t.at, t.depth, Feature(0, 0));
            out.End();
        }

        // ---- box against box ----------------------------------------------------------------------------------------

        /** The separation of boxes a and b along @p axis: |d · L| − (rA + rB). */
        float BoxSeparation(const Colliders& colliders, const int a, const int b, const Mat3& axesA, const Mat3& axesB,
                            const Vec3 d, const Vec3 axis)
        {
            float ra = 0.f, rb = 0.f;
            for (int k = 0; k < 3; ++k) {
                ra += colliders.size[a][k] * std::abs(Dot(axesA.column[k], axis));
                rb += colliders.size[b][k] * std::abs(Dot(axesB.column[k], axis));
            }
            return std::abs(Dot(d, axis)) - (ra + rb);
        }

        void SwapPoints(Vec3* points, float* depths, int* ids, const int x, const int y)
        {
            if (x == y) return;
            std::swap(points[x], points[y]);
            std::swap(depths[x], depths[y]);
            std::swap(ids[x], ids[y]);
        }

        /** Merges points closer than MergeDistance, keeping the deeper of each pair; returns how many are left. */
        int MergeClose(Vec3* points, float* depths, int* ids, const int count)
        {
            int n = count;
            for (int i = 0; i < n; ++i) {
                int j = i + 1;
                while (j < n) {
                    if (LengthSquared(points[i] - points[j]) < MergeDistance * MergeDistance) {
                        if (depths[j] > depths[i]) SwapPoints(points, depths, ids, i, j);
                        SwapPoints(points, depths, ids, j, n - 1);          // the shallower goes to the end, and is dropped
                        --n;
                    } else {
                        ++j;
                    }
                }
            }
            return n;
        }

        /**
         * The face case: reference face @p face (0 +X, 1 −X, 2 +Y, 3 −Y, 4 +Z, 5 −Z of box @p ref) facing box @p inc, its
         * outward normal @p n. The incident face (inc's most against n) is clipped by the reference face's four sides
         * (a millimetre wider, so corners right on its edges stay whole), the points within the margin kept, each put
         * back onto the face and halfway to it, merged, and reduced to four.
         */
        void BoxBoxFace(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const int ref, const int inc,
                        const int face, const Vec3 n, const float margin, Contacts& out)
        {
            const Mat3 axesRef = FromQuat(bodies.orientation[ref]);
            const Mat3 axesInc = FromQuat(bodies.orientation[inc]);

            // 1. The incident face: its outward normal most against n.
            int incident = 0;
            float lowest = FLT_MAX;
            for (int f = 0; f < 6; ++f) {
                const float along = (f % 2 == 0 ? 1.f : -1.f) * Dot(axesInc.column[f / 2], n);
                if (along < lowest) { lowest = along; incident = f; }
            }
            // Its four corners, round the face: (−u, −w), (+u, −w), (+u, +w), (−u, +w), ids 0 to 3.
            const int fi = incident / 2, fu = (fi + 1) % 3, fw = (fi + 2) % 3;
            Vec3 polygonA[8], polygonB[8];
            int idsA[8], idsB[8];
            for (int q = 0; q < 4; ++q) {
                Vec3 local;
                local[fi] = (incident % 2 == 0 ? 1.f : -1.f) * colliders.size[inc][fi];
                local[fu] = (q == 1 || q == 2 ? 1.f : -1.f) * colliders.size[inc][fu];
                local[fw] = (q >= 2 ? 1.f : -1.f) * colliders.size[inc][fw];
                polygonA[q] = bodies.position[inc] + axesInc * local;
                idsA[q] = q;
            }

            // 2. Clipped by the reference face's four side planes, back and forth between the two buffers.
            const int k = face / 2;
            int count = 4;
            Vec3* points = polygonA;
            int* ids = idsA;
            for (int side = 0; side < 4; ++side) {
                const int t = side < 2 ? (k + 1) % 3 : (k + 2) % 3;
                const Vec3 plane = axesRef.column[t] * (side % 2 == 0 ? 1.f : -1.f);
                const float offset = Dot(plane, bodies.position[ref]) + colliders.size[ref][t] + ClipTolerance;
                Vec3* toPoints = points == polygonA ? polygonB : polygonA;
                int* toIds = ids == idsA ? idsB : idsA;
                count = narrowphase::ClipPolygon(points, ids, count, plane, offset, side + 1, toPoints, toIds);
                points = toPoints;
                ids = toIds;
                if (count == 0) return;
            }

            // 3. The points within the margin of the reference face, each put back within its sides and moved halfway to it.
            const float faceOffset = Dot(n, bodies.position[ref]) + colliders.size[ref][k];
            float depths[8];
            int kept = 0;
            for (int q = 0; q < count; ++q) {
                const float s = Dot(n, points[q]) - faceOffset;
                if (s > margin) continue;
                Vec3 p = points[q];
                for (int side = 1; side <= 2; ++side) {
                    const int t = (k + side) % 3;
                    const float h = colliders.size[ref][t];
                    const float along = Dot(axesRef.column[t], p - bodies.position[ref]);
                    if (along > h) p += axesRef.column[t] * (h - along);
                    else if (along < -h) p += axesRef.column[t] * (-h - along);
                }
                points[kept] = p + n * (-s / 2.f);
                depths[kept] = -s;
                ids[kept] = ids[q];
                ++kept;
            }
            if (kept == 0) return;
            kept = MergeClose(points, depths, ids, kept);
            // 4. More than four: the four that hold the face up best.
            if (kept > 4) kept = narrowphase::ReduceToFour(points, depths, ids, kept, n);

            // 5. From A to B, whichever box the face is on.
            Manifold& m = out.Begin(a, b, ref == a ? n : -n);
            for (int q = 0; q < kept; ++q) Contacts::AddPoint(m, points[q], depths[q], ref == a ? Feature(face, ids[q]) : Feature(ids[q], face));
            out.End();
        }

        /**
         * The edge case: edge axis i of box a crossed with edge axis j of box b, the normal from a to b. a's edge is the
         * one furthest along the normal, b's the one furthest against it; the point is halfway between their closest
         * points, the depth the separation negated. feature(8 + i, 8 + j).
         */
        void BoxBoxEdges(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const int i, const int j,
                         const Vec3 n, const float separation, Contacts& out)
        {
            const Mat3 axesA = FromQuat(bodies.orientation[a]);
            const Mat3 axesB = FromQuat(bodies.orientation[b]);
            Vec3 centre = bodies.position[a];
            for (int k = 0; k < 3; ++k)
                if (k != i) centre += axesA.column[k] * ((Dot(axesA.column[k], n) >= 0.f ? 1.f : -1.f) * colliders.size[a][k]);
            const Vec3 a0 = centre + axesA.column[i] * -colliders.size[a][i];
            const Vec3 a1 = centre + axesA.column[i] * colliders.size[a][i];
            centre = bodies.position[b];
            for (int k = 0; k < 3; ++k)
                if (k != j) centre += axesB.column[k] * ((Dot(axesB.column[k], n) >= 0.f ? -1.f : 1.f) * colliders.size[b][k]);
            const Vec3 b0 = centre + axesB.column[j] * -colliders.size[b][j];
            const Vec3 b1 = centre + axesB.column[j] * colliders.size[b][j];
            float s = 0.f, t = 0.f;
            narrowphase::ClosestBetweenSegments(a0, a1, b0, b1, s, t);
            const Vec3 pA = a0 + (a1 - a0) * s, pB = b0 + (b1 - b0) * t;
            Manifold& m = out.Begin(a, b, n);
            Contacts::AddPoint(m, (pA + pB) * 0.5f, -separation, Feature(8 + i, 8 + j));
            out.End();
        }

        /**
         * Box A and box B, by the separating axis test, then clipping: the three face axes of each and the nine edge
         * crossings. Faces are preferred (A's on a near tie), and an edge pair wins only when clearly better, so the
         * choice does not flicker between two nearly equal axes from step to step: a stack rests only if the same face
         * is chosen every step.
         */
        void BoxBox(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            const Mat3 axesA = FromQuat(bodies.orientation[a]);
            const Mat3 axesB = FromQuat(bodies.orientation[b]);
            const Vec3 d = bodies.position[b] - bodies.position[a];

            float sA = -FLT_MAX, sB = -FLT_MAX;
            int faceA = 0, faceB = 0;
            for (int i = 0; i < 3; ++i) {
                const float s1 = BoxSeparation(colliders, a, b, axesA, axesB, d, axesA.column[i]);
                if (s1 > margin) return;
                if (s1 > sA) { sA = s1; faceA = i; }
                const float s2 = BoxSeparation(colliders, a, b, axesA, axesB, d, axesB.column[i]);
                if (s2 > margin) return;
                if (s2 > sB) { sB = s2; faceB = i; }
            }
            float sE = -FLT_MAX;
            int edgeA = 0, edgeB = 0;
            Vec3 edgeAxis;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    Vec3 axis = Cross(axesA.column[i], axesB.column[j]);
                    const float length = Length(axis);
                    if (length < 1e-6f) continue;                           // parallel edges: a face axis covers it
                    axis = axis * (1.f / length);
                    const float s = BoxSeparation(colliders, a, b, axesA, axesB, d, axis);
                    if (s > margin) return;
                    if (s > sE) { sE = s; edgeA = i; edgeB = j; edgeAxis = axis; }
                }

            const bool referenceIsA = !(sB > 0.95f * sA + 0.005f);
            const float sReference = referenceIsA ? sA : sB;
            if (sE > 0.95f * sReference + 0.005f) {
                if (Dot(d, edgeAxis) < 0.f) edgeAxis = -edgeAxis;            // from A towards B
                BoxBoxEdges(bodies, colliders, a, b, edgeA, edgeB, edgeAxis, sE, out);
                return;
            }
            if (referenceIsA) {
                // The face of A towards B: ±uA_i, whichever points along d.
                const Vec3 u = axesA.column[faceA];
                const float sign = Dot(d, u) < 0.f ? -1.f : 1.f;
                BoxBoxFace(bodies, colliders, a, b, a, b, faceA * 2 + (sign > 0.f ? 0 : 1), u * sign, margin, out);
            } else {
                // The face of B towards A: its outward normal points against d.
                const Vec3 u = axesB.column[faceB];
                const float sign = Dot(d, u) > 0.f ? -1.f : 1.f;
                BoxBoxFace(bodies, colliders, a, b, b, a, faceB * 2 + (sign > 0.f ? 0 : 1), u * sign, margin, out);
            }
        }

        // ---- boxes and capsules -------------------------------------------------------------------------------------

        /**
         * Box A and capsule B: each end of B's segment as a sphere against the box (feature(0, 0) and feature(0, 1));
         * if neither touches, the segment's point nearest the box, found by going back and forth four times from its
         * middle (feature(0, 2)). The manifold's normal is its deepest point's.
         */
        void BoxCapsule(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out)
        {
            Vec3 lower, upper;
            narrowphase::CapsuleEnds(bodies, colliders, b, lower, upper);
            const float r = colliders.size[b].x;
            Touch first, second;
            const bool hasFirst = SphereBox(bodies, colliders, a, lower, r, margin, first);
            const bool hasSecond = SphereBox(bodies, colliders, a, upper, r, margin, second);
            bool middle = false;
            if (!hasFirst && !hasSecond) {
                Vec3 q = (lower + upper) * 0.5f;
                const Vec3 segment = upper - lower;
                for (int k = 0; k < 4; ++k) {
                    Vec3 x;
                    narrowphase::ClosestOnBox(bodies, colliders, a, q, x);
                    q = lower + segment * narrowphase::ClosestOnSegment(x, lower, upper);
                }
                middle = SphereBox(bodies, colliders, a, q, r, margin, first);
                if (!middle) return;
            }
            const Touch& deepest = middle || (hasFirst && (!hasSecond || first.depth >= second.depth)) ? first : second;
            Manifold& m = out.Begin(a, b, deepest.normal);
            if (middle) Contacts::AddPoint(m, first.at, first.depth, Feature(0, 2));
            if (hasFirst) Contacts::AddPoint(m, first.at, first.depth, Feature(0, 0));
            if (hasSecond) Contacts::AddPoint(m, second.at, second.depth, Feature(0, 1));
            out.End();
        }

        /** (e × (p − p0)) · n: which side of the line from p0 along e point p is, and how far. */
        float TriangleSide(const Vec3 p, const Vec3 p0, const Vec3 e, const Vec3 n) { return Dot(Cross(e, p - p0), n); }
    }

    void Collide(const Bodies& bodies, const Colliders& colliders, const int a, const int b, const float margin, Contacts& out, const float sweep)
    {
        const bool swap = Rank(colliders.shape[a]) < Rank(colliders.shape[b]);
        const int first = swap ? b : a;
        const int second = swap ? a : b;
        const int before = out.Count();
        const int other = colliders.shape[second];
        switch (colliders.shape[first]) {
        case Sphere:
            if (other == Plane) SpherePlane(bodies, colliders, first, second, margin, out);
            else if (other == Sphere) SphereSphere(bodies, colliders, first, second, margin, out);
            break;
        case Capsule:
            if (other == Plane) CapsulePlane(bodies, colliders, first, second, margin, out);
            else if (other == Sphere) CapsuleSphere(bodies, colliders, first, second, margin, out);
            else if (other == Capsule) CapsuleCapsule(bodies, colliders, first, second, margin, out);
            break;
        case Box:
            if (other == Plane) BoxPlane(bodies, colliders, first, second, margin, out);
            else if (other == Sphere) BoxSphere(bodies, colliders, first, second, margin, out);
            else if (other == Capsule) BoxCapsule(bodies, colliders, first, second, margin, out);
            else if (other == Box) BoxBox(bodies, colliders, first, second, margin, out);
            break;
        case Cone:
            if (other == Plane) convex::ConePlane(bodies, colliders, first, second, margin, out);
            else if (other == Sphere || other == Capsule) convex::ConeRound(bodies, colliders, first, second, margin, out);
            else convex::ConePoly(bodies, colliders, first, second, margin, out);
            break;
        case Mesh:
            // Against a plane or another mesh: two things nothing moves, never tested.
            if (other != Plane && other != Mesh) convex::MeshAny(bodies, colliders, first, second, margin, out, sweep);
            break;
        default: break;                                                     // plane–plane: two static planes never meet
        }
        if (swap)
            for (int m = before; m < out.Count(); ++m) Contacts::Flip(out.manifolds[m]);
    }

    namespace narrowphase
    {
        float ClosestOnSegment(const Vec3 p, const Vec3 a, const Vec3 b)
        {
            const Vec3 ab = b - a;
            const float t = std::clamp(Dot(ab, p - a) / Dot(ab, ab), 0.f, 1.f);
            return std::isnan(t) ? 0.f : t;
        }

        void ClosestBetweenSegments(const Vec3 p1, const Vec3 q1, const Vec3 p2, const Vec3 q2, float& s, float& t)
        {
            const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
            const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
            const float c = Dot(d1, r), b = Dot(d1, d2);
            if (a < Epsilon && e < Epsilon) { s = 0.f; t = 0.f; return; }
            if (a < Epsilon) { s = 0.f; t = ClosestOnSegment(p1, p2, q2); return; }
            if (e < Epsilon) { s = ClosestOnSegment(p2, p1, q1); t = 0.f; return; }
            const float denominator = a * e - b * b;
            const float s0 = denominator != 0.f ? std::clamp((b * f - c * e) / denominator, 0.f, 1.f) : 0.f;
            const float t0 = (b * s0 + f) / e;
            if (t0 < 0.f) { t = 0.f; s = std::clamp(-c / a, 0.f, 1.f); }
            else if (t0 > 1.f) { t = 1.f; s = std::clamp((b - c) / a, 0.f, 1.f); }
            else { t = t0; s = s0; }
        }

        void CapsuleEnds(const Bodies& bodies, const Colliders& colliders, const int a, Vec3& lower, Vec3& upper)
        {
            const Vec3 axis = FromQuat(bodies.orientation[a]).column[1];
            const float half = colliders.size[a].y;
            lower = bodies.position[a] + axis * -half;
            upper = bodies.position[a] + axis * half;
        }

        Vec3 BoxCorner(const Bodies& bodies, const Colliders& colliders, const int b, const int corner)
        {
            Vec3 local;
            for (int c = 0; c < 3; ++c) local[c] = ((corner >> c) & 1) == 1 ? colliders.size[b][c] : -colliders.size[b][c];
            return bodies.position[b] + FromQuat(bodies.orientation[b]) * local;
        }

        bool ClosestOnBox(const Bodies& bodies, const Colliders& colliders, const int b, const Vec3 p, Vec3& closest)
        {
            const Mat3 axes = FromQuat(bodies.orientation[b]);
            const Vec3 d = p - bodies.position[b];
            bool inside = true;
            Vec3 local;
            for (int c = 0; c < 3; ++c) {
                const float x = Dot(axes.column[c], d);
                const float h = colliders.size[b][c];
                if (x > h || x < -h) inside = false;
                local[c] = std::clamp(x, -h, h);
            }
            closest = bodies.position[b] + axes * local;
            return inside;
        }

        int ClipPolygon(const Vec3* points, const int* ids, const int count, const Vec3 n, const float d, const int clipId,
                        Vec3* out, int* outIds)
        {
            int kept = 0;
            for (int q = 0; q < count; ++q) {
                const int r = q + 1 == count ? 0 : q + 1;
                const float dp = Dot(n, points[q]) - d;
                const float dq = Dot(n, points[r]) - d;
                if (dp <= 0.f) {
                    out[kept] = points[q];
                    outIds[kept] = ids[q];
                    ++kept;
                }
                if ((dp <= 0.f) != (dq <= 0.f)) {
                    const float t = dp / (dp - dq);                         // where the edge crosses the plane
                    out[kept] = points[q] + (points[r] - points[q]) * t;
                    outIds[kept] = 4 * clipId + ids[q] % 4;
                    ++kept;
                }
            }
            return kept;
        }

        int ReduceToFour(Vec3* points, float* depths, int* ids, const int count, const Vec3 normal)
        {
            // Each one chosen is swapped to the front, so the first four are the ones kept. 1. The deepest.
            int best = 0;
            for (int q = 1; q < count; ++q)
                if (depths[q] > depths[best]) best = q;
            SwapPoints(points, depths, ids, 0, best);
            // 2. The farthest from it.
            best = 1;
            float farthest = -1.f;
            for (int q = 1; q < count; ++q) {
                const float distance = LengthSquared(points[q] - points[0]);
                if (distance > farthest) { farthest = distance; best = q; }
            }
            SwapPoints(points, depths, ids, 1, best);
            // 3 and 4. The largest triangle with those two on each side of their line.
            const Vec3 e = points[1] - points[0];
            float most = -FLT_MAX;
            best = 2;
            for (int q = 2; q < count; ++q) {
                const float area = TriangleSide(points[q], points[0], e, normal);
                if (area > most) { most = area; best = q; }
            }
            SwapPoints(points, depths, ids, 2, best);
            float least = FLT_MAX;
            best = 3;
            for (int q = 3; q < count; ++q) {
                const float area = TriangleSide(points[q], points[0], e, normal);
                if (area < least) { least = area; best = q; }
            }
            SwapPoints(points, depths, ids, 3, best);
            return 4;
        }
    }
}
