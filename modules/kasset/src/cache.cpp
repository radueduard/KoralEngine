#include <kasset/cache.h>

#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace kasset
{
    namespace {
        // XXH64, as its reference has it.
        constexpr std::uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                                P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;

        std::uint64_t Word(const std::byte* at) { std::uint64_t w; std::memcpy(&w, at, 8); return w; }   // little-endian, as every target is
        std::uint32_t Half(const std::byte* at) { std::uint32_t w; std::memcpy(&w, at, 4); return w; }
        std::uint64_t Round(std::uint64_t lane, const std::uint64_t input) { lane += input * P2; lane = std::rotl(lane, 31); return lane * P1; }
        std::uint64_t Merge(std::uint64_t hash, const std::uint64_t lane) { hash ^= Round(0, lane); return hash * P1 + P4; }
    }

    std::uint64_t Hash(const std::span<const std::byte> bytes, const std::uint64_t seed)
    {
        const std::byte* at = bytes.data();
        const std::byte* const end = at + bytes.size();
        std::uint64_t hash;
        if (bytes.size() >= 32) {
            std::uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
            for (; at + 32 <= end; at += 32) {
                v1 = Round(v1, Word(at)); v2 = Round(v2, Word(at + 8)); v3 = Round(v3, Word(at + 16)); v4 = Round(v4, Word(at + 24));
            }
            hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
            hash = Merge(hash, v1); hash = Merge(hash, v2); hash = Merge(hash, v3); hash = Merge(hash, v4);
        } else {
            hash = seed + P5;
        }
        hash += bytes.size();
        for (; at + 8 <= end; at += 8) hash = std::rotl(hash ^ Round(0, Word(at)), 27) * P1 + P4;
        if (at + 4 <= end) { hash = std::rotl(hash ^ (static_cast<std::uint64_t>(Half(at)) * P1), 23) * P2 + P3; at += 4; }
        for (; at < end; ++at) hash = std::rotl(hash ^ (static_cast<std::uint64_t>(*at) * P5), 11) * P1;
        hash ^= hash >> 33; hash *= P2; hash ^= hash >> 29; hash *= P3; hash ^= hash >> 32;
        return hash;
    }

    std::uint64_t Hash(const std::string_view text, const std::uint64_t seed) { return Hash(std::as_bytes(std::span(text)), seed); }

    std::optional<std::uint64_t> HashFile(const std::filesystem::path& file)
    {
        std::error_code failed;
        const auto size = std::filesystem::file_size(file, failed);
        if (failed) return std::nullopt;
        const auto time = std::filesystem::last_write_time(file, failed).time_since_epoch().count();
        if (failed) return std::nullopt;
        // What was worked out, by the file as it was then: its size and time changed, it is worked out again.
        static std::mutex lock;
        static std::map<std::tuple<std::string, std::uintmax_t, long long>, std::uint64_t> known;
        const auto stamp = std::tuple(std::filesystem::absolute(file, failed).lexically_normal().generic_string(), size, static_cast<long long>(time));
        {
            const std::scoped_lock held(lock);
            if (const auto at = known.find(stamp); at != known.end()) return at->second;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) return std::nullopt;
        // A chunk at a time, each hashed from the hash before it: the same content is the same hash, whatever its size.
        std::uint64_t hash = size;
        std::vector<std::byte> chunk(std::size_t(1) << 20);
        while (in) {
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            const auto read = static_cast<std::size_t>(in.gcount());
            if (read == 0) break;
            hash = Hash(std::span(chunk.data(), read), hash);
        }
        if (in.bad()) return std::nullopt;
        const std::scoped_lock held(lock);
        known[stamp] = hash;
        return hash;
    }

    Key::Key(const std::string_view producer) : _a(Hash(producer, 0x6b6f72616c)), _b(Hash(producer, 0x6173736574)) {}

    Key& Key::Add(const std::string_view text)
    {
        // Each part's length first, so that "ab" + "c" is not "a" + "bc".
        Add(static_cast<std::uint64_t>(text.size()));
        _a = Hash(text, _a);
        _b = Hash(text, _b ^ 0x9e3779b97f4a7c15ull);
        return *this;
    }

    Key& Key::Add(const std::uint64_t number)
    {
        const auto bytes = std::as_bytes(std::span(&number, 1));
        _a = Hash(bytes, _a);
        _b = Hash(bytes, _b ^ 0x9e3779b97f4a7c15ull);
        return *this;
    }

    bool Key::AddFile(const std::filesystem::path& file)
    {
        const auto hash = HashFile(file);
        if (!hash) { _valid = false; return false; }
        Add(*hash);
        return true;
    }

    std::string Key::Text() const { return std::format("{:016x}{:016x}", _a, _b); }

    std::filesystem::path Cache::PathOf(const std::string_view kind, const Key& key, const std::string_view extension) const
    {
        return _directory / kind / std::format("{}{}", key.Text(), extension);
    }

    std::optional<std::vector<std::byte>> Cache::Read(const std::string_view kind, const Key& key, const std::string_view extension) const
    {
        if (!key.Valid()) return std::nullopt;
        std::ifstream in(PathOf(kind, key, extension), std::ios::binary | std::ios::ate);
        if (!in) return std::nullopt;
        const auto size = static_cast<std::size_t>(in.tellg());
        std::vector<std::byte> bytes(size);
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return std::nullopt;
        return bytes;
    }

    std::expected<std::filesystem::path, std::string> Cache::Write(const std::string_view kind, const Key& key, const std::string_view extension, const std::span<const std::byte> bytes) const
    {
        if (!key.Valid()) return std::unexpected("what it is made of could not all be read");
        const std::filesystem::path path = PathOf(kind, key, extension);
        std::error_code failed;
        std::filesystem::create_directories(path.parent_path(), failed);
        if (failed) return std::unexpected(std::format("{} could not be made: {}", path.parent_path().string(), failed.message()));
        // Written beside under a name of this thread's, so two making the same at once do not write into one file.
        std::filesystem::path writing = path;
        writing += std::format(".{}.writing", std::hash<std::thread::id> {}(std::this_thread::get_id()));
        {
            std::ofstream out(writing, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out.flush()) { out.close(); std::filesystem::remove(writing, failed); return std::unexpected(std::format("{} could not be written", path.string())); }
        }
        std::filesystem::rename(writing, path, failed);
        if (failed) { std::filesystem::remove(writing, failed); return std::unexpected(std::format("{} could not be written: {}", path.string(), failed.message())); }
        return path;
    }

    std::filesystem::path CacheDirectoryOf(const std::filesystem::path& project) { return project / ".koral" / "cache"; }
}
