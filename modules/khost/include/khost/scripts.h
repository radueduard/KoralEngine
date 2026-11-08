#pragma once

#include <filesystem>
#include <string>

#include <keditor/scripting.h>

namespace khost
{
    class Runtime;

    /**
     * @brief A project's scripts, run inside this program: .NET started in it for scripts in C#, a JVM for scripts
     *        in Kotlin — whichever the project's are in — and the project's scripts compiled and run on the scene
     *        that is open. No other program is started: `koral-editor` and `koral-play` open a project with its
     *        scripts, as they open one without.
     *
     * Each runtime is started the first time a project needs it, and is there until the program ends (neither can be
     * started twice in a process): another project, or the same one opened again, only has its scripts run on it.
     */
    class Scripts final : public keditor::ScriptRuntime {
    public:
        Scripts();
        ~Scripts() override;
        Scripts(const Scripts&) = delete;
        Scripts& operator=(const Scripts&) = delete;

        /**
         * @brief Between frames: the scripts of @p document's project run on it. When it is another document than
         *        the last — another scene, another project — those stop and its own start; and a saved script is
         *        compiled and loaded again. Null for none.
         */
        void Follow(keditor::Document* document);
        /** @brief Waits for the scripts being compiled, and loads them: for a run of so many frames, before they start. */
        void Settle();
        /**
         * @brief Of a project in Kotlin, as a program starts: the JVM started now, and Kotlin's compiler run once on its
         *        thread on what is nothing — while the program goes on to open its window and its scene — so that the
         *        project's scripts, when they are asked for, are compiled by a compiler that is warm. Nothing for any other.
         */
        void WarmUp(const std::filesystem::path& project);
        /** @brief The scripts stop, and let go of the document: before the program's scenes go. */
        void Stop();

        [[nodiscard]] bool Runs(const std::filesystem::path& project, std::string& why) const override;
        void Forget(keditor::Document* document) override;

    private:
        void Begin();

        keditor::Document* _document = nullptr;
        std::filesystem::path _project;     ///< The document's project, when it has one.
        Runtime* _running = nullptr;        ///< What runs its scripts, when anything does.
        char _language = 0;                 ///< The language they are in: 'c', 'k', or 0 for none yet.
        std::string _whyNot;                ///< Why they are not run, when the project has some and they are not.
        unsigned _sinceLooked = 0;          ///< Frames since the project was last looked at for a language, when it had none.
    };
}
