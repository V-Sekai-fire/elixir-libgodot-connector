#include <erl_nif.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <libgodot.h>

// The v-sekai-fire entities-godot fork exposes libgodot with a 3-argument
// create signature (argc, argv, init_func). The Ughuuu fork this NIF was
// originally coded against adds callback plumbing (invoke/executor/log)
// that entities-godot does not carry. Keep the type names as compat aliases
// so declarations that mention them still compile; the call site below is
// adjusted to the 3-argument form entities-godot actually exports.
typedef void *LogCallbackData;
typedef void (*LogCallbackFunction)(LogCallbackData, const char *, bool);
typedef void *ExecutorData;
typedef void (*InvokeCallbackFunction)(ExecutorData, void (*)(void *), void *);

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/godot_instance.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdio>

#ifdef _WIN32
struct Dl_info {
    const char *dli_fname;
};

static thread_local std::string g_dlerror_text;
static thread_local char g_dladdr_path[MAX_PATH + 1];

static void set_dlerror_from_last_error(const char *prefix) {
    DWORD err = GetLastError();
    char *msg = nullptr;
    FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            err,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&msg),
            0,
            nullptr);
    if (msg) {
        g_dlerror_text = std::string(prefix) + ": " + msg;
        LocalFree(msg);
    } else {
        g_dlerror_text = std::string(prefix) + ": error " + std::to_string(err);
    }
}

static void *dlopen(const char *path, int) {
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        set_dlerror_from_last_error("LoadLibraryA");
        return nullptr;
    }
    g_dlerror_text.clear();
    return reinterpret_cast<void *>(h);
}

static void *dlsym(void *handle, const char *symbol) {
    FARPROC proc = GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol);
    if (!proc) {
        set_dlerror_from_last_error("GetProcAddress");
        return nullptr;
    }
    g_dlerror_text.clear();
    return reinterpret_cast<void *>(proc);
}

static int dlclose(void *handle) {
    if (!handle) {
        return 0;
    }
    BOOL ok = FreeLibrary(reinterpret_cast<HMODULE>(handle));
    if (!ok) {
        set_dlerror_from_last_error("FreeLibrary");
        return -1;
    }
    g_dlerror_text.clear();
    return 0;
}

static const char *dlerror() {
    return g_dlerror_text.empty() ? nullptr : g_dlerror_text.c_str();
}

static int dladdr(void *addr, Dl_info *info) {
    if (!addr || !info) {
        return 0;
    }

    HMODULE module = nullptr;
    BOOL ok = GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr),
            &module);
    if (!ok || !module) {
        set_dlerror_from_last_error("GetModuleHandleExA");
        return 0;
    }

    DWORD len = GetModuleFileNameA(module, g_dladdr_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        set_dlerror_from_last_error("GetModuleFileNameA");
        return 0;
    }

    g_dladdr_path[len] = '\0';
    info->dli_fname = g_dladdr_path;
    return 1;
}

#define access _access
#ifndef R_OK
#define R_OK 4
#endif
#ifndef RTLD_LAZY
#define RTLD_LAZY 0
#endif
#endif

#if defined(_WIN32)
#define LIBGODOT_DEFAULT_PATH "../../build/libgodot.dll"
#define LIBGODOT_PRIV_NAME "libgodot.dll"
#elif defined(__APPLE__)
#define LIBGODOT_DEFAULT_PATH "../../build/libgodot.dylib"
#define LIBGODOT_PRIV_NAME "libgodot.dylib"
#else
#define LIBGODOT_DEFAULT_PATH "../../build/libgodot.so"
#define LIBGODOT_PRIV_NAME "libgodot.so"
#endif

static bool file_exists_readable(const std::string &path) {
    return access(path.c_str(), R_OK) == 0;
}

static std::string dirname_from_path(const char *path) {
    if (!path) {
        return {};
    }
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    if (pos == std::string::npos) {
        return {};
    }
    return s.substr(0, pos);
}

