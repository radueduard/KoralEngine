//
// kscript: a script's component as its source declares it, read without running the script. A C# class that is a
// Script, or a Kotlin one, has its public fields (public vars) laid out as the language's host lays them out, and what
// each is initialised to as the component's starting value. So the engine knows a project's components — and its own
// package's, the scene's camera and lights — whether or not anything runs the scripts.
//

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scripts.h"

namespace kscript
{
    /** @brief What a script source declares: a type for each class that is a Script, and what could not be read. */
    struct Declarations {
        std::vector<ScriptType> types;      ///< No callbacks: what the component is, not what is done with it.
        std::vector<std::string> problems;  ///< A field of a type a script cannot keep, a starting value not understood.
    };

    /**
     * @brief The script types @p source declares, read as C# when @p kotlin is false and Kotlin when it is. A class is a
     *        Script's when it derives from Script or GraphicsScript; abstract ones are none. Its component is named for
     *        the class, or for its ScriptName. A field's starting value is understood when it is a number, true or
     *        false, or a vector, quaternion or matrix made of numbers; anything else starts at zero, and is said.
     * @param file What is said of a problem as where it is.
     */
    [[nodiscard]] Declarations Declare(std::string_view source, bool kotlin, std::string_view file = {});
    /** @brief The script types of a file (`.cs` or `.kt`), or of every such file under a folder, in the order of their paths. */
    [[nodiscard]] Declarations DeclareFiles(const std::filesystem::path& path);

    /**
     * @brief The engine's own package: scripts every project has — the scene's Camera, its lights, its Sky — in its
     *        `scripts` folder. `KORAL_ENGINE_PACKAGE` in the environment says where, and otherwise where the engine
     *        was built from.
     */
    [[nodiscard]] std::filesystem::path PackageDirectory();
    /**
     * @brief Makes the package's script types known to @p scripts' world, as data (Scripts::Declare): every world an
     *        engine scene is of has them. What could not be declared is returned.
     */
    std::vector<std::string> DeclarePackage(Scripts& scripts);
}
