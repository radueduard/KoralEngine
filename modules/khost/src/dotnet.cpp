// .NET in this process, through hostfxr: started as the app Koral.Engine.Host is — its runtimeconfig.json says which
// runtime, its deps.json what is trusted (Koral.Engine, Roslyn, Koral's assemblies) — though nothing of it is run but
// Koral.Engine.Embedded's functions, which this calls.
//
// What is used of hostfxr is declared here, as hostfxr.h and coreclr_delegates.h have it, rather than taken from a
// .NET SDK's packs: a machine with only the runtime runs scripts as well.

#include "runtime.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <format>
#include <optional>
#include <vector>

namespace khost
{
    namespace {
#ifdef _WIN32
        using char_t = wchar_t;
        std::wstring Native(const std::filesystem::path& path) { return path.wstring(); }
        constexpr const char_t* Fxr = L"hostfxr.dll";
#define KHOST_TEXT(text) L##text
#else
        using char_t = char;
        std::string Native(const std::filesystem::path& path) { return path.string(); }
#  ifdef __APPLE__
        constexpr const char_t* Fxr = "libhostfxr.dylib";
#  else
        constexpr const char_t* Fxr = "libhostfxr.so";
#  endif
#define KHOST_TEXT(text) text
#endif

        struct InitializeParameters {
            std::size_t size;
            const char_t* hostPath;
            const char_t* dotnetRoot;
        };
        using InitializeForCommandLine = std::int32_t (*)(int argc, const char_t** argv, const InitializeParameters* parameters, void** context);
        using GetRuntimeDelegate = std::int32_t (*)(void* context, int type, void** delegate);
        using Close = std::int32_t (*)(void* context);
        using ErrorWriter = void (*)(const char_t* message);
        using SetErrorWriter = ErrorWriter (*)(ErrorWriter writer);
        using GetFunctionPointer = int (*)(const char_t* typeName, const char_t* methodName, const char_t* delegateTypeName,
                                           void* loadContext, void* reserved, void** delegate);
        constexpr int GetFunctionPointerDelegate = 6;       // hdt_get_function_pointer
        const char_t* const UnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

        /** What hostfxr said went wrong, kept for the error that follows. */
        std::string& Said()
        {
            static std::string said;
            return said;
        }
        void Heard(const char_t* message)
        {
            std::string& said = Said();
            if (!said.empty()) said += "; ";
            said += std::filesystem::path(message).string();
        }

        /** The highest version of the folders in @p folder, by their numbers: host/fxr's, shared/Microsoft.NETCore.App's. */
        std::optional<std::filesystem::path> Highest(const std::filesystem::path& folder)
        {
            std::error_code ignored;
            std::optional<std::filesystem::path> best;
            std::vector<int> bestNumbers;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
                if (!entry.is_directory(ignored)) continue;
                std::vector<int> numbers;
                const std::string name = entry.path().filename().string();
                int number = 0;
                bool any = false;
                for (const char c : name) {
                    if (c >= '0' && c <= '9') { number = number * 10 + (c - '0'); any = true; }
                    else if (c == '.') { numbers.push_back(number); number = 0; }
                    else break;     // 10.0.0-preview: behind what is released
                }
                if (!any) continue;
                numbers.push_back(number);
                if (!best || numbers > bestNumbers) { best = entry.path(); bestNumbers = std::move(numbers); }
            }
            return best;
        }

        /** Where .NET is: beside this program (a runtime it is shipped with), DOTNET_ROOT, or where it is installed. */
        std::vector<std::filesystem::path> DotnetRoots()
        {
            std::vector<std::filesystem::path> roots { detail::ProgramDirectory() / "dotnet" };
            if (auto root = detail::Variable("DOTNET_ROOT"); !root.empty()) roots.push_back(std::move(root));
#ifdef _WIN32
            if (auto programs = detail::Variable("ProgramFiles"); !programs.empty()) roots.push_back(programs / "dotnet");
#else
            if (auto home = detail::Variable("HOME"); !home.empty()) roots.push_back(home / ".dotnet");
            for (const char* root : { "/usr/share/dotnet", "/usr/lib/dotnet", "/usr/lib64/dotnet", "/usr/local/share/dotnet", "/opt/dotnet" }) roots.emplace_back(root);
#endif
            // The `dotnet` the shell would run: its folder.
            const auto path = detail::Variable("PATH").string();
#ifdef _WIN32
            constexpr char separator = ';';
            constexpr const char* program = "dotnet.exe";
#else
            constexpr char separator = ':';
            constexpr const char* program = "dotnet";
#endif
            for (std::size_t at = 0; at <= path.size();) {
                const std::size_t end = std::min(path.find(separator, at), path.size());
                const std::filesystem::path folder = path.substr(at, end - at);
                std::error_code ignored;
                if (!folder.empty() && std::filesystem::exists(folder / program, ignored))
                    roots.push_back(std::filesystem::weakly_canonical(folder / program, ignored).parent_path());
                at = end + 1;
            }
            return roots;
        }

        class DotnetRuntime final : public Runtime {
        public:
            using Start = int (*)(void* engine);
            using AttachFunction = int (*)(void* document, const char* project);
            using Call = void (*)();

            Start start = nullptr;
            AttachFunction attach = nullptr;
            Call poll = nullptr, detach = nullptr;

