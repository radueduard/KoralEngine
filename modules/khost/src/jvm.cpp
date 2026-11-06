// A JVM in this process, through JNI: its classpath is the engine's Kotlin layer and everything it runs on (beside
// this program, in kotlin/lib), and Kotlin's compiler (kotlin/compiler) compiles a project's scripts in it.
// koral.engine.Embedded's functions are what this calls.

#include "runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <format>
#include <vector>

#include <kscript/declarations.h>

#ifdef KHOST_JNI
#include <jni.h>
#endif

namespace khost
{
#ifndef KHOST_JNI
    Runtime* Jvm(std::string& error)
    {
        error = "this build of the engine runs no Kotlin: it was built without a JDK";
        return nullptr;
    }
#else
    namespace {
        using CreateJavaVM = jint (JNICALL*)(JavaVM** vm, void** env, void* arguments);
        using GetCreatedJavaVMs = jint (JNICALL*)(JavaVM** vms, jsize size, jsize* count);

#ifdef _WIN32
        constexpr char Separator = ';';
        const std::filesystem::path JvmLibrary = std::filesystem::path("bin") / "server" / "jvm.dll";
#elif defined(__APPLE__)
        constexpr char Separator = ':';
        const std::filesystem::path JvmLibrary = std::filesystem::path("lib") / "server" / "libjvm.dylib";
#else
        constexpr char Separator = ':';
        const std::filesystem::path JvmLibrary = std::filesystem::path("lib") / "server" / "libjvm.so";
#endif

        /** Where a JVM is: beside this program (one it is shipped with), KORAL_ENGINE_JAVA_HOME, JAVA_HOME, or this build's JDK. */
        std::vector<std::filesystem::path> JavaHomes()
        {
            std::vector<std::filesystem::path> homes { detail::ProgramDirectory() / "jre" };
            for (const char* name : { "KORAL_ENGINE_JAVA_HOME", "JAVA_HOME" })
                if (auto home = detail::Variable(name); !home.empty()) homes.push_back(std::move(home));
#ifdef KHOST_JAVA_HOME
            homes.emplace_back(KHOST_JAVA_HOME);
#endif
            return homes;
        }

        /** The jars of @p folder, one after another as a classpath has them. */
        std::string Jars(const std::filesystem::path& folder)
        {
            std::vector<std::filesystem::path> jars;
            std::error_code ignored;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
                if (entry.path().extension() == ".jar") jars.push_back(entry.path());
            std::ranges::sort(jars);
            std::string classpath;
            for (const auto& jar : jars) {
                if (!classpath.empty()) classpath += Separator;
                classpath += jar.string();
            }
            return classpath;
        }

        /** A Java string of @p text, as JNI takes it: modified UTF-8, which is UTF-8 for any path there is. */
        jstring Text(JNIEnv* env, const std::filesystem::path& text)
        {
            const auto utf8 = text.u8string();
            return env->NewStringUTF(reinterpret_cast<const char*>(utf8.c_str()));
        }

        /** What a Java string says: empty for none. */
        std::string Said(JNIEnv* env, jstring text)
        {
            if (!text) return {};
            const char* chars = env->GetStringUTFChars(text, nullptr);
            std::string said = chars ? chars : "";
            if (chars) env->ReleaseStringUTFChars(text, chars);
            env->DeleteLocalRef(text);
            return said;
        }

        class JvmRuntime final : public Runtime {
        public:
            JavaVM* vm = nullptr;
            JNIEnv* env = nullptr;
            jclass embedded = nullptr;
            jmethodID start = nullptr, attach = nullptr, poll = nullptr, settle = nullptr, detach = nullptr;

            /** Whether the call before threw: said, and cleared, so that the next can be made. */
            bool Threw()
            {
                if (!env->ExceptionCheck()) return false;
                env->ExceptionDescribe();
                env->ExceptionClear();
                return true;
            }

            bool Attach(void* document, const std::filesystem::path& project, std::string& error) override
            {
                const jstring folder = Text(env, project), package = Text(env, kscript::PackageDirectory());
                const auto why = Said(env, static_cast<jstring>(env->CallStaticObjectMethod(embedded, attach, static_cast<jlong>(reinterpret_cast<std::intptr_t>(document)), folder, package)));
                env->DeleteLocalRef(folder);
                env->DeleteLocalRef(package);
                if (Threw()) { error = "the engine's Kotlin layer threw: what it threw is above"; return false; }
                if (!why.empty()) { error = why; return false; }
                return true;
            }
            void Poll() override { env->CallStaticVoidMethod(embedded, poll); Threw(); }
            void Settle() override { env->CallStaticVoidMethod(embedded, settle); Threw(); }
            void Detach() override { env->CallStaticVoidMethod(embedded, detach); Threw(); }
        };

