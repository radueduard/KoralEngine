#pragma once

/**
 * @file cache.h
 * @brief Where what producers made is kept, by a key of everything it was made of: a file's content (not its time,
 *        which a checkout or a copy changes), the parameters, and the producer's version. A key changed is another
 *        file, so nothing is ever made again that need not be, and nothing stale is ever read. A cache can be deleted
 *        whenever: it is made again.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kasset
{
    /** @brief 64 bits of @p bytes (XXH64), from @p seed. */
    [[nodiscard]] std::uint64_t Hash(std::span<const std::byte> bytes, std::uint64_t seed = 0);
    [[nodiscard]] std::uint64_t Hash(std::string_view text, std::uint64_t seed = 0);
    /**
     * @brief The hash of @p file 's content: worked out once for each size and time it has, in this process, so a file
     *        looked at again is not read again. Nothing for a file that cannot be read. Safe from any thread.
     */
    [[nodiscard]] std::optional<std::uint64_t> HashFile(const std::filesystem::path& file);

    /** @brief What an artifact is kept by: everything it was made of, put together. */
    class Key {
    public:
        /** @brief Of what makes it, by name and version: `model@1`. A producer's version goes up whenever what it makes changes. */
        explicit Key(std::string_view producer);
        Key& Add(std::string_view text);
        Key& Add(std::uint64_t number);
        /** @brief @p file 's content, by HashFile. False, and the key is no key (Valid), when it cannot be read. */
        bool AddFile(const std::filesystem::path& file);

        [[nodiscard]] bool Valid() const { return _valid; }
        /** @brief 32 hex digits: what its file is named. */
        [[nodiscard]] std::string Text() const;

    private:
        std::uint64_t _a, _b;
        bool _valid = true;
    };

    /** @brief A folder of artifacts, a folder of each kind: `<cache>/models/<key>.kmodel`. */
    class Cache {
    public:
        explicit Cache(std::filesystem::path directory) : _directory(std::move(directory)) {}

        [[nodiscard]] const std::filesystem::path& Directory() const { return _directory; }
        /** @brief Where an artifact of @p kind and @p key is, whether it is there or not. */
        [[nodiscard]] std::filesystem::path PathOf(std::string_view kind, const Key& key, std::string_view extension) const;
        /** @brief What was kept, whole; nothing when it is not there. */
        [[nodiscard]] std::optional<std::vector<std::byte>> Read(std::string_view kind, const Key& key, std::string_view extension) const;
        /** @brief Keeps @p bytes, whole or not at all: written beside, then put in place, so a reader never sees half. */
        [[nodiscard]] std::expected<std::filesystem::path, std::string> Write(std::string_view kind, const Key& key, std::string_view extension, std::span<const std::byte> bytes) const;

    private:
        std::filesystem::path _directory;
    };

    /** @brief Where a project keeps what was made of its assets: `.koral/cache` in its folder. */
    [[nodiscard]] std::filesystem::path CacheDirectoryOf(const std::filesystem::path& project);
}
