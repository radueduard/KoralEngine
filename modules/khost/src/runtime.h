#pragma once

#include <filesystem>
#include <string>

namespace khost
{
    /** @brief .NET or a JVM, started in this process: what is attached to a document runs its project's scripts. */
    class Runtime {
    public:
        virtual ~Runtime() = default;
        /** @brief @p project's scripts run on @p document from now. False, with @p error, when they cannot be run at all. */
        virtual bool Attach(void* document, const std::filesystem::path& project, std::string& error) = 0;
        /** @brief Between frames: a saved script compiled and loaded again, a build finished loaded. */
        virtual void Poll() = 0;
        /** @brief Waits for a build under way, and loads it. */
        virtual void Settle() {}
        /** @brief Readies what compiles the scripts, on a thread of its own, before any are asked for: what a program does as it opens a project. */
        virtual void WarmUp() {}
        /** @brief The scripts stop, and let go of the document. */
        virtual void Detach() = 0;
    };

    /** @brief .NET, started the first time it is asked for: null, with why in @p error, when it cannot be. */
    Runtime* Dotnet(std::string& error);
    /** @brief A JVM, started the first time it is asked for: null, with why in @p error, when it cannot be. */
    Runtime* Jvm(std::string& error);

    namespace detail
    {
        /** @brief The folder this program is in. */
        std::filesystem::path ProgramDirectory();
        /** @brief Where the Koral library this program loaded is: what the managed side is to load too, the same one. */
        std::filesystem::path KoralLibrary();
        /** @brief Says to whatever is started after, in this process: @p name is @p value — unless it is said already. */
        void SetVariable(const char* name, const std::filesystem::path& value);
        /** @brief An environment variable as a path: empty when it is not set. */
        std::filesystem::path Variable(const char* name);
        /** @brief Loads the native library @p path, its own folder searched for what it needs: null when it cannot be. */
        void* LoadLibraryAt(const std::filesystem::path& path, std::string& error);
        /** @brief A function of a library LoadLibraryAt loaded. */
        void* Function(void* library, const char* name);
    }
}

/** A symbol of this program's own, by its name: how the managed side finds the engine's C interface in it. */
extern "C" void* khost_symbol(const char* name);