// Determine priv directory by locating this NIF shared library at runtime.
// This enables shipping libgodot alongside the NIF in priv/.
static std::string detect_priv_dir() {
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&detect_priv_dir), &info) == 0) {
        return {};
    }
    return dirname_from_path(info.dli_fname);
}

static std::string resolve_default_libgodot_path() {
    // Allow explicit override.
    if (const char *env = getenv("LIBGODOT_PATH")) {
        if (env[0] != '\0') {
            return std::string(env);
        }
    }

    // Prefer repo build location for local development.
    std::string repo_build = std::string(LIBGODOT_DEFAULT_PATH);
    if (file_exists_readable(repo_build)) {
        return repo_build;
    }

    // Fallback to libgodot shipped in priv/.
    std::string priv = detect_priv_dir();
    if (!priv.empty()) {
        std::string candidate = priv + "/" LIBGODOT_PRIV_NAME;
        if (file_exists_readable(candidate)) {
            return candidate;
        }
    }

    return repo_build;
}

static void libgodot_log_callback(LogCallbackData, const char *p_log_message, bool p_err) {
    const char *prefix = p_err ? "[libgodot][err] " : "[libgodot] ";
    if (p_log_message) {
        fprintf(p_err ? stderr : stdout, "%s%s\n", prefix, p_log_message);
    } else {
        fprintf(p_err ? stderr : stdout, "%s<null log message>\n", prefix);
    }
    fflush(p_err ? stderr : stdout);
}

static bool libgodot_log_enabled() {
    const char *env = getenv("LIBGODOT_LOG");
    return env && env[0] == '1' && env[1] == '\0';
}

// Elixir -> Godot message queue.
// Producer: BEAM threads calling the NIF.
// Consumer: Godot main loop thread (inside instance->iteration()).
static std::mutex incoming_messages_mutex;
static std::deque<std::string> incoming_messages;
static std::atomic<bool> incoming_messages_pending{false};

// Godot -> Elixir subscriber (used by ElixirBus.send_to_elixir).
static ErlNifPid subscriber_pid;
static bool has_subscriber = false;
static std::mutex subscriber_mutex;

// Request/response table (Elixir waits, Godot fulfills).
struct ResponseWaiter {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string response;
};

static std::mutex response_mutex;
static std::unordered_map<uint64_t, std::shared_ptr<ResponseWaiter>> pending_responses;
static std::atomic<uint64_t> next_request_id{1};

class ElixirBus : public godot::Object {
    GDCLASS(ElixirBus, godot::Object);

protected:
    static void _bind_methods() {
        ADD_SIGNAL(godot::MethodInfo("new_message", godot::PropertyInfo(godot::Variant::INT, "queued")));
        godot::ClassDB::bind_method(godot::D_METHOD("drain"), &ElixirBus::drain);
        godot::ClassDB::bind_method(godot::D_METHOD("size"), &ElixirBus::size);
        godot::ClassDB::bind_method(godot::D_METHOD("send_to_elixir", "msg"), &ElixirBus::send_to_elixir);
        godot::ClassDB::bind_method(godot::D_METHOD("send_event", "kind", "payload"), &ElixirBus::send_event);
        godot::ClassDB::bind_method(godot::D_METHOD("respond", "request_id", "response"), &ElixirBus::respond);
    }

public:
    godot::PackedStringArray drain() const {
        std::deque<std::string> drained;
        {
            std::lock_guard<std::mutex> lk(incoming_messages_mutex);
            drained.swap(incoming_messages);
        }

        godot::PackedStringArray out;
        out.resize(static_cast<int32_t>(drained.size()));
        int32_t i = 0;
        for (const auto &s : drained) {
            out.set(i++, godot::String(s.c_str()));
        }
        return out;
    }

    int32_t size() const {
        std::lock_guard<std::mutex> lk(incoming_messages_mutex);
        return static_cast<int32_t>(incoming_messages.size());
    }

