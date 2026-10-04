#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace remote_control {

inline bool TempFileBelongsToDeadProcess(const char* name,
                                         const char* prefix) {
    const size_t prefixLen = strlen(prefix);
    if (strncmp(name, prefix, prefixLen) != 0) return false;
    const char* pidStart = name + prefixLen;
    const char* pidEnd = pidStart;
    while (*pidEnd >= '0' && *pidEnd <= '9') ++pidEnd;
    if (pidEnd == pidStart || *pidEnd != '-' || pidEnd[1] == '\0') {
        return false;
    }
    char* suffix = nullptr;
    errno = 0;
    const long pid = strtol(pidStart, &suffix, 10);
    if (errno != 0 || suffix == pidStart || pid <= 0 ||
        pid > std::numeric_limits<pid_t>::max() || suffix != pidEnd) {
        return false;
    }
    if (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) return false;
    return errno == ESRCH;
}

inline size_t RemoveStaleTempFiles(int directoryFd, const char* prefix) {
    const int scanFd = dup(directoryFd);
    if (scanFd < 0) return 0;
    DIR* dir = fdopendir(scanFd);
    if (!dir) {
        close(scanFd);
        return 0;
    }

    size_t removed = 0;
    const int fd = dirfd(dir);
    while (dirent* entry = readdir(dir)) {
        if (!TempFileBelongsToDeadProcess(entry->d_name, prefix)) continue;
        struct stat st{};
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode) && unlinkat(fd, entry->d_name, 0) == 0) {
            ++removed;
        }
    }
    closedir(dir);
    return removed;
}

inline size_t RemoveStaleTempFiles(const char* directory,
                                   const char* prefix) {
    const int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                      O_NOFOLLOW);
    if (fd < 0) return 0;
    const size_t removed = RemoveStaleTempFiles(fd, prefix);
    close(fd);
    return removed;
}

}  // namespace remote_control
