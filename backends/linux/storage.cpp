// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <ht/settings.hpp>
#include <ht/codeplug_records.hpp>
#include <ht/codeplug_storage.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <zephyr/sys/byteorder.h>

namespace ht {
static char directory[384];
static char filename[448];
static bool ready;
static int profile_lock = -1;
static char locked_path[464];
static int session_fd = -1;

int settings_storage_init() {
    if (session_fd >= 0) {
        return -EBUSY;
    }
    ready = false;
    const char *root = getenv("HT_SETTINGS_DIR");
    const char *profile = getenv("HT_PROFILE");
    if (!root) {
        root = "ht-settings";
    }
    if (!profile) {
        profile = "default";
    }
    const size_t length = strnlen(profile, 33);
    if (!length || length > 32) {
        return -EINVAL;
    }
    for (size_t i = 0; i < length; ++i) {
        const char c = profile[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return -EINVAL;
        }
    }
    const int count = snprintf(directory, sizeof(directory), "%s", root);
    if (count <= 0 || count >= static_cast<int>(sizeof(directory))) {
        return -ENAMETOOLONG;
    }
    snprintf(filename, sizeof(filename), "%s/%s.bin", directory, profile);
    if (mkdir(directory, 0700) && errno != EEXIST) {
        return -errno;
    }
    struct stat info;
    if (stat(directory, &info)) {
        return -errno;
    }
    if (!S_ISDIR(info.st_mode)) {
        return -ENOTDIR;
    }
    char lock_path[464];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", filename);
    if (profile_lock < 0 || strcmp(locked_path, lock_path)) {
        const int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) {
            return -errno;
        }
        if (flock(fd, LOCK_EX | LOCK_NB)) {
            const int error = errno;
            close(fd);
            return error == EWOULDBLOCK || error == EAGAIN ? -EBUSY : -error;
        }
        if (profile_lock >= 0) {
            close(profile_lock);
        }
        profile_lock = fd;
        strcpy(locked_path, lock_path);
    }
    ready = true;
    return 0;
}

static bool session_writing;
static uint32_t session_generation;
static CodeplugSavePolicy session_policy;
static char temporary_path[480];
// No caller stack contains both serialized and decoded manifest buffers.
static uint8_t check_bytes[codeplug_record_max];
static CodeplugManifest checked_manifest;

static int read_exact(int fd, uint8_t *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const ssize_t count = read(fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return count < 0 ? -errno : -EBADMSG;
        }
        offset += count;
    }
    return 0;
}

static int read_record(int fd, uint8_t *bytes, size_t capacity, size_t &length) {
    if (!bytes || capacity < codeplug_envelope_size) {
        return -EINVAL;
    }
    int error = read_exact(fd, bytes, codeplug_envelope_size);
    if (error) {
        return error;
    }
    const size_t n = sys_get_le16(bytes + 6);
    if (n < codeplug_envelope_size || n > capacity) {
        return -EBADMSG;
    }
    error = read_exact(fd, bytes + codeplug_envelope_size, n - codeplug_envelope_size);
    if (!error) {
        length = n;
    }
    return error;
}

static int current_generation(uint32_t &generation) {
    const int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            generation = 0;
            return 0;
        }
        return -errno;
    }
    size_t n = 0;
    int error = read_record(fd, check_bytes, sizeof(check_bytes), n);
    const uint32_t found = error ? 0 : sys_get_le32(check_bytes + 8);
    if (!error) {
        error = decode_manifest(check_bytes, n, found, checked_manifest);
    }
    if (close(fd) && !error) {
        error = -errno;
    }
    if (!error) {
        generation = found;
    }
    return error;
}

static bool record_index(StoreRecord kind, size_t index) {
    switch (kind) {
    case StoreRecord::Manifest:
        return index == 0;
    case StoreRecord::Channel:
        return index < channel_capacity;
    case StoreRecord::Bank:
        return index < bank_capacity;
    }
    return false;
}