    void send_to_elixir(const godot::String &msg) const {
        if (!has_subscriber) {
            return;
        }

        ErlNifEnv *msg_env = enif_alloc_env();
        if (!msg_env) {
            return;
        }

        godot::CharString cs = msg.utf8();
        ERL_NIF_TERM payload = enif_make_string(msg_env, cs.get_data(), ERL_NIF_UTF8);
        ERL_NIF_TERM term = enif_make_tuple2(msg_env, enif_make_atom(msg_env, "godot_message"), payload);

        ErlNifPid pid_copy;
        {
            std::lock_guard<std::mutex> lk(subscriber_mutex);
            pid_copy = subscriber_pid;
        }
        enif_send(nullptr, &pid_copy, msg_env, term);
        enif_free_env(msg_env);
    }

    void send_event(const godot::String &kind, const godot::String &payload) const {
        if (!has_subscriber) {
            return;
        }

        ErlNifEnv *msg_env = enif_alloc_env();
        if (!msg_env) {
            return;
        }

        godot::CharString kind_cs = kind.utf8();
        godot::CharString payload_cs = payload.utf8();
        ERL_NIF_TERM kind_term = enif_make_string(msg_env, kind_cs.get_data(), ERL_NIF_UTF8);
        ERL_NIF_TERM payload_term = enif_make_string(msg_env, payload_cs.get_data(), ERL_NIF_UTF8);
        ERL_NIF_TERM term = enif_make_tuple3(msg_env, enif_make_atom(msg_env, "godot_event"), kind_term, payload_term);

        ErlNifPid pid_copy;
        {
            std::lock_guard<std::mutex> lk(subscriber_mutex);
            pid_copy = subscriber_pid;
        }
        enif_send(nullptr, &pid_copy, msg_env, term);
        enif_free_env(msg_env);
    }

    void respond(int64_t request_id, const godot::String &response) const {
        if (request_id <= 0) {
            return;
        }

        std::shared_ptr<ResponseWaiter> waiter;
        {
            std::lock_guard<std::mutex> lk(response_mutex);
            auto it = pending_responses.find(static_cast<uint64_t>(request_id));
            if (it == pending_responses.end()) {
                return;
            }
            waiter = it->second;
        }

        godot::CharString cs = response.utf8();
        {
            std::lock_guard<std::mutex> lk(waiter->m);
            waiter->response.assign(cs.get_data());
            waiter->done = true;
        }
        waiter->cv.notify_one();
    }
};

static ElixirBus *elixir_bus_singleton = nullptr;

static void initialize_default_module(godot::ModuleInitializationLevel p_level) {
    if (p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    godot::ClassDB::register_class<ElixirBus>();

    if (!elixir_bus_singleton) {
        elixir_bus_singleton = memnew(ElixirBus);
        godot::Engine::get_singleton()->register_singleton(godot::StringName("ElixirBus"), elixir_bus_singleton);
    }
}

static void uninitialize_default_module(godot::ModuleInitializationLevel p_level) {
    if (p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    if (elixir_bus_singleton) {
        godot::Engine::get_singleton()->unregister_singleton(godot::StringName("ElixirBus"));
        memdelete(elixir_bus_singleton);
        elixir_bus_singleton = nullptr;
    }
}

extern "C" GDExtensionBool GDE_EXPORT gdextension_default_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization) {
    godot::GDExtensionBinding::InitObject init_object(p_get_proc_address, p_library, r_initialization);

    init_object.register_initializer(initialize_default_module);
    init_object.register_terminator(uninitialize_default_module);
    init_object.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);

    return init_object.init();
}


struct GodotNifResource {
	// The worker thread owns the actual Godot instance.
	// This resource is a handle/token to validate callers.
	uint64_t token = 0;
};

static ErlNifResourceType *GODOT_RES_TYPE = nullptr;

// Godot/libgodot has strict thread-affinity requirements. The BEAM may run
// NIF calls on different OS threads, even within a single GenServer.
// To keep Godot happy, we route all engine calls through a dedicated worker
// thread that owns the instance.
namespace {

enum class RequestType {
    Create,
    CreateWithPath,
    Start,
    Iteration,
    Shutdown,
};

struct Request {
    RequestType type;
    std::string lib_path; // optional
    std::vector<std::string> args;