        std::unique_ptr<JvmRuntime> StartJvm(std::string& error)
        {
            // The engine's Kotlin layer, and Kotlin's compiler: beside this program, or where KORAL_ENGINE_KOTLIN says.
            std::filesystem::path kotlin = detail::Variable("KORAL_ENGINE_KOTLIN");
            if (kotlin.empty()) kotlin = detail::ProgramDirectory() / "kotlin";
            const std::string classpath = Jars(kotlin / "lib");
            std::error_code ignored;
            if (classpath.empty() || !std::filesystem::is_directory(kotlin / "compiler", ignored)) {
                error = std::format("the engine's Kotlin layer is not beside this program ({} has no lib and compiler): build it with a JDK", kotlin.string());
                return nullptr;
            }
            void* library = nullptr;
            std::filesystem::path home;
            std::string loading;
            for (const auto& candidate : JavaHomes()) {
                if (!std::filesystem::exists(candidate / JvmLibrary, ignored)) continue;
                if ((library = detail::LoadLibraryAt(candidate / JvmLibrary, loading))) { home = candidate; break; }
            }
            if (!library) {
                error = std::format("no Java was found (25 or later): set JAVA_HOME to a JDK{}", loading.empty() ? "" : " (" + loading + ")");
                return nullptr;
            }
            const auto create = reinterpret_cast<CreateJavaVM>(detail::Function(library, "JNI_CreateJavaVM"));
            const auto created = reinterpret_cast<GetCreatedJavaVMs>(detail::Function(library, "JNI_GetCreatedJavaVMs"));
            if (!create || !created) { error = std::format("{} is no JVM", (home / JvmLibrary).string()); return nullptr; }

            // The managed side loads the same Koral as this program has: not another copy of it, which knows of nothing here.
            detail::SetVariable("KORAL_LIBRARY", detail::KoralLibrary());

            auto runtime = std::make_unique<JvmRuntime>();
            jsize count = 0;
            if (created(&runtime->vm, 1, &count) == JNI_OK && count > 0) {
                // One there already (a JVM cannot be made twice): this thread is attached to it.
                if (runtime->vm->AttachCurrentThread(reinterpret_cast<void**>(&runtime->env), nullptr) != JNI_OK) { error = "the JVM there is would not take this thread"; return nullptr; }
            } else {
                std::vector<std::string> said {
                    "-Djava.class.path=" + classpath,
                    "-Dkoral.engine.compiler=" + (kotlin / "compiler").string(),
                    "--enable-native-access=ALL-UNNAMED",   // the engine's and Koral's layers call C through java.lang.foreign
                    "-Xrs",                                 // Control+C, and the signals, stay this program's
                    "--sun-misc-unsafe-memory-access=allow", // Kotlin's compiler reads its jars so: not warned of, each run
                    "-Dstdout.encoding=UTF-8",
                    "-Dstderr.encoding=UTF-8",
                };
                std::vector<JavaVMOption> options;
                for (auto& option : said) options.push_back({ option.data(), nullptr });
                JavaVMInitArgs arguments {};
                arguments.version = JNI_VERSION_21;
                arguments.nOptions = static_cast<jint>(options.size());
                arguments.options = options.data();
                arguments.ignoreUnrecognized = JNI_FALSE;
                if (const jint made = create(&runtime->vm, reinterpret_cast<void**>(&runtime->env), &arguments); made != JNI_OK) {
                    error = std::format("the JVM in {} could not be started ({}): it must be Java 25 or later", home.string(), made);
                    return nullptr;
                }
            }
            JNIEnv* env = runtime->env;
            const jclass found = env->FindClass("koral/engine/Embedded");
            if (!found || runtime->Threw()) { error = std::format("the engine's Kotlin layer in {} has no koral.engine.Embedded: it is another version's", kotlin.string()); return nullptr; }
            runtime->embedded = static_cast<jclass>(env->NewGlobalRef(found));
            env->DeleteLocalRef(found);
            runtime->start = env->GetStaticMethodID(runtime->embedded, "start", "(J)Ljava/lang/String;");
            runtime->attach = env->GetStaticMethodID(runtime->embedded, "attach", "(JLjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
            runtime->poll = env->GetStaticMethodID(runtime->embedded, "poll", "()V");
            runtime->settle = env->GetStaticMethodID(runtime->embedded, "settle", "()V");
            runtime->detach = env->GetStaticMethodID(runtime->embedded, "detach", "()V");
            if (runtime->Threw() || !runtime->start || !runtime->attach || !runtime->poll || !runtime->settle || !runtime->detach) {
                error = std::format("koral.engine.Embedded in {} is another version's", kotlin.string());
                return nullptr;
            }
            const auto why = Said(env, static_cast<jstring>(env->CallStaticObjectMethod(runtime->embedded, runtime->start,
                                                                                         static_cast<jlong>(reinterpret_cast<std::intptr_t>(&khost_symbol)))));
            if (runtime->Threw() || !why.empty()) { error = "the engine's Kotlin layer could not start" + (why.empty() ? std::string() : ": " + why); return nullptr; }
            std::fprintf(stderr, "[scripts] the JVM started, from %s\n", home.string().c_str());
            return runtime;
        }
    }

    Runtime* Jvm(std::string& error)
    {
        // Once: a JVM cannot be started a second time in a process, nor stopped and started again.
        static std::string why;
        static const std::unique_ptr<JvmRuntime> runtime = StartJvm(why);
        if (!runtime) error = why;
        return runtime.get();
    }
#endif
}
