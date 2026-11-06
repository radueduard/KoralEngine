// What finding and loading things is on each OS: this program's folder, a library, a symbol of its own.

#include "runtime.h"

#include <cstdlib>
#include <format>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <link.h>
#  include <unistd.h>
#endif

extern "C" void* khost_symbol(const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), name));
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

namespace khost::detail
{
    std::filesystem::path ProgramDirectory()
    {
#ifdef _WIN32
        wchar_t path[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        return std::filesystem::path(std::wstring(path, length)).parent_path();
#else
        std::error_code ignored;
        return std::filesystem::read_symlink("/proc/self/exe", ignored).parent_path();
#endif
    }

    std::filesystem::path KoralLibrary()
    {
#ifdef _WIN32
        if (const HMODULE koral = GetModuleHandleW(L"Koral.dll")) {
            wchar_t path[MAX_PATH * 4];
            const DWORD length = GetModuleFileNameW(koral, path, static_cast<DWORD>(std::size(path)));
            return std::filesystem::path(std::wstring(path, length));
        }
#else
        if (void* symbol = dlsym(RTLD_DEFAULT, "koral_last_error")) {
            Dl_info info {};
            if (dladdr(symbol, &info) && info.dli_fname) return std::filesystem::path(info.dli_fname);
        }
#endif
        return {};
    }

    void SetVariable(const char* name, const std::filesystem::path& value)
    {
        if (value.empty() || !Variable(name).empty()) return;
#ifdef _WIN32
        // The OS's environment as well as the C runtime's: .NET and the JVM read the OS's.
        SetEnvironmentVariableW(std::filesystem::path(name).wstring().c_str(), value.wstring().c_str());
        _putenv_s(name, value.string().c_str());
#else
        setenv(name, value.c_str(), 0);
#endif
    }

    std::filesystem::path Variable(const char* name)
    {
#ifdef _WIN32
        wchar_t value[MAX_PATH * 4];
        const DWORD length = GetEnvironmentVariableW(std::filesystem::path(name).wstring().c_str(), value, static_cast<DWORD>(std::size(value)));
        return length > 0 && length < std::size(value) ? std::filesystem::path(std::wstring(value, length)) : std::filesystem::path();
#else
        const char* value = std::getenv(name);
        return value && *value ? std::filesystem::path(value) : std::filesystem::path();
#endif
    }

    void* LoadLibraryAt(const std::filesystem::path& path, std::string& error)
    {
#ifdef _WIN32
        // What it imports is looked for beside it first, as a JDK's or .NET's libraries are laid out.
        HMODULE library = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library) error = std::format("{} could not be loaded (error {})", path.string(), GetLastError());
        return library;
#else
        void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!library) error = std::format("{} could not be loaded: {}", path.string(), dlerror());
        return library;
#endif
    }

    void* Function(void* library, const char* name)
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
        return dlsym(library, name);
#endif
    }
}