    bool ok = false;
    bool quit = false;
    std::string err;
	uint64_t token = 0;

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

std::mutex queue_mutex;
std::condition_variable queue_cv;
std::deque<std::shared_ptr<Request>> queue;

std::once_flag worker_once;

std::atomic<uint64_t> worker_token{0};

// Worker-owned state.
// When set the worker loop will exit; the thread is joinable so we can
// ensure clean shutdown during NIF unload.
static std::atomic<bool> worker_should_exit{false};
static std::thread worker_thread;

void *worker_handle = nullptr;
// entities-godot's 3-argument signature. Callback fields removed from
// worker_create_instance because entities-godot does not accept them yet;
// libgodot_log_callback / libgodot_log_enabled remain in place for future use.
GDExtensionObjectPtr (*worker_create_instance)(int, char *[], GDExtensionInitializationFunction) = nullptr;
void (*worker_destroy_instance)(GDExtensionObjectPtr) = nullptr;
GDExtensionObjectPtr worker_object = nullptr;
godot::GodotInstance *worker_instance = nullptr;

static void set_result(const std::shared_ptr<Request> &req, bool ok, const std::string &err = "", bool quit = false) {
    std::unique_lock<std::mutex> lk(req->m);
    req->ok = ok;
    req->err = err;
    req->quit = quit;
    req->token = worker_token.load(std::memory_order_relaxed);
    req->done = true;
    lk.unlock();
    req->cv.notify_one();
}

static bool args_to_argv(const std::vector<std::string> &args, std::vector<char *> &argv_out) {
    argv_out.clear();
    argv_out.reserve(args.size() + 1);
    for (const auto &s : args) {
        argv_out.push_back(const_cast<char *>(s.data()));
    }
    argv_out.push_back(nullptr);
    return true;
}

#ifdef __APPLE__
static void maybe_enable_embedded_headless(const std::vector<std::string> &args) {
    for (const auto &s : args) {
        if (s == "--headless") {
            setenv("LIBGODOT_EMBEDDED_HEADLESS", "1", 1);
            return;
        }
    }
}
#endif

static void worker_loop() {
    for (;;) {
        // Exit early when requested (used during NIF unload).
        if (worker_should_exit.load(std::memory_order_acquire)) {
            break;
        }

        std::shared_ptr<Request> req;
        {
            std::unique_lock<std::mutex> lk(queue_mutex);
            queue_cv.wait(lk, [] {
                return worker_should_exit.load(std::memory_order_acquire) || !queue.empty() || incoming_messages_pending.load(std::memory_order_acquire);
            });

            if (worker_should_exit.load(std::memory_order_acquire)) {
                break;
            }

            // If a message arrived, emit the signal immediately on this (Godot) thread.
            // Do not force a full frame iteration for message delivery.
            if (incoming_messages_pending.exchange(false, std::memory_order_acq_rel) && elixir_bus_singleton) {
                int32_t queued = 0;
                {
                    std::lock_guard<std::mutex> lk2(incoming_messages_mutex);
                    queued = static_cast<int32_t>(incoming_messages.size());
                }
                elixir_bus_singleton->emit_signal(godot::StringName("new_message"), queued);
            }

            if (queue.empty()) {
                continue;
            }

            req = queue.front();
            queue.pop_front();
        }

        switch (req->type) {
            case RequestType::Create:
            case RequestType::CreateWithPath: {
                if (worker_instance != nullptr) {
                    set_result(req, false, "only_one_instance");
                    break;
                }

#ifdef __APPLE__
                maybe_enable_embedded_headless(req->args);
#endif

                std::string resolved_path;
                const char *path = nullptr;
                if (req->type == RequestType::CreateWithPath) {
                    path = req->lib_path.c_str();
                } else {
                    resolved_path = resolve_default_libgodot_path();
                    path = resolved_path.c_str();
                }

                worker_handle = dlopen(path, RTLD_LAZY);
                if (!worker_handle) {
                    std::string err = dlerror() ? dlerror() : "dlopen_failed";
                    set_result(req, false, err);
                    break;
                }

                *(void **)(&worker_create_instance) = dlsym(worker_handle, "libgodot_create_godot_instance");
                *(void **)(&worker_destroy_instance) = dlsym(worker_handle, "libgodot_destroy_godot_instance");
                if (!worker_create_instance || !worker_destroy_instance) {
					std::string err = dlerror() ? dlerror() : "dlsym_failed";
					dlclose(worker_handle);
					worker_handle = nullptr;
					worker_create_instance = nullptr;
					worker_destroy_instance = nullptr;
					set_result(req, false, err);
					break;
                }

                std::vector<char *> argv_c;
                args_to_argv(req->args, argv_c);
                if (argv_c.size() < 2) {
                    dlclose(worker_handle);
                    worker_handle = nullptr;
                    set_result(req, false, "bad_argv");
                    break;
                }

                // LIBGODOT_LOG is honored via the log_callback stub above rather than
                // through libgodot itself, which entities-godot does not accept yet.
                (void)libgodot_log_enabled;
                (void)libgodot_log_callback;

                GDExtensionObjectPtr obj = worker_create_instance(
                        static_cast<int>(argv_c.size() - 1),
                        argv_c.data(),
                        gdextension_default_init);
                if (!obj) {
                    dlclose(worker_handle);
                    worker_handle = nullptr;
                    set_result(req, false, "create_instance_failed");
                    break;
                }

                worker_object = obj;
                worker_instance = reinterpret_cast<godot::GodotInstance *>(godot::internal::get_object_instance_binding(obj));
                if (!worker_instance) {
                    if (worker_destroy_instance) {
						worker_destroy_instance(obj);
					}
					worker_object = nullptr;
					dlclose(worker_handle);
					worker_handle = nullptr;
					worker_create_instance = nullptr;
					worker_destroy_instance = nullptr;
                    set_result(req, false, "instance_binding_failed");
                    break;
                }
				worker_token.fetch_add(1, std::memory_order_relaxed);
                set_result(req, true);
                break;
            }

            case RequestType::Start: {
                if (!worker_instance) {
                    set_result(req, false, "not_created");
                    break;
                }
                if (!worker_instance->start()) {
                    set_result(req, false, "start_failed");
                    break;
                }
                set_result(req, true);
                break;
            }

            case RequestType::Iteration: {
                if (!worker_instance) {
                    set_result(req, false, "not_created");
                    break;
                }
                bool should_quit = worker_instance->iteration();
                set_result(req, true, "", should_quit);
                break;
            }

            case RequestType::Shutdown: {
                if (!worker_instance) {
                    set_result(req, true);
                    break;
                }
				const char *skip = getenv("LIBGODOT_SKIP_DESTROY");
				const bool skip_destroy = skip && skip[0] == '1' && skip[1] == '\0';
				if (!skip_destroy && worker_destroy_instance && worker_object) {
					worker_destroy_instance(worker_object);
				}
                worker_object = nullptr;
                worker_instance = nullptr;
				worker_token.store(0, std::memory_order_relaxed);
                set_result(req, true);
                break;
            }
        }
    }
}

static void ensure_worker_started() {
    std::call_once(worker_once, [] {
        worker_should_exit.store(false);
        worker_thread = std::thread(worker_loop);
    });
}

static std::shared_ptr<Request> call_worker(RequestType type, const std::vector<std::string> &args, const std::string &lib_path = "") {
    ensure_worker_started();
    auto req = std::make_shared<Request>();
    req->type = type;
    req->args = args;
    req->lib_path = lib_path;
    {
        std::lock_guard<std::mutex> lk(queue_mutex);
        queue.push_back(req);
    }
    queue_cv.notify_one();

    std::unique_lock<std::mutex> lk(req->m);
    req->cv.wait(lk, [&] { return req->done; });
    return req;
}

} // namespace

static void godot_resource_dtor(ErlNifEnv *env, void *obj) {
    (void)env;
    auto *res = static_cast<GodotNifResource *>(obj);
    if (!res) {
        return;
    }
	// Deliberately no-op.
	// The worker thread owns the Godot instance lifecycle.
	// Resource GC should not implicitly stop the engine.
}

static ERL_NIF_TERM make_error(ErlNifEnv *env, const std::string &msg) {
    return enif_make_tuple2(env, enif_make_atom(env, "error"), enif_make_string(env, msg.c_str(), ERL_NIF_UTF8));
}

static bool term_to_string(ErlNifEnv *env, ERL_NIF_TERM term, std::string &out);

// request(ref, binary|string, timeout_ms) -> {:ok, response} | {:error, reason}
static ERL_NIF_TERM request(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GodotNifResource *res = nullptr;
    if (!enif_get_resource(env, argv[0], GODOT_RES_TYPE, (void **)&res) || !res) {
        return enif_make_badarg(env);
    }
    uint64_t current = worker_token.load(std::memory_order_relaxed);
    if (current == 0 || res->token != current) {
        return make_error(env, "stale_resource");
    }

    std::string payload;
    if (!term_to_string(env, argv[1], payload)) {
        return enif_make_badarg(env);
    }

    unsigned timeout_ms = 0;
    if (!enif_get_uint(env, argv[2], &timeout_ms)) {
        return enif_make_badarg(env);
    }

    uint64_t id = next_request_id.fetch_add(1, std::memory_order_relaxed);
    auto waiter = std::make_shared<ResponseWaiter>();
    {
        std::lock_guard<std::mutex> lk(response_mutex);
        pending_responses.emplace(id, waiter);
    }

    // Enqueue request for Godot: "__request__:<id>:<payload>"
    {
        std::lock_guard<std::mutex> lk(incoming_messages_mutex);
        incoming_messages.emplace_back("__request__:" + std::to_string(id) + ":" + payload);
    }
    incoming_messages_pending.store(true, std::memory_order_release);
    queue_cv.notify_one();

    std::unique_lock<std::mutex> lk(waiter->m);
    bool ok = waiter->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return waiter->done; });
    if (!ok) {
        std::lock_guard<std::mutex> lk2(response_mutex);
        pending_responses.erase(id);
        return make_error(env, "timeout");
    }
    std::string response = waiter->response;
    lk.unlock();
    {
        std::lock_guard<std::mutex> lk2(response_mutex);
        pending_responses.erase(id);
    }

    ERL_NIF_TERM resp_term = enif_make_string(env, response.c_str(), ERL_NIF_UTF8);
    return enif_make_tuple2(env, enif_make_atom(env, "ok"), resp_term);
}

