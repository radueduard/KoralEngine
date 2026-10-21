#include <kphysics/broadphase.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>

namespace kphysics
{
    DynamicTree::DynamicTree(const int bodies)
        : _nodes(static_cast<std::size_t>(2 * bodies + 1)), _leaf(bodies, Null), _last(bodies), _ahead(bodies), _leaves(bodies),
          _stack(static_cast<std::size_t>(2 * bodies + 2))
    {
    }

    void DynamicTree::Clear()
    {
        _root = Null;
        _free = Null;
        _used = 0;
        _reinsertions = 0;
        _rebuilds = 0;
        std::ranges::fill(_leaf, Null);
    }

    void DynamicTree::Insert(const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers)
    {
        Seen(b, lowers, uppers);
        InsertLeaf(NewLeaf(b, lowers, uppers));
    }

    bool DynamicTree::NeedsMove(const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers)
    {
        const Vec3 centre = (lowers[b] + uppers[b]) * 0.5f;
        _ahead[b] = (centre - _last[b]) * Prediction;
        _last[b] = centre;
        if (_leaf[b] == Null) return true;
        const Node& node = _nodes[_leaf[b]];
        for (int c = 0; c < 3; ++c) {
            if (lowers[b][c] < node.lower[c] || uppers[b][c] > node.upper[c]) return true;
            // Box2D's "huge" box: the fat box it would get now, widened by 4 × Extension more, and by the reach ahead on
            // both sides (else the edge a moving body leaves behind trips it every few steps). Past that, put back.
            const float a = std::abs(_ahead[b][c]);
            if (node.lower[c] < lowers[b][c] - 5.f * Extension - a || node.upper[c] > uppers[b][c] + 5.f * Extension + a) return true;
        }
        return false;
    }

    void DynamicTree::Reinsert(const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers)
    {
        const int leaf = _leaf[b];
        if (leaf == Null) { InsertLeaf(NewLeaf(b, lowers, uppers)); return; }
        RemoveLeaf(leaf);
        Fatten(_nodes[leaf], b, lowers, uppers);
        InsertLeaf(leaf);
        ++_reinsertions;
    }

    bool DynamicTree::Move(const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers)
    {
        if (!NeedsMove(b, lowers, uppers)) return false;
        Reinsert(b, lowers, uppers);
        return true;
    }

    void DynamicTree::Rebuild(const int* bodies, const int count, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers,
                              const bool fresh)
    {
        _root = Null;
        _free = Null;
        _used = 0;
        std::ranges::fill(_leaf, Null);
        for (int i = 0; i < count; ++i) {
            if (fresh) Seen(bodies[i], lowers, uppers);
            _leaves[i] = NewLeaf(bodies[i], lowers, uppers);
        }
        if (count > 0) {
            _root = Build(0, count);
            _nodes[_root].parent = Null;
        }
        if (!fresh) ++_rebuilds;
    }

    bool DynamicTree::FatOverlap(const int a, const int b) const
    {
        const int x = _leaf[a], y = _leaf[b];
        if (x == Null || y == Null) return false;
        const Node& p = _nodes[x];
        const Node& q = _nodes[y];
        for (int c = 0; c < 3; ++c)
            if (p.lower[c] > q.upper[c] || q.lower[c] > p.upper[c]) return false;
        return true;
    }

    bool DynamicTree::IsValid() const
    {
        if (_root == Null) return true;
        if (_nodes[_root].parent != Null) return false;
        int top = 0;
        _stack[top++] = _root;
        while (top > 0) {
            const int index = _stack[--top];
            const Node& node = _nodes[index];
            if (node.IsLeaf()) {
                if (node.child2 != Null || node.height != 0 || _leaf[node.body] != index) return false;
                continue;
            }
            const Node& a = _nodes[node.child1];
            const Node& b = _nodes[node.child2];
            if (a.parent != index || b.parent != index) return false;
            if (node.height != 1 + std::max(a.height, b.height)) return false;
            for (int c = 0; c < 3; ++c) {
                if (node.lower[c] > std::min(a.lower[c], b.lower[c])) return false;
                if (node.upper[c] < std::max(a.upper[c], b.upper[c])) return false;
            }
            _stack[top++] = node.child1;
            _stack[top++] = node.child2;
        }
        return true;
    }

    void DynamicTree::Seen(const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers)
    {
        _last[b] = (lowers[b] + uppers[b]) * 0.5f;
        _ahead[b] = {};
    }