int codeplug_store_open(bool writing, uint32_t &generation, const CodeplugSavePolicy &policy) {
    if (!ready || profile_lock < 0) {
        return -ENODEV;
    }
    if (session_fd >= 0) {
        return -EBUSY;
    }
    const int policy_error = writing ? codeplug_save_check(policy) : 0;
    if (policy_error) {
        return policy_error;
    }
    if (writing) {
        uint32_t current = 0;
        const int error = current_generation(current);
        if (error) {
            return error;
        }
        if (current != generation) {
            return -ESTALE;
        }
        if (current == UINT32_MAX) {
            return -EOVERFLOW;
        }
        snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.XXXXXX", filename);
        session_fd = mkstemp(temporary_path);
        if (session_fd < 0) {
            return -errno;
        }
        if (fcntl(session_fd, F_SETFD, FD_CLOEXEC)) {
            const int error = -errno;
            close(session_fd);
            session_fd = -1;
            unlink(temporary_path);
            return error;
        }
        generation = current + 1;
    } else {
        session_fd = open(filename, O_RDONLY | O_CLOEXEC);
        if (session_fd < 0) {
            return -errno;
        }
        uint8_t header[codeplug_envelope_size];
        int error = read_exact(session_fd, header, sizeof(header));
        if (!error && lseek(session_fd, 0, SEEK_SET) < 0) {
            error = -errno;
        }
        if (!error && !sys_get_le32(header + 8)) {
            error = -EBADMSG;
        }
        if (error) {
            close(session_fd);
            session_fd = -1;
            return error;
        }
        generation = sys_get_le32(header + 8);
    }
    session_policy = writing ? policy : CodeplugSavePolicy{};
    session_writing = writing;
    session_generation = generation;
    return 0;
}

int codeplug_store_read(StoreRecord kind, size_t index, uint8_t *bytes, size_t capacity,
                        size_t &length) {
    if (session_fd < 0 || session_writing) {
        return -ENODEV;
    }
    if (!record_index(kind, index)) {
        return -EINVAL;
    }
    return read_record(session_fd, bytes, capacity, length);
}

int codeplug_store_write(StoreRecord kind, size_t index, const uint8_t *bytes, size_t length) {
    if (session_fd < 0 || !session_writing) {
        return -ENODEV;
    }
    if (!record_index(kind, index) || !bytes || !length || length > codeplug_record_max) {
        return -EINVAL;
    }
    size_t offset = 0;
    while (offset < length) {
        const int policy_error = codeplug_save_check(session_policy);
        if (policy_error) {
            return policy_error;
        }
        const ssize_t count = write(session_fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return count < 0 ? -errno : -EIO;
        }
        offset += count;
    }
    return 0;
}

int codeplug_store_close(bool commit, bool &published) {
    published = false;
    if (session_fd < 0) {
        return -ENODEV;
    }
    int error = 0;
    if (session_writing && commit) {
        error = codeplug_save_check(session_policy);
        if (!error && fsync(session_fd)) {
            error = -errno;
        }
    } else if (!session_writing) {
        uint8_t extra;
        ssize_t count;
        do {
            count = read(session_fd, &extra, 1);
        } while (count < 0 && errno == EINTR);
        if (count) {
            error = count < 0 ? -errno : -EBADMSG;
        }
    }
    if (close(session_fd) && !error) {
        error = -errno;
    }
    session_fd = -1;
    if (session_writing && commit && !error) {
        error = codeplug_save_check(session_policy);
    }
    if (session_writing && commit && !error) {
        if (rename(temporary_path, filename)) {
            error = -errno;
            uint32_t current = 0;
            published = !current_generation(current) && current == session_generation;
        } else {
            published = true;
            const int dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dir_fd < 0) {
                error = -errno;
            } else {
                if (fsync(dir_fd)) {
                    error = -errno;
                }
                if (close(dir_fd) && !error) {
                    error = -errno;
                }
            }
        }
    }
    if (session_writing && !published) {
        unlink(temporary_path);
    }
    session_writing = false;
    return error;
}
} // namespace ht
