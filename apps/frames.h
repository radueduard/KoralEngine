// How koral-editor and koral-play run their frames: the project's scripts (khost::Scripts) run inside the program, on
// the scene that is open, looked after between one frame and the next.

#pragma once

#include <functional>

#include <app.h>

#include <khost/scripts.h>

namespace apps
{
    /**
     * Frames until no window is left — or, @p frames given, so many — with @p scripts following the document @p open
     * gives (null for none) between each. A run of so many frames waits for the scripts to be compiled before its
     * frames start, so that they are what it runs.
     */
    inline void RunFrames(kor::App& app, khost::Scripts& scripts, const std::function<keditor::Document*()>& open, const long frames = 0,
                          const std::function<void(long)>& each = {})
    {
        scripts.Follow(open());
        if (frames > 0) scripts.Settle();
        for (long i = 0; frames <= 0 || i < frames; ++i) {
            if (each) each(i);
            if (!app.Frame()) break;
            scripts.Follow(open());
        }
        // The scripts go on until the document goes: it tells them, while it is whole (ScriptRuntime::Forget).
    }
}