    int DynamicTree::NewLeaf(const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers)
    {
        const int leaf = Allocate();
        Node& node = _nodes[leaf];
        Fatten(node, b, lowers, uppers);
        node.body = b;
        node.height = 0;
        node.child1 = Null;
        node.child2 = Null;
        _leaf[b] = leaf;
        return leaf;
    }

    void DynamicTree::Fatten(Node& node, const int b, const std::vector<Vec3>& lowers, const std::vector<Vec3>& uppers) const
    {
        for (int c = 0; c < 3; ++c) {
            const float a = _ahead[b][c];
            node.lower[c] = lowers[b][c] - Extension + std::min(a, 0.f);
            node.upper[c] = uppers[b][c] + Extension + std::max(a, 0.f);
        }
    }

    // ---- building at once ---------------------------------------------------------------------------------------------

    /** A node over _leaves[from, to): split at the median of their centres along the axis those spread widest on. */
    int DynamicTree::Build(const int from, const int to)
    {
        if (to - from == 1) return _leaves[from];
        int axis = 0;
        float widest = -1.f;
        for (int c = 0; c < 3; ++c) {
            float low = FLT_MAX, high = -FLT_MAX;
            for (int i = from; i < to; ++i) {
                const float m = _nodes[_leaves[i]].lower[c] + _nodes[_leaves[i]].upper[c];
                low = std::min(low, m);
                high = std::max(high, m);
            }
            if (high - low > widest) { widest = high - low; axis = c; }
        }
        const int middle = (from + to) / 2;
        Select(from, to - 1, middle, axis);
        const int index = Allocate();
        _nodes[index].body = -1;
        const int a = Build(from, middle);
        const int b = Build(middle, to);
        _nodes[index].child1 = a;
        _nodes[index].child2 = b;
        _nodes[a].parent = index;
        _nodes[b].parent = index;
        Fit(index);
        return index;
    }

    /** _leaves[left..right] rearranged so that _leaves[k] is where sorting by centre would put it, smaller ones before. */
    void DynamicTree::Select(int left, int right, const int k, const int axis)
    {
        const auto centre = [&](const int i) { return _nodes[_leaves[i]].lower[axis] + _nodes[_leaves[i]].upper[axis]; };
        while (left < right) {
            const float pivot = centre((left + right) / 2);
            int i = left, j = right;
            while (i <= j) {
                while (centre(i) < pivot) ++i;
                while (centre(j) > pivot) --j;
                if (i <= j) { std::swap(_leaves[i], _leaves[j]); ++i; --j; }
            }
            if (k <= j) right = j;
            else if (k >= i) left = i;
            else return;
        }
    }

    // ---- inserting and removing ---------------------------------------------------------------------------------------

    void DynamicTree::InsertLeaf(const int leaf)
    {
        if (_root == Null) { _root = leaf; _nodes[leaf].parent = Null; return; }
        // Down to the best sibling: at each node, the cost of a new parent here against the least it could cost to go
        // down either side (the area this node's box grows by is paid whichever way).
        int index = _root;
        while (!_nodes[index].IsLeaf()) {
            const float area = Area(index);
            const float combined = CombinedArea(index, leaf);
            const float here = 2.f * combined;
            const float inherited = 2.f * (combined - area);
            const float cost1 = Descend(_nodes[index].child1, leaf) + inherited;
            const float cost2 = Descend(_nodes[index].child2, leaf) + inherited;
            if (here < cost1 && here < cost2) break;
            index = cost1 < cost2 ? _nodes[index].child1 : _nodes[index].child2;
        }
        const int sibling = index;
        const int oldParent = _nodes[sibling].parent;
        const int newParent = Allocate();
        Node& parent = _nodes[newParent];
        parent.parent = oldParent;
        parent.body = -1;
        for (int c = 0; c < 3; ++c) {
            parent.lower[c] = std::min(_nodes[sibling].lower[c], _nodes[leaf].lower[c]);
            parent.upper[c] = std::max(_nodes[sibling].upper[c], _nodes[leaf].upper[c]);
        }
        parent.height = _nodes[sibling].height + 1;
        if (oldParent != Null) {
            if (_nodes[oldParent].child1 == sibling) _nodes[oldParent].child1 = newParent;
            else _nodes[oldParent].child2 = newParent;
        } else {
            _root = newParent;
        }
        parent.child1 = sibling;
        parent.child2 = leaf;
        _nodes[sibling].parent = newParent;
        _nodes[leaf].parent = newParent;
        RefitUpFrom(newParent);
    }

