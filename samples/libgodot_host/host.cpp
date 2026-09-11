// libgodot_host: standalone executable whose main() owns thread 0 so
// libgodot's macOS init (which touches NSApplication) is on the main
// thread as AppKit requires.
//
// All communication is over weft::harness (iceoryx2 shared memory).
// Lifecycle commands from elixir and P2P frames between godot hosts use
// the same framing — DYNAMIC-payload pub/sub, byte-slice in, byte-slice
// out — differing only in service name. The BEAM never sees the hot
// path; it drives lifecycle through a weft-harness client.
//
// Modes:
//   --smoke     one create/start/iterate/stop cycle, exit 0
//   (default)   weft::run_command_loop dispatching lifecycle opcodes

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/godot_instance.hpp>

#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)
#include "weft/command.hpp"
#include "weft/loop.hpp"
#endif

// libgodot exports (see 4-entities/entities-godot/core/extension/libgodot.h).
// Only create/destroy are C-exports; start/iteration/stop/focus_*/pause/
// resume live on GodotInstance and are reached through godot-cpp bindings.
typedef GDExtensionObjectPtr (*libgodot_create_fn)(int, char *[], GDExtensionInitializationFunction);
typedef void (*libgodot_destroy_fn)(GDExtensionObjectPtr);

// GDExtension entry-point: register nothing (the host does not own scene
// classes), but hand godot-cpp a valid InitObject so its runtime registers
// its own bookkeeping — otherwise reinterpret_cast'ing the returned
// GodotInstance pointer through godot-cpp will read from an uninitialized
// binding.
static void host_initialize_module(godot::ModuleInitializationLevel /*p_level*/) {}
static void host_uninitialize_module(godot::ModuleInitializationLevel /*p_level*/) {}

extern "C" GDExtensionBool GDE_EXPORT host_gdextension_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization) {
    godot::GDExtensionBinding::InitObject init_object(p_get_proc_address, p_library, r_initialization);
    init_object.register_initializer(host_initialize_module);
    init_object.register_terminator(host_uninitialize_module);
    init_object.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_object.init();
}

static std::string resolve_libgodot_path(const char *arg0) {
    if (const char *env = getenv("LIBGODOT_PATH")) return env;
    std::string self = arg0;
    auto slash = self.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : self.substr(0, slash);
    return dir + "/../../build/libgodot.dylib";
}

static void die(const char *what) {
    const char *err = dlerror();
    fprintf(stderr, "libgodot_host: %s: %s\n", what, err ? err : "unknown");
    exit(2);
}

// -------------------------------------------------------------------------
// Shared state driven by both the CLI smoke path and the bus loop path.
// A single global lets the free-function weft::Ask handler reach the same
// engine the process has already booted, without wrapping ctx around a
// tangle of small structs. The host is a one-instance driver by design
// (libgodot itself refuses a second create).
// -------------------------------------------------------------------------
struct HostState {
    void *lib_handle = nullptr;
    libgodot_create_fn create = nullptr;
    libgodot_destroy_fn destroy = nullptr;
    GDExtensionObjectPtr obj = nullptr;
    godot::GodotInstance *instance = nullptr;
    // argv storage kept alive for the whole engine lifetime — libgodot
    // captures pointers into it via Main::setup.
    std::vector<std::string> argv_storage;
    std::vector<char *> argv;
    uint64_t tick_count = 0;
};

static HostState g_host;

static void host_load_lib(const std::string &libpath) {
    fprintf(stderr, "libgodot_host: loading %s\n", libpath.c_str());
    g_host.lib_handle = dlopen(libpath.c_str(), RTLD_LAZY);
    if (!g_host.lib_handle) die("dlopen");
    g_host.create = (libgodot_create_fn)dlsym(g_host.lib_handle, "libgodot_create_godot_instance");
    if (!g_host.create) die("dlsym create");
    g_host.destroy = (libgodot_destroy_fn)dlsym(g_host.lib_handle, "libgodot_destroy_godot_instance");
    if (!g_host.destroy) die("dlsym destroy");
}

