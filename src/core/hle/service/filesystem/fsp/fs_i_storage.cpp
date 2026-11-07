// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // Reads are paced at ReadBytesPerSecond for Mario Strikers' intro (Eden #4525). Ocarina of
    // Time reads its movies while it plays them, and the 1.6 ms or more per read made its movie
    // player drop frames behind the audio. EDEN_FS_READ_PACING=0 or 1 forces it for any game.
    static const int forced_pacing = [] {
        const char* value = std::getenv("EDEN_FS_READ_PACING");
        return value == nullptr ? -1 : value[0] == '0' ? 0 : 1;
    }();
    const bool pace = forced_pacing != -1
                          ? forced_pacing == 1
                          : system.GetApplicationProcessProgramID() != 0x0400486024DEC000ULL;
    if (pace) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_read_tick) {
            std::this_thread::sleep_for(next_read_tick - now);
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / ReadBytesPerSecond};
    }

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