    void DynamicTree::RemoveLeaf(const int leaf)
    {
        if (leaf == _root) { _root = Null; return; }
        const int parent = _nodes[leaf].parent;
        const int grand = _nodes[parent].parent;
        const int sibling = _nodes[parent].child1 == leaf ? _nodes[parent].child2 : _nodes[parent].child1;
        if (grand != Null) {
            // The sibling takes the parent's place, and the parent goes.
            if (_nodes[grand].child1 == parent) _nodes[grand].child1 = sibling;
            else _nodes[grand].child2 = sibling;
            _nodes[sibling].parent = grand;
            Release(parent);
            RefitUpFrom(grand);
        } else {
            _root = sibling;
            _nodes[sibling].parent = Null;
            Release(parent);
        }
    }

    /** From @p start up to the root: each node balanced, then its box and height from its children again. */
    void DynamicTree::RefitUpFrom(const int start)
    {
        int index = start;
        while (index != Null) {
            index = Balance(index);
            Fit(index);
            index = _nodes[index].parent;
        }
    }

    /** If one of a's children is two levels deeper than the other, that child is turned up into a's place (Box2D's rotation). */
    int DynamicTree::Balance(const int a)
    {
        if (_nodes[a].IsLeaf() || _nodes[a].height < 2) return a;
        const int b = _nodes[a].child1, c = _nodes[a].child2;
        const int difference = _nodes[c].height - _nodes[b].height;
        if (difference > 1) return RotateUp(a, c, true);
        if (difference < -1) return RotateUp(a, b, false);
        return a;
    }

    /** Turns @p up (a's deeper child, its second when @p upIsSecond) into a's place; the deeper of up's children stays with it. */
    int DynamicTree::RotateUp(const int a, const int up, const bool upIsSecond)
    {
        const int f = _nodes[up].child1, g = _nodes[up].child2;
        _nodes[up].child1 = a;
        _nodes[up].parent = _nodes[a].parent;
        _nodes[a].parent = up;
        const int p = _nodes[up].parent;
        if (p != Null) {
            if (_nodes[p].child1 == a) _nodes[p].child1 = up;
            else _nodes[p].child2 = up;
        } else {
            _root = up;
        }
        const int keep = _nodes[f].height > _nodes[g].height ? f : g;
        const int give = keep == f ? g : f;
        _nodes[up].child2 = keep;
        if (upIsSecond) _nodes[a].child2 = give;
        else _nodes[a].child1 = give;
        _nodes[give].parent = a;
        Fit(a);
        Fit(up);
        return up;
    }

    void DynamicTree::Fit(const int index)
    {
        Node& node = _nodes[index];
        const Node& a = _nodes[node.child1];
        const Node& b = _nodes[node.child2];
        node.height = 1 + std::max(a.height, b.height);
        for (int c = 0; c < 3; ++c) {
            node.lower[c] = std::min(a.lower[c], b.lower[c]);
            node.upper[c] = std::max(a.upper[c], b.upper[c]);
        }
    }

    float DynamicTree::Area(const int index) const
    {
        const Vec3 e = _nodes[index].upper - _nodes[index].lower;
        return 2.f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }

    float DynamicTree::CombinedArea(const int a, const int b) const
    {
        const Node& p = _nodes[a];
        const Node& q = _nodes[b];
        const float x = std::max(p.upper.x, q.upper.x) - std::min(p.lower.x, q.lower.x);
        const float y = std::max(p.upper.y, q.upper.y) - std::min(p.lower.y, q.lower.y);
        const float z = std::max(p.upper.z, q.upper.z) - std::min(p.lower.z, q.lower.z);
        return 2.f * (x * y + y * z + z * x);
    }

    /** What going down into @p child would cost at least: a leaf, the new box's area; a node, what its box would grow by. */
    float DynamicTree::Descend(const int child, const int leaf) const
    {
        return _nodes[child].IsLeaf() ? CombinedArea(child, leaf) : CombinedArea(child, leaf) - Area(child);
    }

    int DynamicTree::Allocate()
    {
        if (_free != Null) {
            const int node = _free;
            _free = _nodes[node].parent;
            _nodes[node].parent = Null;
            return node;
        }
        if (_used >= static_cast<int>(_nodes.size())) throw std::length_error("kphysics::DynamicTree: no room");
        const int node = _used++;
        _nodes[node].parent = Null;
        return node;
    }

    void DynamicTree::Release(const int node)
    {
        _nodes[node].parent = _free;
        _nodes[node].height = -1;
        _free = node;
    }
}