// Build godot argv from three fields the outer caller controls (arg0,
// optional -s script, optional --path project); ownership stays in
// g_host.argv_storage so godot's Main::setup can hold the pointers.
static void host_build_argv(const char *arg0, const std::string &script, const std::string &project) {
    g_host.argv_storage.clear();
    g_host.argv.clear();
    g_host.argv_storage.emplace_back(arg0);
    g_host.argv_storage.emplace_back("--headless");
    if (!script.empty()) {
        g_host.argv_storage.emplace_back("-s");
        g_host.argv_storage.emplace_back(script);
    }
    if (!project.empty()) {
        g_host.argv_storage.emplace_back("--path");
        g_host.argv_storage.emplace_back(project);
    }
    for (auto &s : g_host.argv_storage) g_host.argv.push_back(s.data());
}

// Boot godot on the current (main) thread. Returns true on success.
static bool host_create_instance() {
    if (g_host.obj) return true; // idempotent
    fprintf(stderr, "libgodot_host: libgodot_create_godot_instance on main thread\n");
    g_host.obj = g_host.create((int)g_host.argv.size(), g_host.argv.data(), host_gdextension_init);
    if (!g_host.obj) {
        fprintf(stderr, "libgodot_host: create returned nullptr (Main::setup failed?)\n");
        return false;
    }
    g_host.instance = reinterpret_cast<godot::GodotInstance *>(
            godot::internal::get_object_instance_binding(g_host.obj));
    fprintf(stderr, "libgodot_host: GodotInstance ptr %p\n", (void *)g_host.instance);
    return true;
}

static void host_destroy_instance() {
    if (!g_host.obj) return;
    if (g_host.instance) g_host.instance->stop();
    g_host.destroy(g_host.obj);
    g_host.obj = nullptr;
    g_host.instance = nullptr;
    g_host.tick_count = 0;
}

int main(int argc, char *argv[]) {
    bool smoke = false;
    int max_iterations = 8;
    std::string libpath;
    std::string script_path;
    std::string project_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--smoke") smoke = true;
        else if (a == "--libgodot" && i + 1 < argc) libpath = argv[++i];
        else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
        else if (a == "--project" && i + 1 < argc) project_path = argv[++i];
        else if (a == "--max-iterations" && i + 1 < argc) max_iterations = atoi(argv[++i]);
    }
    if (libpath.empty()) libpath = resolve_libgodot_path(argv[0]);

    host_load_lib(libpath);

    if (smoke) {
        // One-shot: build argv from CLI, create/start/iterate/stop/destroy, exit.
        host_build_argv(argv[0], script_path, project_path);
        if (!host_create_instance()) return 3;
        if (!g_host.instance->start()) {
            fprintf(stderr, "libgodot_host: start() returned false\n");
            host_destroy_instance();
            return 4;
        }
        fprintf(stderr, "libgodot_host: started\n");
        int ticks = 0;
        while (ticks < max_iterations) {
            bool quit = g_host.instance->iteration();
            ticks++;
            if (quit) {
                fprintf(stderr, "libgodot_host: iteration %d requested quit\n", ticks);
                break;
            }
        }
        fprintf(stderr, "libgodot_host: ran %d iteration(s)\n", ticks);
        host_destroy_instance();
        fprintf(stderr, "libgodot_host: smoke OK\n");
        return 0;
    }

