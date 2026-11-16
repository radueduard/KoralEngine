# A scene over a network

Playing a scene over a network is the **Network** module's: a Kotlin module, in `EngineProjects/NetworkModule`, which a
project imports as it imports Physics. Its README says how — the `Network` and `Replicated` components, messages between
scripts by topic, and how a game's server and client scenes are laid out. This page is what the engine does for it.

## A server needs no window

A scene with no Screen and no ScreenFramebuffer — in its graph, or in a function it calls — shows nothing, and
`koral-play` runs it with no window and no display at all: offscreen, `--rate N` frames a second (60), until it is
stopped (`Document::ShowsAnything`). That is a server's scene. A scene that draws opens its window as before.

## What a script sends of an entity

`world.entityJson(entity, skip)` is an entity as JSON — its name, and each of its components' fields but those in
`skip` — and `world.setEntityJson(entity, json)` makes another entity so, in another world. `world.changedTicksOf(entities,
ignoring)` says which changed since a tick, and `world.worldMatricesOf(entities)` where many are. See
[scripts](scripts.md#many-entities-one-script) — they are what the module sends, and makes again.

## Several editors on one project

A project can be open in several editors at once — a server's and its clients'. Each scene is changed by one of them:
the first to open it holds its lock (`.koral/cache/locks/<scene>.lock`, a lock the operating system keeps, let go of
when the editor closes or crashes). In the others the scene is **read-only**, as the status bar says:

- It can be looked at and played.
- What is done to it is undone — read again from its file — and Save says who saves it.
- It is read again whenever the editor that holds it saves it.
- Once that editor opens another scene, or closes, the next to look takes the lock and can change it.

So three clients' scenes and a server's can each be edited in their own editor, at once. **File, Open another editor**
opens one — this program again, on the same project — with the scene chosen there (`koral-editor <project> --scene
<name>`). Each editor's title says its project, its scene, and whether that is another's to change.
