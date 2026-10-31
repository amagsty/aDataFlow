#pragma once

#include <cstddef>
#include <cstdio>

// A successful final attempt is still success. Never accept mkdir failure.
template <typename Filesystem, typename NextId>
bool create_recording_folder(Filesystem &fs, char *path, size_t capacity,
                             size_t attempts, NextId next_id)
{
    for (size_t i = 0; i < attempts; ++i)
    {
        const int length = std::snprintf(path, capacity, "/DAT_%04u", unsigned(next_id()));
        if (length < 0 || size_t(length) >= capacity)
            return false;
        if (!fs.exists(path) && fs.mkdir(path))
            return true;
    }
    return false;
}