#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)
    // Bus mode: open the lifecycle service and dispatch opcodes until QUIT.
    fprintf(stderr, "libgodot_host: entering weft::run_command_loop on service '%s'\n",
            weft::COMMAND_SERVICE_NAME);
    // The bus loop drives godot::iteration() only when elixir asks (ITERATE
    // opcode). This puts the tick rate in the manager's hands and keeps the
    // process idle when no one is asking, which is what a lifecycle-managed
    // host should do.
    //
    // The Ask handler stashes prep args from --script/--project as the
    // default for CREATE when the payload is empty; a real client sends
    // its own argv over the wire.
    struct AskCtx {
        std::string default_script;
        std::string default_project;
        const char *arg0;
    };
    AskCtx ctx{script_path, project_path, argv[0]};

    auto ask = [](void *ctx_v, const char *command, size_t len,
                  unsigned char *reply, size_t cap, int *stop) -> size_t {
        auto *ac = static_cast<AskCtx *>(ctx_v);
        if (len == 0 || cap < 1) return 0;
        const uint8_t opcode = static_cast<uint8_t>(command[0]);
        const char *body = command + 1;
        const size_t body_len = len - 1;

        auto write_ok = [&](size_t extra) -> size_t {
            reply[0] = 0x00; // ok
            return 1 + extra;
        };
        auto write_err = [&](const char *msg) -> size_t {
            reply[0] = 0x01; // err
            const size_t n = std::strlen(msg);
            const size_t room = cap - 1;
            const size_t copy = n < room ? n : room;
            std::memcpy(reply + 1, msg, copy);
            return 1 + copy;
        };

        switch (opcode) {
        case 0x01: { // CREATE — body is NUL-separated argv
            if (g_host.obj) return write_err("already_created");
            // Parse body into g_host argv_storage; fall back to CLI defaults if empty.
            g_host.argv_storage.clear();
            g_host.argv.clear();
            g_host.argv_storage.emplace_back(ac->arg0);
            if (body_len > 0) {
                const char *p = body;
                const char *end = body + body_len;
                while (p < end) {
                    const char *nul = static_cast<const char *>(std::memchr(p, '\0', end - p));
                    size_t seg = nul ? (size_t)(nul - p) : (size_t)(end - p);
                    g_host.argv_storage.emplace_back(p, seg);
                    p = nul ? nul + 1 : end;
                }
            } else {
                g_host.argv_storage.emplace_back("--headless");
                if (!ac->default_script.empty()) {
                    g_host.argv_storage.emplace_back("-s");
                    g_host.argv_storage.emplace_back(ac->default_script);
                }
                if (!ac->default_project.empty()) {
                    g_host.argv_storage.emplace_back("--path");
                    g_host.argv_storage.emplace_back(ac->default_project);
                }
            }
            for (auto &s : g_host.argv_storage) g_host.argv.push_back(s.data());
            if (!host_create_instance()) return write_err("create_failed");
            return write_ok(0);
        }
        case 0x02: { // START
            if (!g_host.instance) return write_err("no_instance");
            if (!g_host.instance->start()) return write_err("start_failed");
            return write_ok(0);
        }
        case 0x03: { // ITERATE — reply body: [quit:u8][tick:u64 LE]
            if (!g_host.instance) return write_err("no_instance");
            if (cap < 1 + 1 + 8) return write_err("cap_too_small");
            bool quit = g_host.instance->iteration();
            g_host.tick_count++;
            reply[0] = 0x00;
            reply[1] = quit ? 1 : 0;
            std::memcpy(reply + 2, &g_host.tick_count, 8);
            return 1 + 1 + 8;
        }
        case 0x04: { // STOP
            if (g_host.instance) g_host.instance->stop();
            return write_ok(0);
        }
        case 0x05: { // DESTROY (idempotent)
            host_destroy_instance();
            return write_ok(0);
        }
        case 0x7F: { // QUIT — tear down and end the loop
            host_destroy_instance();
            *stop = 1;
            return write_ok(0);
        }
        default:
            return write_err("unknown_opcode");
        }
    };

    int rc = weft::run_command_loop(&ctx, ask);
    fprintf(stderr, "libgodot_host: run_command_loop returned %d\n", rc);
    return rc;
#else
    fprintf(stderr, "libgodot_host: built without LIBGODOT_HOST_HAS_WEFT_HARNESS; "
                    "either build against thirdparty/weft-harness/ or pass --smoke\n");
    return 5;
#endif
}