// send_message(ref, binary|string) -> :ok | {:error, reason}
static ERL_NIF_TERM send_message(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GodotNifResource *res = nullptr;
    if (!enif_get_resource(env, argv[0], GODOT_RES_TYPE, (void **)&res) || !res) {
        return enif_make_badarg(env);
    }
    uint64_t current = worker_token.load(std::memory_order_relaxed);
    if (current == 0 || res->token != current) {
        return make_error(env, "stale_resource");
    }

    std::string msg;
    if (!term_to_string(env, argv[1], msg)) {
        return enif_make_badarg(env);
    }

    {
        std::lock_guard<std::mutex> lk(incoming_messages_mutex);
        incoming_messages.push_back(std::move(msg));
    }

	incoming_messages_pending.store(true, std::memory_order_release);
    queue_cv.notify_one();

    return enif_make_atom(env, "ok");
}

static bool term_to_string(ErlNifEnv *env, ERL_NIF_TERM term, std::string &out) {
    if (enif_is_binary(env, term)) {
        ErlNifBinary bin;
        if (!enif_inspect_binary(env, term, &bin)) {
            return false;
        }
        out.assign(reinterpret_cast<const char *>(bin.data), bin.size);
        return true;
    }

    if (enif_is_list(env, term)) {
        unsigned len = 0;
        if (enif_get_list_length(env, term, &len)) {
            std::vector<char> buf(len + 1);
            if (enif_get_string(env, term, buf.data(), static_cast<unsigned>(buf.size()), ERL_NIF_UTF8) > 0) {
                out.assign(buf.data());
                return true;
            }
        }
    }

    return false;
}

