#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <memory>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <iomanip>
#include <signal.h>
#include <errno.h>

// Simple port that launches godot executable as subprocess
struct GodotProcess {
    pid_t pid = 0;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    std::string ref;

    ~GodotProcess() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
        if (stdin_fd >= 0) close(stdin_fd);
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
    }
};

static std::string generate_ref() {
    static uint64_t counter = 0;
    return "godot_" + std::to_string(++counter);
}

static std::map<std::string, std::unique_ptr<GodotProcess>> processes;

static void send_response(const std::string &json) {
    std::string response = json + "\n";
    write(STDOUT_FILENO, response.c_str(), response.length());
}

static std::string escape_json(const std::string &s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                } else {
                    o << c;
                }
        }
    }
    return o.str();
}

static void handle_create(const std::vector<std::string> &args, const std::string &godot_path = "") {
    // Find godot executable
    std::string godot_exe = godot_path.empty() ? args[0] : godot_path;

    // Create pipes for communication
    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        send_response("{\"ok\":false,\"error\":\"pipe_creation_failed\"}");
        return;
    }

    // Fork process
    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        send_response("{\"ok\":false,\"error\":\"fork_failed\"}");
        return;
    }

    if (pid == 0) {
        // Child process - redirect and exec
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        // Redirect stdin/stdout/stderr
        if (dup2(stdin_pipe[0], STDIN_FILENO) == -1 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(stderr_pipe[1], STDERR_FILENO) == -1) {
            _exit(1);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Prepare arguments
        std::vector<char *> argv;
        for (const auto &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(godot_exe.c_str(), argv.data());

        // If we reach here, exec failed
        _exit(1);
    }

    // Parent process: close unused pipe ends
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    std::string ref = generate_ref();
    auto process = std::make_unique<GodotProcess>();
    process->pid = pid;
    process->stdin_fd = stdin_pipe[1];
    process->stdout_fd = stdout_pipe[0];
    process->stderr_fd = stderr_pipe[0];
    process->ref = ref;

    processes[ref] = std::move(process);

    send_response("{\"ok\":true,\"ref\":\"" + ref + "\"}");
}

static void handle_start(const std::string &ref) {
    auto it = processes.find(ref);
    if (it == processes.end()) {
        send_response("{\"ok\":false,\"error\":\"invalid_ref\"}");
        return;
    }

    // For subprocess approach, godot starts automatically
    send_response("{\"ok\":true}");
}

static void handle_iteration(const std::string &ref) {
    auto it = processes.find(ref);
    if (it == processes.end()) {
        send_response("{\"ok\":false,\"error\":\"invalid_ref\"}");
        return;
    }

    // Check if process is still alive
    int status;
    pid_t result = waitpid(it->second->pid, &status, WNOHANG);

    if (result == it->second->pid) {
        // Process exited
        if (WIFEXITED(status)) {
            send_response("{\"ok\":true,\"quit\":true,\"exit_code\":" + std::to_string(WEXITSTATUS(status)) + "}");
        } else {
            send_response("{\"ok\":true,\"quit\":true,\"signal\":" + std::to_string(WTERMSIG(status)) + "}");
        }
    } else if (result == 0) {
        // Process still running
        send_response("{\"ok\":true,\"quit\":false}");
    } else {
        send_response("{\"ok\":false,\"error\":\"process_check_failed\"}");
    }
}

static void handle_send_message(const std::string &ref, const std::string &msg) {
    auto it = processes.find(ref);
    if (it == processes.end()) {
        send_response("{\"ok\":false,\"error\":\"invalid_ref\"}");
        return;
    }

    // Send message to godot process via stdin
    std::string message = "MSG:" + msg + "\n";
    ssize_t written = write(it->second->stdin_fd, message.c_str(), message.length());

    if (written < 0) {
        send_response("{\"ok\":false,\"error\":\"write_failed\"}");
    } else {
        send_response("{\"ok\":true}");
    }
}

static void handle_shutdown(const std::string &ref) {
    auto it = processes.find(ref);
    if (it == processes.end()) {
        send_response("{\"ok\":false,\"error\":\"invalid_ref\"}");
        return;
    }

    // Send quit command
    std::string quit_cmd = "QUIT\n";
    write(it->second->stdin_fd, quit_cmd.c_str(), quit_cmd.length());

    // Wait a bit then kill if still running
    sleep(1);

    if (kill(it->second->pid, 0) == 0) {
        // Process still exists, force kill
        kill(it->second->pid, SIGKILL);
        waitpid(it->second->pid, nullptr, 0);
    }

    processes.erase(it);
    send_response("{\"ok\":true}");
}

// Simple JSON parser (basic, just for this use case)
static bool parse_json(const std::string &line, std::string &cmd, std::vector<std::string> &args, std::string &ref, std::string &msg, std::string &lib_path) {
    // Very basic JSON parsing - just extract what we need
    if (line.find("\"cmd\"") == std::string::npos) return false;

    size_t cmd_pos = line.find("\"cmd\"");
    if (cmd_pos == std::string::npos) return false;

    size_t cmd_start = line.find('"', cmd_pos + 6);
    size_t cmd_end = line.find('"', cmd_start + 1);
    if (cmd_start == std::string::npos || cmd_end == std::string::npos) return false;
    cmd = line.substr(cmd_start + 1, cmd_end - cmd_start - 1);

    if (line.find("\"ref\"") != std::string::npos) {
        size_t ref_pos = line.find("\"ref\"");
        size_t ref_start = line.find('"', ref_pos + 6);
        size_t ref_end = line.find('"', ref_start + 1);
        if (ref_start != std::string::npos && ref_end != std::string::npos) {
            ref = line.substr(ref_start + 1, ref_end - ref_start - 1);
        }
    }

    if (line.find("\"msg\"") != std::string::npos) {
        size_t msg_pos = line.find("\"msg\"");
        size_t msg_start = line.find('"', msg_pos + 6);
        size_t msg_end = line.find('"', msg_start + 1);
        if (msg_start != std::string::npos && msg_end != std::string::npos) {
            msg = line.substr(msg_start + 1, msg_end - msg_start - 1);
        }
    }

    if (line.find("\"lib_path\"") != std::string::npos) {
        size_t path_pos = line.find("\"lib_path\"");
        size_t path_start = line.find('"', path_pos + 11);
        size_t path_end = line.find('"', path_start + 1);
        if (path_start != std::string::npos && path_end != std::string::npos) {
            lib_path = line.substr(path_start + 1, path_end - path_start - 1);
        }
    }

    // Parse args array
    if (line.find("\"args\"") != std::string::npos) {
        size_t args_pos = line.find("\"args\"");
        size_t array_start = line.find('[', args_pos);
        if (array_start != std::string::npos) {
            size_t array_end = line.find(']', array_start);
            if (array_end != std::string::npos) {
                std::string args_str = line.substr(array_start + 1, array_end - array_start - 1);
                std::istringstream iss(args_str);
                std::string arg;
                while (std::getline(iss, arg, ',')) {
                    // Remove quotes and trim
                    size_t start = arg.find('"');
                    size_t end = arg.rfind('"');
                    if (start != std::string::npos && end != std::string::npos && end > start) {
                        args.push_back(arg.substr(start + 1, end - start - 1));
                    }
                }
            }
        }
    }

    return true;
}

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::string cmd, ref, msg, lib_path;
        std::vector<std::string> args;

        if (!parse_json(line, cmd, args, ref, msg, lib_path)) {
            send_response("{\"ok\":false,\"error\":\"parse_error\"}");
            continue;
        }

        if (cmd == "create") {
            if (lib_path.empty()) {
                handle_create(args);
            } else {
                handle_create(args, lib_path);
            }
        } else if (cmd == "start") {
            handle_start(ref);
        } else if (cmd == "iteration") {
            handle_iteration(ref);
        } else if (cmd == "send_message") {
            handle_send_message(ref, msg);
        } else if (cmd == "shutdown") {
            handle_shutdown(ref);
        } else {
            send_response("{\"ok\":false,\"error\":\"unknown_command\"}");
        }
    }

    return 0;
}