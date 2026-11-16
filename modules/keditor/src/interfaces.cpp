#include <keditor/interfaces.h>

#include <algorithm>

#include <frameGraph.h>
#include <input.h>
#include <kui/render.h>
#include <kui/widgets.h>

#include <keditor/document.h>

namespace keditor
{
    void InterfaceHost::Show(kor::FrameGraph& graph, const Document& document, kor::Input& input, const kor::UVec2 size, const float seconds)
    {
        const auto& shown = document.Interfaces();
        // One hidden since: its pass taken out, before anything reaches its Ui.
        for (auto it = _passes.begin(); it != _passes.end();) {
            if (std::ranges::find(shown, it->first, &ScriptInterface::ui) != shown.end()) { ++it; continue; }
            graph.Remove(*it->second);
            it = _passes.erase(it);
        }
        // Each built again from what changed, and given the input: the one shown last first, as it is on top.
        for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
            if (it->compose) it->compose(it->user);
            it->ui->Update(input, kor::Vec2(static_cast<float>(size.x), static_cast<float>(size.y)), seconds);
        }
        // Over everything the graph draws: its last passes, in the order they were shown. Writers of the screen run in
        // the order their passes were added, so one added since — the game's graph built again — puts them back last.
        const auto passes = graph.Passes();
        bool last = passes.size() >= shown.size() && _passes.size() == shown.size();
        for (std::size_t i = 0; last && i < shown.size(); ++i)
            last = _passes[i].first == shown[i].ui && passes[passes.size() - shown.size() + i] == _passes[i].second;
        if (last) return;
        for (const auto& [ui, pass] : _passes) graph.Remove(*pass);
        _passes.clear();
        for (const auto& interface : shown) _passes.emplace_back(interface.ui, &graph.Add<kui::UiPass>(*interface.ui));
    }

    void InterfaceHost::Clear(kor::FrameGraph& graph)
    {
        for (const auto& [ui, pass] : _passes) graph.Remove(*pass);
        _passes.clear();
    }
}