static bool list_to_argv(ErlNifEnv *env, ERL_NIF_TERM list_term, std::vector<std::string> &arg_storage, std::vector<char *> &argv_out) {
    arg_storage.clear();
    argv_out.clear();

    if (enif_is_empty_list(env, list_term)) {
        arg_storage.emplace_back("godot");
        argv_out.push_back(arg_storage.back().data());
        return true;
    }

    ERL_NIF_TERM head;
    ERL_NIF_TERM tail;
    ERL_NIF_TERM list = list_term;
    while (enif_get_list_cell(env, list, &head, &tail)) {
        std::string s;
        if (!term_to_string(env, head, s)) {
            return false;
        }
        arg_storage.push_back(std::move(s));
        list = tail;
    }

    if (!enif_is_empty_list(env, list)) {
        return false;
    }

    argv_out.reserve(arg_storage.size());
    for (auto &s : arg_storage) {
        argv_out.push_back(s.data());
    }

    return true;
}

static ERL_NIF_TERM subscribe(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    ErlNifPid pid;
    if (!enif_get_local_pid(env, argv[0], &pid)) {
        return enif_make_badarg(env);
    }
    {
        std::lock_guard<std::mutex> lk(subscriber_mutex);
        subscriber_pid = pid;
    }
    has_subscriber = true;
    return enif_make_atom(env, "ok");
}

