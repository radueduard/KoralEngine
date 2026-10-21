//
// kphysics: the broadphase, which pairs of bodies are close enough to be worth the narrowphase. Each body gets a box
// aligned with the world's axes around its collider, widened by the contact margin; two bodies are a pair when their
// boxes overlap.
//
// Four ways to find the pairs, which find the same ones:
//   AllPairs       every pair, n²: the reference the others are checked against
//   SweepAndPrune  bodies sorted along one axis, each tested against those whose interval it overlaps; the order is kept
//                  from step to step, so the sort is nearly free when little has moved
//   Grid           a uniform grid, each body in the cells its box covers: what a GPU does (hash the cell, sort by it)
//   Tree           a dynamic AABB tree (DynamicTree): the cheapest when little moves or moves steadily, and good when
//                  bodies are of very different sizes, which a grid's one cell size suits badly
//
// Whichever is used, the pairs come out in the same order: by the lower body, then the higher. Two bodies nothing moves
// (inverse mass 0) are left out, and so are two neither of which is active (asleep, or static), and two a joint keeps
// apart.
//

#pragma once

#include <kphysics/math.h>

#include <cstdint>
#include <vector>

namespace kphysics
{
    class Simulation;

    /**
     * @brief A bounding volume hierarchy kept up to date as bodies move, after Box2D's b2DynamicTree. Each body is a
     *        leaf holding a "fat" box: its own box widened by Extension, and reaching Prediction steps ahead of the way
     *        it moves. Inner nodes hold the box around their children. A leaf follows its body only when the body's box
     *        leaves the fat one (or the fat one is far too big for it), and goes where it adds the least surface area,
     *        the tree rotated on the way up so it stays shallow. When many leaves must move at once, the whole tree is
     *        built again instead, top down, split at the median of the widest axis.
     */
    class DynamicTree {
    public:
        static constexpr int Null = -1;
        /** @brief How much bigger than its body's box a leaf's box is, in metres. */
        static constexpr float Extension = 0.1f;
        /** @brief How many steps' worth of movement a fat box reaches ahead (Box2D's aabbMultiplier). */
        static constexpr float Prediction = 4.f;

        explicit DynamicTree(int bodies);

        void Clear();
        /** @brief Body @p b's leaf, its box (lowers[b], uppers[b]) made fat. */
        void Insert(int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers);
        /**
         * @brief Notes where body @p b is and how far it moved since last asked (once a step for every body), and says
         *        whether its leaf must follow: its box has left the fat one, or the fat one is far bigger than it needs.
         */
        bool NeedsMove(int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers);
        /** @brief Body @p b's leaf taken out and put back with a fat box made from where it is now. */
        void Reinsert(int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers);
        /** @brief NeedsMove, then Reinsert if it does. Returns whether it did. */
        bool Move(int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers);
        /**
         * @brief The whole tree built again from @p count bodies, top down. @p fresh: they have not been seen before;
         *        otherwise each was seen by NeedsMove this step, and its fat box reaches ahead as a reinserted one would.
         */
        void Rebuild(const int* bodies, int count, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers, bool fresh);

        /** @brief Every body whose leaf's fat box overlaps the box (lower, upper), handed to @p found. */
        template <typename F> void Query(Vec3 lower, Vec3 upper, F&& found) const;
        /** @brief Every body whose leaf's fat box overlaps body @p b's own leaf's (itself included). */
        template <typename F> void QueryLeaf(const int b, F&& found) const { const Node& n = _nodes[_leaf[b]]; Query(n.lower, n.upper, found); }
        /** @brief Whether the fat boxes of bodies @p a's and @p b's leaves overlap. */
        [[nodiscard]] bool FatOverlap(int a, int b) const;

        /** @brief The tree's height: about log₂ n while it stays balanced. */
        [[nodiscard]] int Height() const { return _root == Null ? 0 : _nodes[_root].height; }
        /** @brief Whether every node is as it should be: links both ways, heights, each box around its children's. */
        [[nodiscard]] bool IsValid() const;
        /** @brief Leaves taken out and put back one by one since the tree was cleared: what moving costs. */
        [[nodiscard]] long Reinsertions() const { return _reinsertions; }
        /** @brief Times the whole tree was built again at once. */
        [[nodiscard]] long Rebuilds() const { return _rebuilds; }

