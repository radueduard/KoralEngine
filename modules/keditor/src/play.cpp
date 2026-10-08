#include <keditor/play.h>

#include <log.h>

namespace keditor
{
    PlayScene::PlayScene(std::filesystem::path file) : _file(std::move(file)) {}
    PlayScene::PlayScene(const kor::SceneArgs& arguments) : _file(arguments.String("scene")) {}

    void PlayScene::Initialize()
    {
        New();
        if (!_file.empty() && !Open(_file, _error)) kor::log::Error("{}", _error);
    }

    // A frame, in the order a script counts on: its updates, then what graphics scripts make for the passes, then the
    // passes readied with all of it.
    void PlayScene::FixedUpdate() { if (_document) _document->Scripts().FixedUpdate(static_cast<float>(Time::FixedDeltaTime())); }
    void PlayScene::Update() { if (_document) _document->Scripts().Update(static_cast<float>(Time::FrameTime())); }
    void PlayScene::LateUpdate()
    {
        if (!_document) return;
        _document->Scripts().LateUpdate(static_cast<float>(Time::FrameTime()));
        _document->Scripts().Prepare();
        _document->Update();
    }
    void PlayScene::Shutdown() { _document.reset(); }

    void PlayScene::New()
    {
        _document.reset();      // the old one's passes out of the graph before the new one's go in
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
