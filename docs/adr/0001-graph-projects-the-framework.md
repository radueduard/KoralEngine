# The graph projects the framework; friendliness comes from Functions

The pass graph is meant for prototyping and for optimisation alike, so it has to stay complete with what the GPU can
do. We decided that the graph exposes what the Koral framework (`kor::`) exposes and nothing past it. A Vulkan
capability the graph lacks is added to the framework first, and reaches the graph as an input on the node it belongs to
(for example async compute as a Pass's `async`). Friendlier, higher-level building blocks (bloom, shadow cascades, a
camera) are Functions made of those nodes, which can always be opened, copied and edited. They are never node types
implemented in C++ that hide what they do. So "friendly" and "complete" never compete: every friendly node is a box of
complete nodes that an optimiser can open.

## Considered Options

- High-level node types written in C++ (a `BloomNode`): quicker to make, but closed. You can't tune what's inside, and
  each one is a second implementation to keep in step with the framework.
- Graph nodes that call Vulkan directly, past the framework: complete sooner, but the frame graph's ordering, barriers
  and aliasing would no longer know what those nodes do.
