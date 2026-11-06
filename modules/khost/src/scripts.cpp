#include <khost/scripts.h>

#include "runtime.h"

#include <cstdio>
#include <format>

#include <keditor/document.h>
#include <keditor/project.h>

namespace khost
{
    namespace {
        bool Same(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::error_code ignored;
            return std::filesystem::equivalent(a, b, ignored);
        }
    }

    Scripts::Scripts() { keditor::scriptRuntime = this; }

    Scripts::~Scripts()
    {
        Stop();
        if (keditor::scriptRuntime == this) keditor::scriptRuntime = nullptr;
    }

    void Scripts::Follow(keditor::Document* document)
    {
        if (document != _document) {
            Stop();
            _document = document;
            Begin();
        } else if (_document && !_running && _language == 0 && !_project.empty() && ++_sinceLooked >= 30) {
            // A project with no scripts yet: looked at again now and then, for the first, which says their language.
            _sinceLooked = 0;
            Begin();
        }
        if (_running) _running->Poll();
    }

    void Scripts::Begin()
    {
        _project.clear();
        _language = 0;
        _whyNot.clear();
        if (!_document) return;
        const auto directory = _document->ProjectDirectory();
        if (directory.empty()) return;
        keditor::Project project;
        std::string error;
        if (!keditor::Project::Open(directory, project, error)) return;
        _project = project.directory;
        _language = project.Language();
        if (_language == 0) return;     // no scripts, and no language said for them yet
        Runtime* runtime = _language == 'k' ? Jvm(error) : Dotnet(error);
        const char* language = _language == 'k' ? "Kotlin" : "C#";
        if (!runtime) {
            _whyNot = std::format("its scripts are in {}, and {}", language, error);
            std::fprintf(stderr, "[scripts] %s: %s\n", project.name.c_str(), _whyNot.c_str());
            return;
        }
        if (!runtime->Attach(_document, _project, error)) {
            _whyNot = std::format("its {} scripts could not be run: {}", language, error);
            std::fprintf(stderr, "[scripts] %s: %s\n", project.name.c_str(), _whyNot.c_str());
            return;
        }
        _running = runtime;
    }

    void Scripts::Settle()
    {
        if (_running) _running->Settle();
    }

    void Scripts::Stop()
    {
        if (_running) _running->Detach();
        _running = nullptr;
        _document = nullptr;
        _project.clear();
        _language = 0;
    }

    bool Scripts::Runs(const std::filesystem::path& project, std::string& why) const
    {
        if (_running && Same(project, _project)) return true;
        why = !_whyNot.empty() && Same(project, _project) ? _whyNot : "they have not been started yet";
        return false;
    }

    void Scripts::Forget(keditor::Document* document)
    {
        if (document == _document) Stop();
    }
}