    private:
        struct Node {
            Vec3 lower, upper;
            int parent = Null, child1 = Null, child2 = Null;
            int height = 0;         ///< 0 for a leaf, −1 for a free node.
            int body = -1;          ///< A leaf's.
            [[nodiscard]] bool IsLeaf() const { return child1 == Null; }
        };

        int NewLeaf(int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers);
        void Fatten(Node& node, int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers) const;
        void Seen(int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers);
        int Build(int from, int to);
        void Select(int left, int right, int k, int axis);
        void InsertLeaf(int leaf);
        void RemoveLeaf(int leaf);
        void RefitUpFrom(int start);
        int Balance(int a);
        int RotateUp(int a, int up, bool upIsSecond);
        void Fit(int node);
        [[nodiscard]] float Area(int node) const;
        [[nodiscard]] float CombinedArea(int a, int b) const;
        [[nodiscard]] float Descend(int child, int leaf) const;
        int Allocate();
        void Release(int node);

        std::vector<Node> _nodes;
        std::vector<int> _leaf;                 // each body's leaf, or Null
        std::vector<Vec3> _last, _ahead;        // each body's box centre when last asked; how far its fat box reaches
        std::vector<int> _leaves;               // a rebuild's leaves, split in place
        mutable std::vector<int> _stack;
        int _root = Null;
        int _free = Null;                       // the first free node; each free one's parent is the next
        int _used = 0;
        long _reinsertions = 0, _rebuilds = 0;
    };

    enum class BroadphaseKind { AllPairs, SweepAndPrune, Grid, Tree };

    class Broadphase {
    public:
        explicit Broadphase(int capacity);

        BroadphaseKind kind = BroadphaseKind::Tree;
        /** @brief Grid: the cell's edge; 0 works it out each step, as twice the bodies' average box. */
        float cellSize = 0.f;

        /** @brief Each body's box. A plane's is infinite. */
        std::vector<Vec3> lower, upper;
        /** @brief The pairs found, as PairKey(a, b), in order. */
        std::vector<std::uint64_t> pairs;

        /** @brief Every body's box, then the pairs whose boxes overlap. */
        void Update(const Simulation& sim);
        /** @brief Body @p b's box, widened by @p margin on every side. */
        void Bounds(const Simulation& sim, int b, float margin);
        [[nodiscard]] int PairCount() const { return static_cast<int>(pairs.size()); }
        [[nodiscard]] const DynamicTree& Tree() const { return _tree; }
        /** @brief Sweep and prune's axis: the one the boxes' centres spread along most. */
        [[nodiscard]] int Axis() const { return _axis; }

    private:
        [[nodiscard]] bool Overlap(int a, int b) const;
        [[nodiscard]] static bool Wanted(const Simulation& sim, int a, int b);
        void AllPairs(const Simulation& sim, int n);
        void SweepAndPrune(const Simulation& sim, int n);
        [[nodiscard]] int WidestAxis(int n) const;
        void Grid(const Simulation& sim, int n);
        void TreePairs(const Simulation& sim, int n);
        /** Planes, and what is too big for a grid's cells, against everything. */
        void Larges(const Simulation& sim, int n, int count);
        void MergeFresh();
        [[nodiscard]] float AverageBox(int n) const;
        void Unique();

        // Sweep and prune: the bodies in order along the axis, kept between steps.
        std::vector<int> _order, _open;
        int _ordered = 0;
        int _axis = 0;
        // Grid: (cell hash, body) entries; planes and the too-big, flagged.
        std::vector<std::uint64_t> _cells;
        std::vector<int> _large;
        std::vector<char> _isLarge;
        // Tree: kept between steps, with every pair whose leaves' fat boxes overlap (candidates), in order.
        DynamicTree _tree;
        int _treeBodies = -1;
        std::vector<std::uint64_t> _candidates, _fresh, _spare;
        std::vector<int> _moved;
    };

    template <typename F>
    void DynamicTree::Query(const Vec3 lower, const Vec3 upper, F&& found) const
    {
        if (_root == Null) return;
        int top = 0;
        _stack[top++] = _root;
        while (top > 0) {
            const Node& node = _nodes[_stack[--top]];
            if (lower.x > node.upper.x || upper.x < node.lower.x || lower.y > node.upper.y || upper.y < node.lower.y ||
                lower.z > node.upper.z || upper.z < node.lower.z) continue;
            if (node.IsLeaf()) found(node.body);
            else { _stack[top++] = node.child1; _stack[top++] = node.child2; }
        }
    }
}
