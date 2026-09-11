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
//   (default)   weft::run_command_loop on the "lifecycle" service

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

// libgodot exports (see 4-entities/entities-godot/core/extension/libgodot.h).
// Only create/destroy are C-exports; start/iteration/shutdown live on the
// GodotInstance object and are reached through godot-cpp bindings.
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

    fprintf(stderr, "libgodot_host: loading %s\n", libpath.c_str());
    void *handle = dlopen(libpath.c_str(), RTLD_LAZY);
    if (!handle) die("dlopen");

    auto create = (libgodot_create_fn)dlsym(handle, "libgodot_create_godot_instance");
    if (!create) die("dlsym create");
    auto destroy = (libgodot_destroy_fn)dlsym(handle, "libgodot_destroy_godot_instance");
    if (!destroy) die("dlsym destroy");

    // argv for godot's Main::setup. --headless keeps the display server
    // minimal after platform init has completed on the main thread.
    // Either -s <script.gd> runs a MainLoop script (no project needed),
    // or --path <dir> loads a project.godot (requires libgodot compiled
    // with SCons `disable_path_overrides=no`).
    std::vector<char *> gargs;
    gargs.push_back(argv[0]);
    char headless[] = "--headless";
    char script_flag[] = "-s";
    char path_flag[] = "--path";
    gargs.push_back(headless);
    std::vector<char> script_buf;
    if (!script_path.empty()) {
        gargs.push_back(script_flag);
        script_buf.assign(script_path.begin(), script_path.end());
        script_buf.push_back('\0');
        gargs.push_back(script_buf.data());
    }
    std::vector<char> project_buf;
    if (!project_path.empty()) {
        gargs.push_back(path_flag);
        project_buf.assign(project_path.begin(), project_path.end());
        project_buf.push_back('\0');
        gargs.push_back(project_buf.data());
    }

    fprintf(stderr, "libgodot_host: libgodot_create_godot_instance on main thread\n");
    GDExtensionObjectPtr obj = create((int)gargs.size(), gargs.data(), host_gdextension_init);
    if (!obj) {
        fprintf(stderr, "libgodot_host: create returned nullptr (Main::setup failed?)\n");
        return 3;
    }

    // godot-cpp holds the binding for the returned GodotInstance under
    // an internal token; get_object_instance_binding hands us the typed
    // pointer we can call start()/iteration()/shutdown() on.
    auto *instance = reinterpret_cast<godot::GodotInstance *>(
            godot::internal::get_object_instance_binding(obj));
    fprintf(stderr, "libgodot_host: GodotInstance ptr %p\n", (void *)instance);

    if (!instance->start()) {
        fprintf(stderr, "libgodot_host: start() returned false\n");
        destroy(obj);
        return 4;
    }
    fprintf(stderr, "libgodot_host: started\n");

    // Engine lifecycle: iterate until GodotInstance signals it is done
    // (a MainLoop that returns true from _process, or --quit-after N).
    int ticks = 0;
    while (ticks < max_iterations) {
        bool quit = instance->iteration();
        ticks++;
        if (quit) {
            fprintf(stderr, "libgodot_host: iteration %d requested quit\n", ticks);
            break;
        }
    }
    fprintf(stderr, "libgodot_host: ran %d iteration(s)\n", ticks);

    // stop() ends the engine main loop; destroy() (the C export) frees the
    // underlying OS/engine state. Both are safe on the host's main thread.
    instance->stop();
    destroy(obj);
    fprintf(stderr, "libgodot_host: %s OK\n", smoke ? "smoke" : "run");
    return 0;
}

// --- weft::harness lifecycle loop (default mode) ---------------------------
//
// The host embeds weft::run_command_loop on a well-known service name
// (LIBGODOT_HOST_LIFECYCLE_SERVICE) so any weft-harness client, elixir
// included, can send byte-slice commands. The Ask handler decodes a
// small opcode header (see 2-contract/bus/include/weft/command.hpp for
// the shared shape) and dispatches to create/start/iterate/stop/destroy
// on the GodotInstance already spinning on this thread.
//
// Commands (opcode + payload):
//   CREATE   argv terms                  -> ok | err(reason)
//   START    -                           -> ok | err
//   ITERATE  -                           -> ok(quit:bool, tick:u64)
//   STOP     -                           -> ok
//   QUIT     -                           -> ok, then loop.stop
//
// Framing lives in weft::command; opcodes and reply codes are defined
// there so publisher and subscriber pin the same bytes. Not wired in
// this commit — the smoke path above proves the engine lifecycle runs
// on the main thread; the loop connection is the next commit.