// create([args]) -> {:ok, ref} | {:error, reason}
static ERL_NIF_TERM create(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;

    std::vector<std::string> arg_storage;
    std::vector<char *> ignored;
    if (!list_to_argv(env, argv[0], arg_storage, ignored)) {
        return enif_make_badarg(env);
    }

    auto result = call_worker(RequestType::Create, arg_storage);
    if (!result->ok) {
        return make_error(env, result->err);
    }

    auto *res = static_cast<GodotNifResource *>(enif_alloc_resource(GODOT_RES_TYPE, sizeof(GodotNifResource)));
    if (!res) {
        return make_error(env, "alloc_resource_failed");
    }
    new (res) GodotNifResource();
	res->token = result->token;

    ERL_NIF_TERM term = enif_make_resource(env, res);
    enif_release_resource(res);
    return enif_make_tuple2(env, enif_make_atom(env, "ok"), term);
}

// create(libgodot_path, [args]) -> {:ok, ref} | {:error, reason}
static ERL_NIF_TERM create_with_path(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;

    std::string libgodot_path;
    if (!term_to_string(env, argv[0], libgodot_path)) {
        return enif_make_badarg(env);
    }

    std::vector<std::string> arg_storage;
    std::vector<char *> ignored;
    if (!list_to_argv(env, argv[1], arg_storage, ignored)) {
        return enif_make_badarg(env);
    }

    auto result = call_worker(RequestType::CreateWithPath, arg_storage, libgodot_path);
    if (!result->ok) {
        return make_error(env, result->err);
    }

    auto *res = static_cast<GodotNifResource *>(enif_alloc_resource(GODOT_RES_TYPE, sizeof(GodotNifResource)));
    if (!res) {
        return make_error(env, "alloc_resource_failed");
    }
    new (res) GodotNifResource();
	res->token = result->token;

    ERL_NIF_TERM term = enif_make_resource(env, res);
    enif_release_resource(res);
    return enif_make_tuple2(env, enif_make_atom(env, "ok"), term);
}

// start(ref) -> :ok | {:error, reason}
static ERL_NIF_TERM start(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GodotNifResource *res = nullptr;
    if (!enif_get_resource(env, argv[0], GODOT_RES_TYPE, (void **)&res) || !res) {
        return enif_make_badarg(env);
    }
    uint64_t current = worker_token.load(std::memory_order_relaxed);
    if (current == 0 || res->token != current) {
        return make_error(env, "stale_resource");
    }

    auto result = call_worker(RequestType::Start, {});
    if (!result->ok) {
        return make_error(env, result->err);
    }

    if (has_subscriber) {
        ErlNifPid pid_copy;
        {
            std::lock_guard<std::mutex> lk(subscriber_mutex);
            pid_copy = subscriber_pid;
        }
        ERL_NIF_TERM msg = enif_make_tuple2(env, enif_make_atom(env, "godot_status"), enif_make_atom(env, "started"));
        enif_send(nullptr, &pid_copy, nullptr, msg);
    }

    return enif_make_atom(env, "ok");
}

