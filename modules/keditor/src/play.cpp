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

    void PlayScene::Update() { if (_document) _document->Update(); }
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
