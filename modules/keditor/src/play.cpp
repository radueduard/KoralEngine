#include <keditor/play.h>
#include <keditor/project.h>

#include <log.h>

#include <kgraph/scene.h>

namespace keditor
{
    PlayScene::PlayScene(std::filesystem::path file) : _file(std::move(file)) {}
    // "scene" is a scene file. "project" is a project's folder: its shaders are used, and the scene it starts with run.
    PlayScene::PlayScene(const kor::SceneArgs& arguments) : _file(arguments.String("scene"))
    {
        if (!arguments.Has("project")) return;
        Project project;
        if (!Project::Open(arguments.String("project"), project, _error)) return;
        project.Use();
        if (_file.empty()) _file = project.ScenePath(project.start);
    }

    void PlayScene::Initialize()
    {
        New();
        if (!_error.empty()) kor::log::Error("{}", _error);     // the project it was given could not be opened
        else if (!_file.empty() && !Open(_file, _error)) kor::log::Error("{}", _error);
    }

    // A frame, in the order a script counts on: its updates, then what graphics scripts make for the passes, then the
    // passes readied with all of it.
    void PlayScene::FixedUpdate() { if (_document && _playing) _document->Scripts().FixedUpdate(static_cast<float>(Time::FixedDeltaTime())); }
    void PlayScene::Update()
    {
        _replaced.reset();      // the frame it may have been drawn in is done
        if (_onFrame) _onFrame();
        if (!_document || !_playing) return;
        _document->Scripts().Update(static_cast<float>(Time::FrameTime()));
    }
    void PlayScene::LateUpdate()
    {
        if (!_document) return;
        if (_playing) _document->Scripts().LateUpdate(static_cast<float>(Time::FrameTime()));
        _document->Scripts().Prepare();
        // What the graph is told of the frame: how big the window is, and whose debug shapes a DebugDraw node draws.
        _document->Runtime().SetScreenExtent(SceneWindow().Extent());
        _document->Runtime().SetDebugDraw(&SceneDebug());
        _document->Update();
    }
    void PlayScene::Shutdown() { _replaced.reset(); _document.reset(); }

    void PlayScene::New()
    {
        // The old one is kept until this scene's next update: started over from an interface, in the middle of a
        // frame, this frame has drawn with what it holds and is not finished with it. Its passes come out of the
        // graph when it goes, which is before anything is recorded again.
        if (_document) _replaced = std::move(_document);
        _document = std::make_unique<Document>(Graph());
    }

    bool PlayScene::Open(const std::filesystem::path& file, std::string& error)
    {
        New();
        if (_document->Load(file, error)) return true;
        New();
        return false;
    }
}
