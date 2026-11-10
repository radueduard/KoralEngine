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
    /** @brief How many bytes a Material's picture (a Texture field) is: a shared reference, whatever it refers to. */
    inline constexpr std::size_t TextureSize = sizeof(std::shared_ptr<void>);

    /** @brief A class of conditions a source declares (one that derives from Conditions): no component, but tests the graph keeps entities by. */
    struct DeclaredConditions {
        std::string name;                   ///< The class's: what each of its conditions is named under, `<name>.<function>`.
        std::string file;                   ///< The file it is declared in, as it was read; empty for one read from a string.
        std::vector<std::string> functions; ///< Each [Condition(N)] function's name, in the order it declares them.
    };

    /** @brief What a script source declares: a type for each class that is a Script, and what could not be read. */
    struct Declarations {
        std::vector<ScriptType> types;      ///< No callbacks: what the component is, not what is done with it.
        std::vector<DeclaredConditions> conditions;     ///< Its classes of conditions.
        std::vector<std::string> problems;  ///< A field of a type a script cannot keep, a starting value not understood.
    };

    /**
     * @brief The script types @p source declares, read as C# when @p kotlin is false and Kotlin when it is. A class is a
     *        Script's when it derives from Setup (or, as one was written before it, Script or GraphicsScript); abstract ones are none. One that derives from
     *        ResourceScript makes resources (ScriptType::makesResources), and is no component: its fields of a resource
     *        type — `public Image Height = new(format: "eR32_SFLOAT", width: 256, height: 256);`, Kotlin's
     *        `val height = Image(format = "eR32_SFLOAT", width = 256, height = 256)` — are what it makes, each
     *        argument one of what it is made with (`width`, `height` and `depth` its extent's). Its component is named for
     *        the class, or for its ScriptName. A field's starting value is understood when it is a number, true or
     *        false, or a vector, quaternion or matrix made of numbers; anything else starts at zero, and is said.
     *        One that derives from Material is a material kind (ScriptType::material): its Texture fields come first,
     *        each starting as the picture it is given (`"@textures/ramp.png"`, `Texture.White`, `Texture.Normal`).
     *        A field's `[Choices("A", "B")]` names what a whole number of it is.
     * @param file What is said of a problem as where it is.
     */
    [[nodiscard]] Declarations Declare(std::string_view source, bool kotlin, std::string_view file = {});
    /** @brief The script types of a file (`.cs` or `.kt`), or of every such file under a folder, in the order of their paths. */
    [[nodiscard]] Declarations DeclareFiles(const std::filesystem::path& path);
    /** @brief The script types of @p files, read together: a class in one may derive from a class in another. */
    [[nodiscard]] Declarations DeclareFiles(const std::vector<std::filesystem::path>& files);

    /**
     * @brief The engine's own package: scripts every project has — the scene's Camera, its lights, its Sky — in its
     *        `scripts` folder. `KORAL_ENGINE_PACKAGE` in the environment says where; otherwise `packages/koral` beside
     *        the program, as a release of the engine has it; otherwise where the engine was built from.
     */
    [[nodiscard]] std::filesystem::path PackageDirectory();
    /** @brief The folder the running program is in: where a release of the engine keeps what it ships with it. */
    [[nodiscard]] std::filesystem::path ProgramDirectory();
    /**
     * @brief Makes the package's script types known to @p scripts' world, as data (Scripts::Declare): every world an
     *        engine scene is of has them. Its material kinds are no Scripts' components: they are put in @p materials,
     *        by name, when it is given. What could not be declared is returned.
     */
    std::vector<std::string> DeclarePackage(Scripts& scripts, std::vector<ScriptType>* materials = nullptr);
}
