# Koral Engine

A scene's rendering and simulation is a graph of nodes that the editor draws and the runtime turns into GPU work.

## The graph

**Page**:
A named group of nodes in the graph, laid out together, with a rate (setup, fixed step, every frame) and a place in the order. It records nothing itself.
_Avoid_: Pass (for the group), tab, layer

**Pass**:
A node that records GPU work (Clear, Blit, Dispatch, Draw, TraceRays, …). It is what is ordered, synchronised, timed and put on a queue.
_Avoid_: Command node, page (for this)

**Function**:
A graph of its own that is called as one node and can always be opened down to the nodes it is made of. It is how the graph gets friendlier without hiding anything.
_Avoid_: Macro, subgraph, black-box node

**Bundle**:
A set of named values carried on one wire: an entity's component element, a camera. A field it reaches is filled by name, and a wire to a single field takes precedence over the bundle.
_Avoid_: Struct (for the wire), record, packet

**Frame view**:
The read-only picture of the graph as it runs: only its Passes, in the order the frame graph schedules them, with the resources between them and what each costs.
_Avoid_: Timeline, profiler, overview (the overview is every node at once, laid out)
