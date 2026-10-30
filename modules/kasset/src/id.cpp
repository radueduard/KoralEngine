#include <kasset/id.h>

#include <format>
#include <random>

namespace kasset
{
    Id Id::New()
    {
        thread_local std::mt19937_64 random = [] {
            std::random_device device;
            std::seed_seq seed { device(), device(), device(), device(), device(), device(), device(), device() };
            return std::mt19937_64(seed);
        }();
        Id id;
        do { id = { random(), random() }; } while (!id.Valid());
        return id;
    }

    Id Id::Named(const std::string_view name)
    {
        // Two FNV-1a hashes of the name, from different starts, over "koral/" before it so that no project's name can
        // land on the engine's by chance any more than two random IDs do.
        const auto hash = [name](std::uint64_t h) {
            for (const char c : std::string_view("koral/")) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
            for (const char c : name) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
            // Mixed once more, so that names differing in their last character differ in every digit.
            h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
            return h;
        };
        Id id { hash(0xcbf29ce484222325ull), hash(0x84222325cbf29ce4ull) };
        if (!id.Valid()) id.low = 1;
        return id;
    }

    std::optional<Id> Id::Parse(const std::string_view text)
    {
        if (text.size() != 32) return std::nullopt;
        Id id;
        for (std::size_t i = 0; i < 32; ++i) {
            const char c = text[i];
            std::uint64_t digit;
            if (c >= '0' && c <= '9') digit = static_cast<std::uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint64_t>(c - 'a' + 10);
            else return std::nullopt;
            std::uint64_t& half = i < 16 ? id.high : id.low;
            half = (half << 4) | digit;
        }
        if (!id.Valid()) return std::nullopt;
        return id;
    }

    std::string Id::Text() const { return std::format("{:016x}{:016x}", high, low); }

    std::optional<Reference> Reference::Parse(const std::string_view text)
    {
        const auto slash = text.find('/');
        const auto asset = Id::Parse(text.substr(0, slash));
        if (!asset) return std::nullopt;
        if (slash == std::string_view::npos) return Reference { *asset, {} };
        const auto part = Id::Parse(text.substr(slash + 1));
        if (!part) return std::nullopt;
        return Reference { *asset, *part };
    }

    std::string Reference::Text() const { return part ? std::format("{}/{}", asset.Text(), part.Text()) : asset.Text(); }
}
