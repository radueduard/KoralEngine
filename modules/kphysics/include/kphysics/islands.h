//
// kphysics: islands and sleep, and colouring.
//
// An island is a group of dynamic bodies that touch or are joined, one to the next; static bodies join nothing. An
// island whose bodies have all been nearly still for timeToSleep goes to sleep: not integrated, its contacts not even
// looked for, until something wakes it. Waking needs nothing special: a moving body that touches a sleeping one makes a
// contact, the contact puts them in one island, and an island with a body not still long enough is awake, all of it.
// Islands are found each step by union–find, the lower root winning so the islands are the same every run.
//
// Colouring groups the manifolds so that no two in a colour share a dynamic body: a GPU solves a colour per dispatch,
// all of it in parallel, and the colours one after another. Greedy, a bit per colour per body; a body in more than 64
// manifolds sends the rest to an overflow colour, solved one after another.
//

#pragma once

#include <cstdint>
#include <vector>

namespace kphysics
{
    class Simulation;

    class Islands {
    public:
        explicit Islands(int capacity) : _parent(capacity), _still(capacity), _awake(capacity) {}

        bool allowSleep = true;
        float linearTolerance = 0.05f;          ///< Below these a body counts as still: m/s,
        float angularTolerance = 0.05f;         ///< and rad/s.
        float timeToSleep = 0.5f;               ///< How long an island must be still before it sleeps, in seconds.

        void Update(Simulation& sim, float dt);
        /** @brief Body @p b's island: its root, the path to it shortened on the way. */
        int Find(int b);

    private:
        void Join(const Simulation& sim, int a, int b);

        std::vector<int> _parent;
        std::vector<float> _still;              // per root: the shortest stillness of its bodies
        std::vector<char> _awake;               // per root: whether any of its bodies is awake
    };

    class Colouring {
    public:
        static constexpr int Overflow = 64;

        std::vector<int> colour;                ///< Each manifold's, 0 to ColourCount − 1.
        std::vector<int> order;                 ///< The manifolds by colour, then in their own order.
        std::vector<int> start = std::vector<int>(Overflow + 2);   ///< Colour k is order[start[k], start[k + 1]).

        void Colour(const Simulation& sim);
        [[nodiscard]] int ColourCount() const { return _colourCount; }
        /** @brief Whether no two manifolds of one colour (overflow aside) share a dynamic body. */
        [[nodiscard]] bool IsValid(const Simulation& sim) const;

    private:
        std::vector<std::uint64_t> _used;       // per body: a bit for each colour it is in already
        std::vector<int> _next = std::vector<int>(Overflow + 2);
        int _colourCount = 0;
    };
}
