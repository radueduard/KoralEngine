//
// keditor: which editor may change a scene. A project can be open in several editors at once — a client's, another
// client's, the server's — and each scene is changed and saved by one of them only: the first to open it. The others
// have it to look at and to play, and see it as it is saved.
//

#pragma once

#include <filesystem>
#include <string>

namespace keditor
{
    /**
     * @brief The right to change one scene, held while it is open: a lock the operating system keeps on a file of the
     *        project's (`.koral/cache/locks/<scene>.lock`), let go of when this is — or when the program ends however it
     *        ends, so that an editor that crashed holds nothing. Not copyable; moved, the right goes with it.
     */
    class SceneLock {
    public:
        SceneLock() = default;
        ~SceneLock() { Release(); }
        SceneLock(const SceneLock&) = delete;
        SceneLock& operator=(const SceneLock&) = delete;

        /**
         * @brief Takes the right to change the scene in @p scene, letting go of any held before. True when it is this
         *        program's; false when another holds it — Holder() says which — or the lock could not be made (Holder()
         *        empty: then there is no one else either).
         */
        bool Take(const std::filesystem::path& scene);
        /** @brief Lets go of it: another editor may take it now. */
        void Release();
        /** @brief Whether this program holds it. */
        [[nodiscard]] bool Held() const { return _handle != Nothing; }
        /** @brief Who holds it, when this program does not: what that one wrote of itself, `koral-editor 1234 on host`. */
        [[nodiscard]] const std::string& Holder() const { return _holder; }
        /** @brief The file it is of, held or not. */
        [[nodiscard]] const std::filesystem::path& Scene() const { return _scene; }

        /** @brief Where a scene's lock is: in its project's `.koral/cache/locks`, by the scene's name. */
        [[nodiscard]] static std::filesystem::path FileOf(const std::filesystem::path& scene);

    private:
#ifdef _WIN32
        using Handle = void*;
        static inline Handle const Nothing = reinterpret_cast<void*>(static_cast<long long>(-1));
#else
        using Handle = int;
        static constexpr Handle Nothing = -1;
#endif
        Handle _handle = Nothing;
        std::filesystem::path _scene;
        std::string _holder;
    };
}
