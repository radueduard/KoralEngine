// The engine's scenes, for whatever runs them: the framework's runtime, or a program in another language that opens
// them by name.

#include <sceneLibrary.h>

#include <keditor/editor.h>
#include <keditor/play.h>

KORAL_SCENES(
    KORAL_SCENE("Play", keditor::PlayScene),
    KORAL_SCENE("Editor", keditor::EditorScene),
)
