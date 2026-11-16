#include <keditor/scene_lock.h>

#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace keditor
{
    namespace {
        std::string ThisProgram()
        {
#ifdef _WIN32
            char host[256] = {};
            DWORD size = sizeof host;
            GetComputerNameA(host, &size);
            return std::format("koral-editor {} on {}", GetCurrentProcessId(), host);
#else
            char host[256] = {};
            gethostname(host, sizeof host - 1);
            return std::format("koral-editor {} on {}", static_cast<long>(getpid()), host);
#endif
        }

        std::string Read(const std::filesystem::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            std::string said = text.str();
            while (!said.empty() && (said.back() == '\n' || said.back() == '\r' || said.back() == '\0')) said.pop_back();
            return said.empty() ? std::string("another editor") : said;
        }
    }

    std::filesystem::path SceneLock::FileOf(const std::filesystem::path& scene)
    {
        const std::filesystem::path absolute = std::filesystem::absolute(scene);
        // scenes/<name>.scene.json, in the project: its lock beside the project's other things of the moment.
        std::string name = absolute.filename().string();
        if (const auto at = name.find(".scene.json"); at != std::string::npos) name.resize(at);
        return absolute.parent_path().parent_path() / ".koral" / "cache" / "locks" / (name + ".lock");
    }

    bool SceneLock::Take(const std::filesystem::path& scene)
    {
        Release();
        _scene = scene;
        _holder.clear();
        const std::filesystem::path file = FileOf(scene);
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
#ifdef _WIN32
        // Shared, so that another editor can open the file to read who holds it; the first byte locked is the right.
        Handle handle = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;   // no holder: it could not be made
        OVERLAPPED at {};
        if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
            CloseHandle(handle);
            _holder = Read(file);
            return false;
        }
        const std::string me = ThisProgram();
        SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(handle);
        DWORD written = 0;
        WriteFile(handle, me.data(), static_cast<DWORD>(me.size()), &written, nullptr);
#else
        const Handle handle = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (handle < 0) return false;   // no holder: it could not be made
        if (flock(handle, LOCK_EX | LOCK_NB) != 0) {
            close(handle);
            _holder = Read(file);
            return false;
        }
        const std::string me = ThisProgram();
        if (ftruncate(handle, 0) == 0) (void)!pwrite(handle, me.data(), me.size(), 0);
#endif
        _handle = handle;
        return true;
    }

    void SceneLock::Release()
    {
        if (_handle == Nothing) return;
#ifdef _WIN32
        OVERLAPPED at {};
        UnlockFileEx(_handle, 0, 1, 0, &at);
        CloseHandle(_handle);
#else
        flock(_handle, LOCK_UN);
        close(_handle);
#endif
        _handle = Nothing;
    }
}