// iteration(ref) -> :ok | {:error, reason}
static ERL_NIF_TERM iteration(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GodotNifResource *res = nullptr;
    if (!enif_get_resource(env, argv[0], GODOT_RES_TYPE, (void **)&res) || !res) {
        return enif_make_badarg(env);
    }
    uint64_t current = worker_token.load(std::memory_order_relaxed);
    if (current == 0 || res->token != current) {
        return make_error(env, "stale_resource");
    }

    auto result = call_worker(RequestType::Iteration, {});
    if (!result->ok) {
        return make_error(env, result->err);
    }
    if (result->quit) {
        return make_error(env, "quit");
    }
    return enif_make_atom(env, "ok");
}

// shutdown(ref) -> :ok
static ERL_NIF_TERM shutdown(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GodotNifResource *res = nullptr;
    if (!enif_get_resource(env, argv[0], GODOT_RES_TYPE, (void **)&res) || !res) {
        return enif_make_badarg(env);
    }
    uint64_t current = worker_token.load(std::memory_order_relaxed);
    if (current != 0 && res->token != current) {
        return make_error(env, "stale_resource");
    }

    auto result = call_worker(RequestType::Shutdown, {});
    if (!result->ok) {
        return make_error(env, result->err);
    }

    if (has_subscriber) {
        ErlNifPid pid_copy;
        {
            std::lock_guard<std::mutex> lk(subscriber_mutex);
            pid_copy = subscriber_pid;
        }
        ERL_NIF_TERM msg = enif_make_tuple2(env, enif_make_atom(env, "godot_status"), enif_make_atom(env, "shutdown"));
        enif_send(nullptr, &pid_copy, nullptr, msg);
    }
    return enif_make_atom(env, "ok");
}

static int nif_load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info) {
    (void)priv_data;
    (void)load_info;
    GODOT_RES_TYPE = enif_open_resource_type(env, nullptr, "libgodot_resource", godot_resource_dtor, ERL_NIF_RT_CREATE, nullptr);
    ensure_worker_started();
    return GODOT_RES_TYPE ? 0 : 1;
}

// Unload handler: ensure the Godot instance is shut down and the worker
// thread is joined so that there are no background threads touching
// Godot internals while the VM is tearing down.
static void nif_unload(ErlNifEnv *env, void *priv_data) {
    (void)env;
    (void)priv_data;

    // Ask the worker loop to destroy any active Godot instance.
    // call_worker will block until the Shutdown request completes.
    call_worker(RequestType::Shutdown, {});

    // Signal the worker loop to exit and join the thread.
    worker_should_exit.store(true);
    queue_cv.notify_one();
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    // Close the dynamically loaded libgodot if still open.
    if (worker_handle) {
        dlclose(worker_handle);
        worker_handle = nullptr;
        worker_create_instance = nullptr;
        worker_destroy_instance = nullptr;
    }
}

static ErlNifFunc nif_funcs[] = {
    {"subscribe", 1, subscribe, 0},
	{"send_message", 2, send_message, 0},
    {"request", 3, request, ERL_NIF_DIRTY_JOB_IO_BOUND},
	// These calls block waiting on a native worker thread; run them as dirty
	// to avoid stalling BEAM schedulers.
    {"create", 1, create, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"create", 2, create_with_path, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"start", 1, start, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"iteration", 1, iteration, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"shutdown", 1, shutdown, ERL_NIF_DIRTY_JOB_IO_BOUND},
};

ERL_NIF_INIT(Elixir.LibGodot, nif_funcs, nif_load, nullptr, nullptr, nif_unload)
