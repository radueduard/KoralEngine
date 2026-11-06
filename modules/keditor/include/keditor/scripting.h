#pragma once

#include <filesystem>
#include <string>

namespace keditor
{
    class Document;

    /**
     * @brief What runs a project's scripts inside the program the editor or the player is in: `koral-editor` and
     *        `koral-play` have one (khost::Scripts), which starts .NET or a JVM in their own process. There is none
     *        in the engine's library (koral-engine), whose scenes are run by whatever program loaded it.
     */
    class ScriptRuntime {
    public:
        virtual ~ScriptRuntime() = default;
        /** @brief Whether @p project's scripts are run here now. When not, why, in @p why. */
        [[nodiscard]] virtual bool Runs(const std::filesystem::path& project, std::string& why) const = 0;
        /** @brief @p document is going: what runs scripts on it lets go of it, now, while it is still whole. */
        virtual void Forget(Document* document) = 0;
    };

    /** @brief This program's, when it has one: set by its `main`. */
    inline ScriptRuntime* scriptRuntime = nullptr;
}
