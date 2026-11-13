#include "process.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

extern char** environ;  // NOLINT: POSIX

namespace nxmac::dmsh {

ProcessResult RunProcess(const std::vector<std::string>& argv, const std::string& stdin_path) {
    ProcessResult r;
    if (argv.empty()) {
        r.error = "no program";
        return r;
    }
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) {
        r.error = std::strerror(errno);
        return r;
    }
    if (pipe(err_pipe) != 0) {
        r.error = std::strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return r;
    }
    // The parent's ends are not inherited by this or any other child
    for (int fd : {out_pipe[0], err_pipe[0]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, stdin_path.empty() ? "/dev/null" : stdin_path.c_str(), O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[1]);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));  // NOLINT: posix_spawn's signature
    args.push_back(nullptr);
    pid_t pid = 0;
    const bool path = absl::StrContains(argv[0], '/');
    const int rc = path ? posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ)
                        : posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (rc != 0) {
        r.error = absl::StrCat(argv[0], ": ", std::strerror(rc));
        close(out_pipe[0]);
        close(err_pipe[0]);
        return r;
    }
    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    std::string* sinks[2] = {&r.out, &r.err};
    int open_fds = 2;
    char buf[1 << 16];
    while (open_fds) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < 2; ++k) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = read(fds[k].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[k]->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[k].fd);
                fds[k].fd = -1;
                --open_fds;
            }
        }
    }
    for (pollfd& p : fds)
        if (p.fd >= 0) close(p.fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

std::string CalledProcessError(const std::vector<std::string>& argv, int code) {
    std::string list;
    for (std::size_t i = 0; i < argv.size(); ++i) absl::StrAppend(&list, i ? ", " : "", "'", argv[i], "'");
    return absl::StrCat("Command '[", list, "]' returned non-zero exit status ", code, ".");
}

}  // namespace nxmac::dmsh
