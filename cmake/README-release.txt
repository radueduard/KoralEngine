Koral Engine for Windows (x64)
==============================

  koral-editor.exe   the editor: make and change a project's scenes, graph, scripts and assets
  koral-play.exe     the player: runs a project as a game  (koral-play.exe <project folder>)

Unzip anywhere and run koral-editor.exe. Nothing needs installing: everything the programs use is in this folder.
Keep the folder whole; the programs look for what is beside them.

What the PC needs
-----------------
  - Windows 10 or 11, 64-bit.
  - A GPU and driver with Vulkan 1.3 (any recent NVIDIA, AMD or Intel driver). Ray tracing needs a GPU that has it.

Projects
--------
New projects are made in your home folder, under KoralProjects, unless you pick another place. Set the environment
variable KORAL_ENGINE_PROJECTS to change where that is.

Scripts
-------
C# scripts run on the .NET in dotnet\ and Kotlin scripts on the Java runtime in jre\, when the release has them. If
it does not, C# needs .NET 10 and Kotlin a JDK 25 or later installed on the PC.

To write scripts with completion in an editor of code (Visual Studio Code, Rider, IntelliJ), open the project's
folder in it: the editor writes the files those tools read when the project is opened.

Folders
-------
  modules\, shaders\, assets\   the Koral framework's
  packages\koral                 the engine's own scripts, shaders and functions
  packages\modules               modules a project can import (File > Import a module)
  dotnet\, kotlin\, jre\         what scripts are compiled and run with
  gradle-wrapper\                given to a project's Kotlin build
