#include <kphysics/recording.h>
#include <kphysics/simulation.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace kphysics
{
    static_assert(std::endian::native == std::endian::little, "a .kprr file is little-endian, as is every machine the engine runs on");

    Recording::Recording(const int bodyCount, const int stepCount)
        : frames(static_cast<std::size_t>(bodyCount) * stepCount * 7), _bodyCount(bodyCount), _stepCount(stepCount)
    {
    }

    Recording& Recording::Record(Simulation& sim, const float dt)
    {
        if (sim.bodies.Count() != _bodyCount) throw std::invalid_argument("kphysics::Recording: the simulation has another number of bodies");
        for (int s = 0; s < _stepCount; ++s) {
            sim.Step(dt);
            for (int b = 0; b < _bodyCount; ++b) {
                float* frame = &frames[(static_cast<std::size_t>(s) * _bodyCount + b) * 7];
                const Vec3 p = sim.bodies.position[b];
                const Quat q = sim.bodies.orientation[b];
                frame[0] = p.x; frame[1] = p.y; frame[2] = p.z;
                frame[3] = q.x; frame[4] = q.y; frame[5] = q.z; frame[6] = q.w;
            }
        }
        return *this;
    }

    float Recording::LargestPositionDifference(const Recording& other) const
    {
        if (other._bodyCount != _bodyCount || other._stepCount != _stepCount) throw std::invalid_argument("kphysics::Recording: recordings of different runs");
        float largest = 0.f;
        for (std::size_t frame = 0; frame < frames.size(); frame += 7)
            for (int c = 0; c < 3; ++c) largest = std::max(largest, std::abs(frames[frame + c] - other.frames[frame + c]));
        return largest;
    }

    int Recording::FirstStepApart(const Recording& other, const float tolerance) const
    {
        for (int s = 0; s < _stepCount; ++s)
            for (int b = 0; b < _bodyCount; ++b) {
                const std::size_t frame = (static_cast<std::size_t>(s) * _bodyCount + b) * 7;
                for (int c = 0; c < 3; ++c)
                    if (std::abs(frames[frame + c] - other.frames[frame + c]) > tolerance) return s;
            }
        return -1;
    }

    bool Recording::Write(const std::filesystem::path& file) const
    {
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        const std::int32_t header[4] { Magic, 1, _bodyCount, _stepCount };
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        out.write(reinterpret_cast<const char*>(frames.data()), static_cast<std::streamsize>(frames.size() * sizeof(float)));
        return static_cast<bool>(out);
    }

    bool Recording::Read(const std::filesystem::path& file, Recording& out, std::string& error)
    {
        std::ifstream in(file, std::ios::binary);
        std::int32_t header[4] {};
        if (!in.read(reinterpret_cast<char*>(header), sizeof header) || header[0] != Magic) {
            error = file.filename().string() + " is no reference run";
            return false;
        }
        if (header[1] != 1) {
            error = file.filename().string() + ": a version this does not read";
            return false;
        }
        Recording read(header[2], header[3]);
        if (!in.read(reinterpret_cast<char*>(read.frames.data()), static_cast<std::streamsize>(read.frames.size() * sizeof(float)))) {
            error = file.filename().string() + " ends early";
            return false;
        }
        out = std::move(read);
        return true;
    }
}
