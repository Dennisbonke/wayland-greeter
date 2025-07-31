#include <systemd/sd-bus.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/wait.h>
#include <iostream>

bool create_logind_session(const char* username, const char* seat_id, char* out_runtime_dir, size_t out_size, const char** out_session_path) {
    // sd_bus *bus = nullptr;
    // sd_bus_error error = SD_BUS_ERROR_NULL;
    // sd_bus_message *msg = nullptr;
    int ret = 0;

    sd_bus* bus = nullptr;
    sd_bus_message* m = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        std::cerr << "Failed to open system bus: " << strerror(-r) << "\n";
        return false;
    }

    r = sd_bus_message_new_method_call(
        bus,
        &m,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "CreateSession"
    );
    if (r < 0) {
        std::cerr << "Failed to create method call: " << strerror(-r) << "\n";
        return false;
    }

    // Append arguments according to uusssssussbss + a(sv)
    r = sd_bus_message_append(m, "uusssssussbss",
        (uint32_t)getuid(),          // uid
        (uint32_t)getpid(),          // pid
        "wl_greeter",           // service
        "wayland",              // type
        "greeter",              // class
        "",                     // desktop
        "seat0",                // seat
        1,                      // vtnr
        "",                     // tty
        "",                     // display
        0,                      // remote
        "",                     // remote user
        ""                      // remote host
        ""                      // remote hostname
    );
    if (r < 0) {
        std::cerr << "Failed to append base args: " << strerror(-r) << "\n";
        return false;
    }

    // Open and close an empty a(sv) array
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(sv)");
    if (r < 0) {
        std::cerr << "Failed to open a(sv) container: " << strerror(-r) << "\n";
        return false;
    }

    // No properties for now

    r = sd_bus_message_close_container(m);
    if (r < 0) {
        std::cerr << "Failed to close a(sv) container: " << strerror(-r) << "\n";
        return false;
    }

    // Send the message
    r = sd_bus_call(bus, m, 0, &error, &reply);
    if (r < 0) {
        std::cerr << "CreateSession failed: " << error.message << "\n";
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    const char* session_id = nullptr;
    const char* session_path = nullptr;

    r = sd_bus_message_read(reply, "so", &session_id, &session_path);
    if (r < 0) {
        std::cerr << "Failed to parse reply: " << strerror(-r) << "\n";
        return false;
    }

    std::cout << "Session created successfully:\n";
    std::cout << "  ID: " << session_id << "\n";
    std::cout << "  Path: " << session_path << "\n";

    // Copy session path to output pointer for later use
    *out_session_path = strdup(session_path);

    // Compose XDG_RUNTIME_DIR for this session; typically /run/user/<uid>
    // Here just setting dummy, you can parse real user uid for correctness
    snprintf(out_runtime_dir, out_size, "/run/user/%d", getuid());

    sd_bus_message_unref(m);
    sd_bus_unref(bus);
    return true;
}

bool activate_logind_session(const char* session_path) {
    sd_bus *bus = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int ret = 0;

    ret = sd_bus_default_system(&bus);
    if (ret < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-ret));
        return false;
    }

    ret = sd_bus_call_method(bus,
        "org.freedesktop.login1",
        session_path,
        "org.freedesktop.login1.Session",
        "Activate",
        &error,
        nullptr,
        "");
    if (ret < 0) {
        fprintf(stderr, "ActivateSession failed: %s\n", error.message);
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    sd_bus_unref(bus);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /path/to/greeter [args...]\n", argv[0]);
        return 1;
    }

    char runtime_dir[256];
    const char* username = getenv("USER");
    if (!username) username = "root"; // fallback
    const char* seat = "seat0";
    const char* session_path = nullptr;

    if (!create_logind_session(username, seat, runtime_dir, sizeof(runtime_dir), &session_path)) {
        fprintf(stderr, "Failed to create logind session\n");
        return 1;
    }

    if (!activate_logind_session(session_path)) {
        fprintf(stderr, "Failed to activate logind session\n");
        free((void*)session_path);
        return 1;
    }

    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);

    free((void*)session_path);

    // Fork and exec the greeter (e.g. sway)
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[1], &argv[1]);
        perror("execvp failed");
        _exit(1);
    } else if (pid < 0) {
        perror("fork failed");
        return 1;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
