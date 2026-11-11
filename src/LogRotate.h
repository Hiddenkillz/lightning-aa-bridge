#pragma once
// Keeps the daemon's log file from growing without bound (the unit appends stdout/stderr to /var/log/aa-bridge.log, which systemd cannot rotate).
// When the file behind `fd` exceeds max_bytes: copy it to "<path>.1" (replacing the previous copy), then truncate it in place. The fd is
// O_APPEND (systemd's `append:`), so writers simply continue at offset 0. Bounded disk use: at most 2 x max_bytes.
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace logrotate {
// Returns true if it rotated. `path_override` is for tests (otherwise the path is read from /proc/self/fd/<fd>).
inline bool rotateIfLarge(int fd, long long max_bytes, const std::string& path_override = "") {
    struct stat st{};
    if (max_bytes <= 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= max_bytes) return false;
    std::string path = path_override;
    if (path.empty()) {
        char buf[1024]; ssize_t n = readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buf, sizeof(buf) - 1);
        if (n <= 0) return false;
        path.assign(buf, (size_t)n);
    }
    const std::string dst = path + ".1", tmp = path + ".1.tmp";
    int in = open(path.c_str(), O_RDONLY); if (in < 0) return false;
    int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return false; }
    off_t off = 0; bool ok = true;
    while (off < st.st_size) { ssize_t n = sendfile(out, in, &off, (size_t)(st.st_size - off)); if (n <= 0) { ok = false; break; } }
    close(in); fsync(out); close(out);
    if (!ok || rename(tmp.c_str(), dst.c_str()) != 0) { unlink(tmp.c_str()); return false; }   // never truncate unless the copy is safe
    return ftruncate(fd, 0) == 0;
}
}  // namespace logrotate
