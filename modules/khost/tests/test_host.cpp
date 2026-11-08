// khost: what finds and starts .NET and a JVM in this process. Each test is a process of its own (ctest runs them so),
// which a runtime needs: neither can be started twice in one.

#include "../src/runtime.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// The folder this program is in: where the runtimes' layers are looked for, beside it.
TEST(Platform, TheProgramsFolderIsWhereItIs)
{
    const fs::path here = khost::detail::ProgramDirectory();
    ASSERT_FALSE(here.empty());
    EXPECT_TRUE(fs::is_directory(here)) << here;
#ifdef _WIN32
    EXPECT_TRUE(fs::exists(here / "khost_tests.exe")) << here;
#else
    EXPECT_TRUE(fs::exists(here / "khost_tests")) << here;
#endif
}

// What is said to a runtime started after: said once, and not over what is said already.
TEST(Platform, AVariableIsSetOnlyWhereItIsNot)
{
    EXPECT_TRUE(khost::detail::Variable("KHOST_TEST_NEVER_SET").empty());
    khost::detail::SetVariable("KHOST_TEST_VARIABLE", "first");
    EXPECT_EQ(khost::detail::Variable("KHOST_TEST_VARIABLE"), fs::path("first"));
    khost::detail::SetVariable("KHOST_TEST_VARIABLE", "second");
    EXPECT_EQ(khost::detail::Variable("KHOST_TEST_VARIABLE"), fs::path("first")) << "what was said stays";
    khost::detail::SetVariable("KHOST_TEST_EMPTY", fs::path());
    EXPECT_TRUE(khost::detail::Variable("KHOST_TEST_EMPTY").empty()) << "nothing is not said";
}

// A library that is not there says where it was looked for.
TEST(Platform, ALibraryThatIsNotThereSaysSo)
{
    std::string error;
    const fs::path nowhere = fs::temp_directory_path() / "khost-no-such-library" / "missing.dll";
    EXPECT_EQ(khost::detail::LoadLibraryAt(nowhere, error), nullptr);
    EXPECT_NE(error.find("missing"), std::string::npos) << error;
}

// The Koral library this program loaded: what the managed side is to load too.
TEST(Platform, TheKoralLibraryIsTheOneLoaded)
{
    const fs::path koral = khost::detail::KoralLibrary();
    ASSERT_FALSE(koral.empty()) << "the program links Koral";
    EXPECT_TRUE(fs::exists(koral)) << koral;
}

// The engine's C interface, found in the program by name, as the managed side finds it.
TEST(Platform, TheEnginesInterfaceIsFoundInTheProgram)
{
    EXPECT_NE(khost_symbol("kengine_last_error"), nullptr) << "the program exports the engine's C interface";
    EXPECT_EQ(khost_symbol("khost_no_such_symbol"), nullptr);
}

// Shipped (KORAL_ENGINE_SHIP_RUNTIMES): a .NET and a JRE beside the program, laid out as khost looks for them first.
TEST(Runtimes, TheShippedOnesAreWhereTheyAreLookedForFirst)
{
#ifndef KHOST_TEST_SHIPPED
    GTEST_SKIP() << "this build ships no runtimes";
#else
    const fs::path here = khost::detail::ProgramDirectory();
    std::error_code ignored;
    bool fxr = false, runtime = false;
    for (const auto& entry : fs::directory_iterator(here / "dotnet" / "host" / "fxr", ignored)) fxr = fxr || fs::exists(entry.path() / "hostfxr.dll") || fs::exists(entry.path() / "libhostfxr.so");
    for (const auto& entry : fs::directory_iterator(here / "dotnet" / "shared" / "Microsoft.NETCore.App", ignored)) runtime = runtime || fs::exists(entry.path() / "System.Private.CoreLib.dll");
    EXPECT_TRUE(fxr) << "hostfxr, in dotnet/host/fxr/<version>";
    EXPECT_TRUE(runtime) << "the runtime, in dotnet/shared/Microsoft.NETCore.App/<version>";
    EXPECT_TRUE(fs::exists(here / "jre" / "bin" / "server" / "jvm.dll") || fs::exists(here / "jre" / "lib" / "server" / "libjvm.so")) << "a JVM, in jre/";
#endif
}

// .NET starts, once, where the build put the engine's C# layer — and asked again, is the same.
TEST(Runtimes, DotnetStartsWithTheEnginesLayer)
{
#ifndef KHOST_TEST_DOTNET
    GTEST_SKIP() << "this build has no C# layer";
#else
    khost::detail::SetVariable("KORAL_ENGINE_DOTNET", KHOST_TEST_DOTNET);
    std::string error;
    khost::Runtime* dotnet = khost::Dotnet(error);
    ASSERT_NE(dotnet, nullptr) << error;
    std::string again;
    EXPECT_EQ(khost::Dotnet(again), dotnet) << "one .NET a process";
#endif
}

// A JVM starts, once, where the build put the engine's Kotlin layer.
TEST(Runtimes, AJvmStartsWithTheEnginesLayer)
{
#ifndef KHOST_TEST_KOTLIN
    GTEST_SKIP() << "this build has no Kotlin layer";
#else
    khost::detail::SetVariable("KORAL_ENGINE_KOTLIN", KHOST_TEST_KOTLIN);
    std::string error;
    khost::Runtime* jvm = khost::Jvm(error);
    ASSERT_NE(jvm, nullptr) << error;
    std::string again;
    EXPECT_EQ(khost::Jvm(again), jvm) << "one JVM a process";
#endif
}
