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

    // A copy would erase the identity when the first of the two goes out of
    // scope, reopening the cycle it is meant to close.
    ScopedDirectoryAncestor(const ScopedDirectoryAncestor &) = delete;
    ScopedDirectoryAncestor &operator=(const ScopedDirectoryAncestor &) = delete;

private:
    DirectoryAncestors &mAncestors;
    DirectoryIdentity mIdentity;
};

class ScopedFileDescriptor {
public:
    explicit ScopedFileDescriptor(int fd) : mFd(fd) {}

    ~ScopedFileDescriptor() {
        if (mFd != -1) {
            close(mFd);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
    ScopedFileDescriptor &operator=(const ScopedFileDescriptor &) = delete;

    int get() const {
        return mFd;
    }

    int release() {
        int fd = mFd;
        mFd = -1;
        return fd;
    }

private:
    int mFd;
};

class ScopedPathComponent {
public:
    ScopedPathComponent(std::string &path, const char *component)
        : mPath(path), mOriginalSize(path.size()) {
        try {
            mPath.push_back('/');
            mPath.append(component);
        } catch (...) {
            mPath.resize(mOriginalSize);
            throw;
        }
    }

    ~ScopedPathComponent() {
        mPath.resize(mOriginalSize);
    }

    ScopedPathComponent(const ScopedPathComponent &) = delete;
    ScopedPathComponent &operator=(const ScopedPathComponent &) = delete;

private:
    std::string &mPath;
    size_t mOriginalSize;
};

struct DirectoryCloser {
    void operator()(DIR *dir) const {
        closedir(dir);
    }
};

static WatcherError pathError(const char *op, const std::string &path, int error, WatcherRef watcher) {
    return WatcherError(std::string(op) + " on '" + path + "' failed: " + strerror(error), watcher);
}

void iterateDir(
    WatcherRef watcher,
    const std::shared_ptr <DirTree> tree,
    const char *relative,
    int parent_fd,
    std::string &path,
    DirectoryAncestors &ancestors,
    bool isRoot) {
    int open_flags = (O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
    ScopedFileDescriptor scoped_fd(openat(parent_fd, relative, open_flags));
    if (scoped_fd.get() == -1) {
        // ENOENT means the directory was removed between the caller's fstatat
        // and this open, which is routine in a tree that is being written to
        // while it is read. Treat it like a directory we were never told about.
        if (errno == EACCES || (!isRoot && errno == ENOENT)) {
            return;
        }

        throw pathError("openat", path, errno, watcher);
    }
    int new_fd = scoped_fd.get();

    struct stat rootAttributes;
    if (fstat(new_fd, &rootAttributes) != 0) {
        throw pathError("fstat", path, errno, watcher);
    }

    DirectoryIdentity identity(rootAttributes.st_dev, rootAttributes.st_ino);
    // Some virtual filesystems expose directory aliases as directories rather
    // than symlinks. Only reject identities in the current ancestry so the same
    // directory can still be visited through independent, non-cyclic paths.
    if (!ancestors.insert(identity).second) {
        return;
    }
    ScopedDirectoryAncestor ancestor(ancestors, identity);

    DIR *rawDir = fdopendir(new_fd);
    if (!rawDir) {
        throw pathError("fdopendir", path, errno, watcher);
    }
    // fdopendir takes ownership of the descriptor, so closedir now covers it.
    scoped_fd.release();
    std::unique_ptr<DIR, DirectoryCloser> dir(rawDir);

    tree->add(path, CONVERT_TIME(rootAttributes.st_mtim), true);

    while (struct dirent *ent = (errno = 0, readdir(dir.get()))) {
        if (ISDOT(ent->d_name)) continue;

        ScopedPathComponent childPath(path, ent->d_name);
        // Skip the entry before handing an unbounded string to the recursive
        // std::regex matcher behind isIgnored(). Skipping rather than failing
        // keeps the rest of the tree usable: only inotify cannot watch paths
        // beyond PATH_MAX, while the brute force and wasm backends traverse
        // entirely through descriptor-relative calls and have no such limit.
        if (path.size() >= PATH_MAX) continue;

        if (!watcher->isIgnored(path)) {
            struct stat attrib;
            if (fstatat(new_fd, ent->d_name, &attrib, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == EACCES || errno == ENOENT) {
                    continue;
                }

                throw pathError("fstatat", path, errno, watcher);
            }
            bool isDir = S_ISDIR(attrib.st_mode);

            if (isDir) {
                iterateDir(watcher, tree, ent->d_name, new_fd, path, ancestors, false);
            } else {
                tree->add(path, CONVERT_TIME(attrib.st_mtim), isDir);
            }
        }
    }

    if (errno) {
        throw pathError("readdir", path, errno, watcher);
    }
}

void BruteForceBackend::readTree(WatcherRef watcher, std::shared_ptr <DirTree> tree) {
    ScopedFileDescriptor fd(open(watcher->mDir.c_str(), O_RDONLY));
    if (fd.get() == -1) {
        throw pathError("open", watcher->mDir, errno, watcher);
    }

    DirectoryAncestors ancestors;
    std::string path = watcher->mDir;
    path.reserve(PATH_MAX);
    iterateDir(watcher, tree, ".", fd.get(), path, ancestors, true);
}
