#include <keditor/play.h>
#include "physics.h"
#include <keditor/project.h>

#include <algorithm>
#include <cstddef>
#include <map>

#include <input.h>

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

    namespace {
        std::map<const Document*, PlayScene*>& Scenes()
        {
            static std::map<const Document*, PlayScene*> scenes;
            return scenes;
        }
    }

    // FlyCamera: the one script the engine has of its own, so that a scene can be flown through without a project of
    // scripts. On an entity — a camera — it turns it with the mouse and moves it with the keys while the right button
    // is held: W A S D along and across the way it looks, E and Q up and down, Shift faster.
    namespace {
        struct FlyCamera {
            float speed = 4.f;          // units a second
            float sensitivity = 0.12f;  // degrees a pixel
        };

        void RegisterFlyCamera(PlayScene& scene)
        {
            kscript::ScriptType type;
            type.name = "FlyCamera";
            type.size = sizeof(FlyCamera);
            type.alignment = alignof(FlyCamera);
            type.fields = { { "speed", "f32", offsetof(FlyCamera, speed), sizeof(float), nullptr }, { "sensitivity", "f32", offsetof(FlyCamera, sensitivity), sizeof(float), nullptr } };
            type.callbacks.user = &scene;
            // A component is made of nothing: one that was given no speed has what a FlyCamera starts with.
            type.callbacks.created = [](void*, kecs::Entity, void* data) {
                auto& fly = *static_cast<FlyCamera*>(data);
                if (fly.speed == 0.f && fly.sensitivity == 0.f) fly = FlyCamera {};
            };
            type.callbacks.update = [](void* user, const kecs::Entity entity, void* data, const float delta) {
                auto& scene = *static_cast<PlayScene*>(user);
                const auto& fly = *static_cast<const FlyCamera*>(data);
                const kor::Input& input = scene.ScriptInput();
                if (!input.IsMouseButtonHeld(kor::MouseButton::eRight)) return;
                kecs::World& world = scene.Doc().World();
                const kor::Vec2 moved = input.MousePositionDelta();
                kecs::Vec3 rotation = world.RotationOf(entity);
                rotation.y -= moved.x * fly.sensitivity;
                rotation.x = std::clamp(rotation.x - moved.y * fly.sensitivity, -89.f, 89.f);
                world.SetRotation(entity, rotation);
                // Along the way it looks now (-Z), across it (+X), and straight up.
                const kecs::Mat4 placed = world.WorldMatrix(entity);
                const float along = (input.IsKeyHeld(kor::Key::eW) ? 1.f : 0.f) - (input.IsKeyHeld(kor::Key::eS) ? 1.f : 0.f);
                const float across = (input.IsKeyHeld(kor::Key::eD) ? 1.f : 0.f) - (input.IsKeyHeld(kor::Key::eA) ? 1.f : 0.f);
                const float up = (input.IsKeyHeld(kor::Key::eE) ? 1.f : 0.f) - (input.IsKeyHeld(kor::Key::eQ) ? 1.f : 0.f);
                const float step = fly.speed * delta * (input.IsKeyHeld(kor::Key::eLeftShift) ? 4.f : 1.f);
                kecs::Vec3 position = world.PositionOf(entity);
                position.x += (-placed.At(2, 0) * along + placed.At(0, 0) * across) * step;
                position.y += (-placed.At(2, 1) * along + placed.At(0, 1) * across + up) * step;
                position.z += (-placed.At(2, 2) * along + placed.At(0, 2) * across) * step;
                world.SetPosition(entity, position);
            };
            scene.Doc().Scripts().Register(std::move(type));
        }
    }

    PlayScene* PlayScene::Of(const Document& document)
    {
        const auto it = Scenes().find(&document);
        return it == Scenes().end() ? nullptr : it->second;
    }

    void PlayScene::Initialize()
    {
        New();
        if (!_error.empty()) kor::log::Error("{}", _error);     // the project it was given could not be opened
        else if (!_file.empty() && !Open(_file, _error)) kor::log::Error("{}", _error);
    }

    // A frame, in the order a script counts on: its updates, then what graphics scripts make for the passes, then the
    // passes readied with all of it.
    void PlayScene::FixedUpdate()
    {
        if (!_document || !_playing) return;
        _document->Scripts().FixedUpdate(static_cast<float>(Time::FixedDeltaTime()));
        // The graph's fixed pages run on this frame: it has a step in it.
        _document->Runtime().FixedStep(static_cast<float>(Time::FixedDeltaTime()));
    }
    void PlayScene::StepOnce()
    {
        if (!_document) return;
        _document->Scripts().FixedUpdate(SceneTime().FixedDeltaTime());
        _document->Runtime().FixedStep(SceneTime().FixedDeltaTime());
    }
    void PlayScene::Restart()
    {
        ++_restarts;
        if (_document) _document->Runtime().RunSetupAgain();
    }
    void PlayScene::Update()
    {
        _overlayBefore.reset();
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
        // How far the frame is between the last fixed step and the next: what is drawn between two steps' states.
        _document->Runtime().SetFixedFraction(SceneTime().FixedStepFraction());
        // The editor's shapes are drawn by the editor's own passes, when there are any: not by a node of the scene's.
        _document->Runtime().SetDebugDraw(_overlay ? nullptr : &SceneDebug());
        _document->Update();
        if (_overlay) _overlay->Update(SceneWindow().Extent(), &SceneDebug());
    }

    void PlayScene::EditAs()
    {
        _edited = true;
        if (_document && !_overlay) _overlay = std::make_unique<keditor::Overlay>(Graph(), _document->World());
    }
    void PlayScene::Shutdown()
    {
        if (_document) Scenes().erase(_document.get());
        _overlayBefore.reset();
        _overlay.reset();
        _replaced.reset();
        _document.reset();
    }

    void PlayScene::New()
    {
        // The old one is kept until this scene's next update: started over from an interface, in the middle of a
        // frame, this frame has drawn with what it holds and is not finished with it. Its passes come out of the
        // graph when it goes, which is before anything is recorded again.
        if (_document) { Scenes().erase(_document.get()); _replaced = std::move(_document); _overlayBefore = std::move(_overlay); }
        _document = std::make_unique<Document>(Graph());
        if (_edited) _overlay = std::make_unique<keditor::Overlay>(Graph(), _document->World());
        Scenes()[_document.get()] = this;
        RegisterFlyCamera(*this);
        RegisterPhysics(*this);
    }

    void GameScene::FixedUpdate()
    {
        // Its own runtime runs the same fixed pages, on the same steps, while the scene plays.
        if (_runtime && _play.Playing()) _runtime->FixedStep(static_cast<float>(Time::FixedDeltaTime()));
    }
    void GameScene::Update() { _before.reset(); }

    void GameScene::LateUpdate()
    {
        // Hidden: its passes are out of the frame, and it follows nothing until it is seen again.
        if (_shown != _enabled) {
            _enabled = _shown;
            for (kor::RenderPass* pass : Graph().Passes()) pass->SetEnabled(_enabled);
        }
        if (!_shown) return;
        // A scene started over is another document: the runtime that followed the one before goes, a frame later.
        Document& document = _play.Doc();
        if (&document != _following || !_runtime) {
            _before = std::move(_runtime);
            _runtime = std::make_unique<kgraph::Runtime>(Graph(), &document.World(), document.Runtime());
            _following = &document;
            _seen = 0;
        }
        if (const std::uint64_t revision = document.Runtime().Revision(); revision != _seen) {
            _seen = revision;
            _runtime->Set(document.Runtime().Current());
        }
        if (_play.Restarts() != _restarts) { _restarts = _play.Restarts(); _runtime->RunSetupAgain(); }
        // What the scripts write into is the document's own buffer, here too: not one of this view's.
        _runtime->Share(document.ScriptResources());
        // Whatever the editor sees from, this sees from the scene's camera; and nothing is picked here.
        _runtime->SetScreenExtent(SceneWindow().Extent());
        _runtime->Update();
    }

    void GameScene::Shutdown()
    {
        _runtime.reset();
        _before.reset();
        _following = nullptr;
    }

    bool PlayScene::Open(const std::filesystem::path& file, std::string& error)
    {
        New();
        if (_document->Load(file, error)) return true;
        New();
        return false;
    }
}
