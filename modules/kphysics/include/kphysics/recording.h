//
// kphysics: reference runs, every body's position and orientation, every step. What a GPU version is checked against,
// and what this port was checked against the Kotlin core with. The file (.kprr) is plain: four little-endian ints (a
// magic number, the version, the body count, the step count), then for each step, for each body, x y z then qx qy qz qw
// as little-endian floats.
//

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kphysics
{
    class Simulation;

    class Recording {
    public:
        static constexpr int Magic = 0x5252504B;    ///< "KPRR": Koral physics reference run.

        Recording(int bodyCount, int stepCount);

        [[nodiscard]] int BodyCount() const { return _bodyCount; }
        [[nodiscard]] int StepCount() const { return _stepCount; }
        /** @brief Step s, body b: frames[(s · bodyCount + b) · 7 ..], x y z qx qy qz qw. */
        std::vector<float> frames;

        /** @brief Steps @p sim StepCount times by @p dt, keeping each step's state. */
        Recording& Record(Simulation& sim, float dt);
        /** @brief The largest difference in position over every body and step: 0 for the same run to the bit. */
        [[nodiscard]] float LargestPositionDifference(const Recording& other) const;
        /** @brief The first step at which some body is more than @p tolerance from where @p other has it, or −1. */
        [[nodiscard]] int FirstStepApart(const Recording& other, float tolerance) const;

        bool Write(const std::filesystem::path& file) const;
        /** @brief The run in @p file; false, and why in @p error, when it is none. */
        static bool Read(const std::filesystem::path& file, Recording& out, std::string& error);

    private:
        int _bodyCount = 0, _stepCount = 0;
    };
}