            bool Attach(void* document, const std::filesystem::path& project, std::string&) override
            {
                // 0 is scripts that did not compile: the log says why, and they are compiled again when one is saved.
                // Run, as far as this goes.
                const auto folder = project.u8string();
                attach(document, reinterpret_cast<const char*>(folder.c_str()));
                return true;
            }
            void Poll() override { poll(); }
            void Detach() override { detach(); }
        };

        std::unique_ptr<DotnetRuntime> StartDotnet(std::string& error)
        {
            // What it starts as: Koral.Engine.Host, beside this program (in dotnet/), or where KORAL_ENGINE_DOTNET says.
            std::filesystem::path assemblies = detail::Variable("KORAL_ENGINE_DOTNET");
            if (assemblies.empty()) assemblies = detail::ProgramDirectory() / "dotnet";
            const auto host = assemblies / "Koral.Engine.Host.dll";
            std::error_code ignored;
            if (!std::filesystem::exists(host, ignored)) {
                error = std::format("the engine's C# layer is not beside this program ({} is not there): build it with a .NET 10 SDK", host.string());
                return nullptr;
            }
            // hostfxr, of the newest .NET there is.
            void* fxr = nullptr;
            std::filesystem::path root;
            std::string loading;
            for (const auto& candidate : DotnetRoots()) {
                const auto version = Highest(candidate / "host" / "fxr");
                if (!version || !std::filesystem::exists(*version / Fxr, ignored)) continue;
                if ((fxr = detail::LoadLibraryAt(*version / Fxr, loading))) { root = candidate; break; }
            }
            if (!fxr) {
                error = std::format(".NET was not found: install the .NET 10 runtime, or set DOTNET_ROOT to where it is{}", loading.empty() ? "" : " (" + loading + ")");
                return nullptr;
            }
            const auto initialize = reinterpret_cast<InitializeForCommandLine>(detail::Function(fxr, "hostfxr_initialize_for_dotnet_command_line"));
            const auto getDelegate = reinterpret_cast<GetRuntimeDelegate>(detail::Function(fxr, "hostfxr_get_runtime_delegate"));
            const auto close = reinterpret_cast<Close>(detail::Function(fxr, "hostfxr_close"));
            const auto setErrorWriter = reinterpret_cast<SetErrorWriter>(detail::Function(fxr, "hostfxr_set_error_writer"));
            if (!initialize || !getDelegate || !close) { error = std::format("the .NET in {} is too old to be started from here", root.string()); return nullptr; }
            if (setErrorWriter) setErrorWriter(Heard);

            // The managed side loads the same Koral as this program has: not another copy of it, which knows of nothing here.
            detail::SetVariable("KORAL_LIBRARY", detail::KoralLibrary());
            // A script's edit made to the code that runs, where it can be (a body changed, nothing added): .NET lets
            // an assembly be changed so only when told before it starts.
            detail::SetVariable("DOTNET_MODIFIABLE_ASSEMBLIES", "debug");

            const auto hostPath = Native(host);
            const auto rootPath = Native(root);
            const InitializeParameters parameters { sizeof(InitializeParameters), hostPath.c_str(), rootPath.c_str() };
            const char_t* argv[] { hostPath.c_str() };
            void* context = nullptr;
            Said().clear();
            const std::int32_t started = initialize(1, argv, &parameters, &context);
            if (started < 0 || !context) {
                error = std::format(".NET could not be started (0x{:08x}){}", static_cast<std::uint32_t>(started), Said().empty() ? "" : ": " + Said());
                if (context) close(context);
                return nullptr;
            }
            void* functionPointer = nullptr;
            const std::int32_t got = getDelegate(context, GetFunctionPointerDelegate, &functionPointer);
            close(context);
            if (got < 0 || !functionPointer) {
                error = std::format(".NET started, but gave nothing to call into it with (0x{:08x}){}", static_cast<std::uint32_t>(got), Said().empty() ? "" : ": " + Said());
                return nullptr;
            }
            const auto function = reinterpret_cast<GetFunctionPointer>(functionPointer);
            auto runtime = std::make_unique<DotnetRuntime>();
            const auto find = [&](const char_t* name, void** into) {
                return function(KHOST_TEXT("Koral.Engine.Embedded, Koral.Engine"), name, UnmanagedCallersOnly, nullptr, nullptr, into) == 0 && *into;
            };
            if (!find(KHOST_TEXT("Start"), reinterpret_cast<void**>(&runtime->start)) || !find(KHOST_TEXT("Attach"), reinterpret_cast<void**>(&runtime->attach))
                || !find(KHOST_TEXT("Poll"), reinterpret_cast<void**>(&runtime->poll)) || !find(KHOST_TEXT("Detach"), reinterpret_cast<void**>(&runtime->detach))) {
                error = std::format("Koral.Engine in {} has no Koral.Engine.Embedded: it is another version's", assemblies.string());
                return nullptr;
            }
            if (runtime->start(nullptr) == 0) { error = "the engine's C# layer could not start: the log says why"; return nullptr; }
            std::fprintf(stderr, "[scripts] .NET started, from %s\n", root.string().c_str());
            return runtime;
        }
    }

    Runtime* Dotnet(std::string& error)
    {
        // Once: .NET cannot be started a second time in a process, nor stopped.
        static std::string why;
        static const std::unique_ptr<DotnetRuntime> runtime = StartDotnet(why);
        if (!runtime) error = why;
        return runtime.get();
    }
}
