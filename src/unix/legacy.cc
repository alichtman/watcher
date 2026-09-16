#include <string>
#include <memory>
#include <set>
#include <utility>

// weird error on linux
#ifdef __THROW
#undef __THROW
#endif
#define __THROW

#ifdef _LIBC
# include <include/sys/stat.h>
#else
# include <sys/stat.h>
#endif
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "../DirTree.hh"
#include "../shared/BruteForceBackend.hh"

#define CONVERT_TIME(ts) ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec)
#if __APPLE__
#define st_mtim st_mtimespec
#endif
#define ISDOT(a) (a[0] == '.' && (!a[1] || (a[1] == '.' && !a[2])))

using DirectoryIdentity = std::pair<dev_t, ino_t>;
using DirectoryAncestors = std::set<DirectoryIdentity>;

class ScopedDirectoryAncestor {
public:
    ScopedDirectoryAncestor(DirectoryAncestors &ancestors, DirectoryIdentity identity)
        : mAncestors(ancestors), mIdentity(identity) {}

    ~ScopedDirectoryAncestor() {
        mAncestors.erase(mIdentity);
    }

private:
    DirectoryAncestors &mAncestors;
    DirectoryIdentity mIdentity;
};

struct DirectoryCloser {
    void operator()(DIR *dir) const {
        closedir(dir);
    }
};

static WatcherError pathError(const char *op, const std::string &path, int error, WatcherRef watcher) {
    return WatcherError(std::string(op) + " on '" + path + "' failed: " + strerror(error), watcher);
}

void iterateDir(WatcherRef watcher, const std::shared_ptr <DirTree> tree, const char *relative, int parent_fd, const std::string &dirname, DirectoryAncestors &ancestors, bool isRoot) {
    int open_flags = (O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
    int new_fd = openat(parent_fd, relative, open_flags);
    if (new_fd == -1) {
        // ENOENT means the directory was removed between the caller's fstatat
        // and this open, which is routine in a tree that is being written to
        // while it is read. Treat it like a directory we were never told about.
        if (errno == EACCES || (!isRoot && errno == ENOENT)) {
            return;
        }

        throw pathError("openat", dirname, errno, watcher);
    }

    struct stat rootAttributes;
    if (fstat(new_fd, &rootAttributes) != 0) {
        int error = errno;
        close(new_fd);
        throw pathError("fstat", dirname, error, watcher);
    }

    DirectoryIdentity identity(rootAttributes.st_dev, rootAttributes.st_ino);
    // Some virtual filesystems expose directory aliases as directories rather
    // than symlinks. Only reject identities in the current ancestry so the same
    // directory can still be visited through independent, non-cyclic paths.
    if (!ancestors.insert(identity).second) {
        close(new_fd);
        return;
    }
    ScopedDirectoryAncestor ancestor(ancestors, identity);

    DIR *rawDir = fdopendir(new_fd);
    if (!rawDir) {
        int error = errno;
        close(new_fd);
        throw pathError("fdopendir", dirname, error, watcher);
    }
    std::unique_ptr<DIR, DirectoryCloser> dir(rawDir);

    tree->add(dirname, CONVERT_TIME(rootAttributes.st_mtim), true);

    while (struct dirent *ent = (errno = 0, readdir(dir.get()))) {
        if (ISDOT(ent->d_name)) continue;

        std::string fullPath = dirname + "/" + ent->d_name;
        // Skip the entry before handing an unbounded string to the recursive
        // std::regex matcher behind isIgnored(). Skipping rather than failing
        // keeps the rest of the tree usable: only inotify cannot watch paths
        // beyond PATH_MAX, while the brute force and wasm backends traverse
        // entirely through descriptor-relative calls and have no such limit.
        if (fullPath.size() >= PATH_MAX) continue;

        if (!watcher->isIgnored(fullPath)) {
            struct stat attrib;
            if (fstatat(new_fd, ent->d_name, &attrib, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == EACCES || errno == ENOENT) {
                    continue;
                }

                throw pathError("fstatat", fullPath, errno, watcher);
            }
            bool isDir = S_ISDIR(attrib.st_mode);

            if (isDir) {
                iterateDir(watcher, tree, ent->d_name, new_fd, fullPath, ancestors, false);
            } else {
                tree->add(fullPath, CONVERT_TIME(attrib.st_mtim), isDir);
            }
        }
    }

    if (errno) {
        throw pathError("readdir", dirname, errno, watcher);
    }
}

void BruteForceBackend::readTree(WatcherRef watcher, std::shared_ptr <DirTree> tree) {
    int fd = open(watcher->mDir.c_str(), O_RDONLY);
    if (fd == -1) {
        throw pathError("open", watcher->mDir, errno, watcher);
    }

    DirectoryAncestors ancestors;
    try {
        iterateDir(watcher, tree, ".", fd, watcher->mDir, ancestors, true);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
}
