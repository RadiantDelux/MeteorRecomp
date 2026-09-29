#pragma once

#include "isa/big_endian.h"
#include "local_checkpoint.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#include <share.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

// EXI flash protocol used by the retail CARD library. Command/address layout
// reference: Dolphin's Core/HW/EXI/EXI_DeviceMemoryCard.cpp (GPL-2.0-or-later).
// The guest still owns its directory, BAT, checksums, callbacks and file API.
class GameCubeMemoryCard {
public:
    static constexpr uint32_t BlockSize = 0x2000;
    static constexpr uint32_t DefaultSize = 16 * 128 * 1024; // 251 usable blocks
    ~GameCubeMemoryCard() {
        if (!file_) return;
        // An opt-in batched launch batches the guest's burst of 512-byte CARD
        // programs so one save does not force the host disk hundreds of times.
        // Do a best-effort final commit on orderly process shutdown so the last
        // completed guest save is not left only in the in-memory card image.
        try { FlushPendingWrites(); } catch (...) {}
        std::fclose(file_);
    }
    GameCubeMemoryCard() = default;
    GameCubeMemoryCard(const GameCubeMemoryCard&) = delete;
    GameCubeMemoryCard& operator=(const GameCubeMemoryCard&) = delete;

    void Open(const std::filesystem::path& path) {
        if (file_) throw std::runtime_error("Memory card is already open");
        std::filesystem::create_directories(path.parent_path());
        // Never truncate an existing image, even when its contents are invalid.
        const bool exists = std::filesystem::exists(path);
#if defined(_WIN32)
        file_ = _wfsopen(path.c_str(), exists ? L"r+b" : L"w+bx", _SH_DENYWR);
#else
        file_ = std::fopen(path.c_str(), exists ? "r+b" : "w+bx");
        if (file_ && flock(fileno(file_), LOCK_EX | LOCK_NB) != 0) {
            std::fclose(file_);
            file_ = nullptr;
        }
#endif
        if (!file_) throw std::runtime_error("Cannot open memory card (in use or not writable)");
        try {
            if (exists) {
                const auto size = std::filesystem::file_size(path);
                if (size < 512 * 1024 || size > 16 * 1024 * 1024 || (size & (size - 1)))
                    throw std::runtime_error("Invalid raw memory card size; image left untouched");
                data_.resize(static_cast<size_t>(size));
                if (std::fread(data_.data(), 1, data_.size(), file_) != data_.size())
                    throw std::runtime_error("Cannot read memory card");
            } else {
                data_ = NewFormattedImage();
                Persist(0, data_.size());
            }
            present_ = true;
        } catch (...) {
            std::fclose(file_);
            file_ = nullptr;
            data_.clear();
            throw;
        }
    }

    bool Present() const { return present_; }
    void SetBatchWrites(bool enabled) {
        FlushPendingWrites();
        batchWrites_ = enabled;
        idleObservedGeneration_ = pendingWriteGeneration_;
    }
    void FlushPendingWrites() {
        if (dirtyEnd_ == 0) return;
        PersistRange(dirtyStart_, dirtyEnd_ - dirtyStart_);
        dirtyEnd_ = 0;
        idleObservedGeneration_ = pendingWriteGeneration_;
    }
    void FlushPendingWritesIfIdle() {
        if (dirtyEnd_ == 0) {
            idleObservedGeneration_ = pendingWriteGeneration_;
            return;
        }
        // The title can program several card pages over consecutive game
        // updates.  Wait for one complete update with no new card write before
        // issuing the expensive host durability barrier.  This turns a whole
        // retail save burst into one commit without delaying guest CARD status.
        if (idleObservedGeneration_ != pendingWriteGeneration_) {
            idleObservedGeneration_ = pendingWriteGeneration_;
            return;
        }
        FlushPendingWrites();
    }
    uint64_t DurableWriteCount() const { return durableWrites_; }
    bool InterruptPending() const { return interruptEnabled_ && interruptPending_; }
    const std::vector<uint8_t>& Data() const { return data_; }

    // Build a separate formatted card from one GCI export. Never opens or
    // modifies an existing card; the caller chooses where to store the result.
    static std::vector<uint8_t> FormattedImageWithSave(const std::vector<uint8_t>& save) {
        if (save.size() < 64) throw std::invalid_argument("Incomplete GCI save");
        const auto blocks = BigEndian::Read16(save.data() + 0x38);
        if (blocks == 0 || blocks > DefaultSize / BlockSize - 5 || save.size() != 64 + size_t(blocks) * BlockSize)
            throw std::invalid_argument("Invalid GCI save size");
        auto bytes = NewFormattedImage();
        for (size_t block : {1u, 2u}) {
            auto* directory = bytes.data() + block * BlockSize;
            std::copy_n(save.data(), 64, directory);
            BigEndian::Write16(directory + 0x36, 5);
            Checksum(directory, 0x1FFC, directory + 0x1FFC);
        }
        for (size_t block : {3u, 4u}) {
            auto* bat = bytes.data() + block * BlockSize;
            BigEndian::Write16(bat + 6, DefaultSize / BlockSize - 5 - blocks);
            BigEndian::Write16(bat + 8, 4 + blocks);
            for (size_t i = 0; i < blocks; ++i)
                BigEndian::Write16(bat + 10 + i * 2, i + 1 == blocks ? 0xFFFF : static_cast<uint16_t>(6 + i));
            Checksum(bat + 4, BlockSize - 4, bat);
        }
        std::copy(save.begin() + 64, save.end(), bytes.begin() + 5 * BlockSize);
        return bytes;
    }

    LocalCheckpoint CaptureState() {
        if (dirtyEnd_ != 0) return {};
        const auto generation = persistedGeneration_;
        const auto file = file_;
        // Until confirmed-write transactions are active, reject a rewind over
        // a disk write. Restoring RAM must never silently undo a persisted save.
        return LocalCheckpoint::Capture([this, generation, file] {
            return generation == persistedGeneration_ && file == file_;
        }, data_, program_, position_, address_, command_, status_, present_,
           interruptEnabled_, interruptPending_, dmaWrite_);
    }

    std::array<uint8_t, 12> FlashId() const {
        std::array<uint8_t, 12> flash{};
        uint64_t seed = (uint64_t(BigEndian::Read32(data_.data() + 12)) << 32) |
                        BigEndian::Read32(data_.data() + 16);
        for (size_t i = 0; i < flash.size(); ++i) {
            seed = (seed * 0x41C64E6DULL + 0x3039) >> 16;
            flash[i] = static_cast<uint8_t>(data_[i] - seed);
            seed = ((seed * 0x41C64E6DULL + 0x3039) >> 16) & 0x7FFF;
        }
        return flash;
    }

    void Select(bool selected) {
        if (selected) { position_ = 0; dmaWrite_ = false; return; }
        if (!present_) return;
        try {
            if (command_ == 0xF1 && position_ > 2) {
                const uint32_t start = (address_ & (data_.size() - 1)) & ~(BlockSize - 1);
                std::fill_n(data_.begin() + start, BlockSize, 0xFF);
                Persist(start, BlockSize);
                Complete();
            } else if (command_ == 0xF2 && position_ >= 5) {
                if (!dmaWrite_) {
                    for (uint32_t i = 0; i < position_ - 5; ++i) {
                        data_[address_ & (data_.size() - 1)] = program_[i & 127];
                        address_ = (address_ & ~0x1FFu) | ((address_ + 1) & 0x1FF);
                    }
                    const uint32_t start = (address_ & (data_.size() - 1)) & ~0x1FFu;
                    Persist(start, 512);
                }
                Complete();
            }
        } catch (...) {
            status_ |= command_ == 0xF1 ? 0x10 : 0x08;
            Complete();
            throw;
        }
    }

    uint8_t Transfer(uint8_t byte) {
        if (!present_) return 0;
        const uint32_t pos = position_++;
        if (pos == 0) {
            command_ = byte;
            if (command_ == 0x89) { status_ = 0x41; interruptPending_ = false; }
            return 0xFF;
        }
        switch (command_) {
        case 0x00:
            return pos == 1 ? 0x80 : static_cast<uint8_t>((data_.size() / (128 * 1024)) >>
                                                        (24 - ((pos - 2) & 3) * 8));
        case 0x83: return status_;
        case 0x85: return pos == 1 || !(pos & 1) ? 0xC2 : 0x21;
        case 0x81: if (pos == 1) interruptEnabled_ = byte != 0; return 0xFF;
        case 0x52:
        case 0xF1:
        case 0xF2:
            if (pos == 1) address_ = uint32_t(byte) << 17;
            if (pos == 2) address_ |= uint32_t(byte) << 9;
            if (command_ != 0xF1) {
                if (pos == 3) address_ |= uint32_t(byte & 3) << 7;
                if (pos == 4) address_ |= byte & 127;
            }
            if (command_ == 0x52 && pos >= 9) {
                const uint8_t result = data_[address_ & (data_.size() - 1)];
                address_ = (address_ & ~0x1FFu) | ((address_ + 1) & 0x1FF);
                return result;
            }
            if (command_ == 0xF2 && pos >= 5) program_[(pos - 5) & 127] = byte;
            return 0xFF;
        default: return 0xFF;
        }
    }

    void DmaRead(uint8_t* out, size_t size) const {
        if (command_ != 0x52 || position_ < 9) throw std::runtime_error("Invalid card DMA read");
        const size_t start = address_ & (data_.size() - 1);
        if (size > data_.size() - start) throw std::runtime_error("Card DMA read out of bounds");
        std::copy_n(data_.data() + start, size, out);
    }

    void DmaWrite(const uint8_t* in, size_t size) {
        if (command_ != 0xF2 || position_ < 5) throw std::runtime_error("Invalid card DMA write");
        const size_t start = address_ & (data_.size() - 1);
        if (size > data_.size() - start) throw std::runtime_error("Card DMA write out of bounds");
        std::copy_n(in, size, data_.data() + start);
        try { Persist(start, size); }
        catch (...) { status_ |= 0x08; Complete(); throw; }
        dmaWrite_ = true;
    }

private:
    static void Checksum(uint8_t* start, size_t size, uint8_t* out) {
        uint16_t sum = 0, inverse = 0;
        for (size_t i = 0; i < size; i += 2) {
            const uint16_t value = BigEndian::Read16(start + i);
            sum += value;
            inverse += static_cast<uint16_t>(~value);
        }
        BigEndian::Write16(out, sum == 0xFFFF ? 0 : sum);
        BigEndian::Write16(out + 2, inverse == 0xFFFF ? 0 : inverse);
    }
    static std::vector<uint8_t> NewFormattedImage() {
        std::vector<uint8_t> bytes(DefaultSize, 0xFF);
        // Blank Western card; the serial encodes the SRAM flash id and timestamp.
        constexpr char flash[] = "DOLPHINSLOTA";
        uint64_t seed = 0;
        for (size_t i = 0; i < 12; ++i) {
            seed = (seed * 0x41C64E6DULL + 0x3039) >> 16;
            bytes[i] = static_cast<uint8_t>(flash[i] + seed);
            seed = ((seed * 0x41C64E6DULL + 0x3039) >> 16) & 0x7FFF;
        }
        std::fill(bytes.begin() + 12, bytes.begin() + 38, 0);
        BigEndian::Write16(bytes.data() + 34, 16);
        BigEndian::Write16(bytes.data() + 0x1FA, 0);
        Checksum(bytes.data(), 0x1FC, bytes.data() + 0x1FC);
        for (size_t block : {1u, 2u}) {
            auto* p = bytes.data() + block * BlockSize;
            BigEndian::Write16(p + 0x1FFA, 0);
            Checksum(p, 0x1FFC, p + 0x1FFC);
        }
        for (size_t block : {3u, 4u}) {
            auto* p = bytes.data() + block * BlockSize;
            std::fill_n(p, BlockSize, 0);
            BigEndian::Write16(p + 6, DefaultSize / BlockSize - 5);
            BigEndian::Write16(p + 8, 4);
            Checksum(p + 4, BlockSize - 4, p);
        }
        return bytes;
    }
    void Complete() { status_ = (status_ & ~0x80) | 0x41; interruptPending_ = true; }
    void Persist(size_t offset, size_t length) {
        ++persistedGeneration_;
        if (batchWrites_) {
            ++pendingWriteGeneration_;
            dirtyStart_ = dirtyEnd_ ? std::min(dirtyStart_, offset) : offset;
            dirtyEnd_ = std::max(dirtyEnd_, offset + length);
            return;
        }
        PersistRange(offset, length);
    }
    void PersistRange(size_t offset, size_t length) {
        const auto traceStarted = std::chrono::steady_clock::now();
        if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0 ||
            std::fwrite(data_.data() + offset, 1, length, file_) != length || std::fflush(file_) != 0)
            throw std::runtime_error("Memory card write failed");
#if defined(_WIN32)
        if (_commit(_fileno(file_)) != 0) throw std::runtime_error("Memory card flush failed");
#else
        if (fsync(fileno(file_)) != 0) throw std::runtime_error("Memory card flush failed");
#endif
        ++durableWrites_;
        if (std::getenv("WIICOMPILED_CARD_TRACE") || std::getenv("CATRAT_CARD_TRACE")) {
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - traceStarted).count();
            std::fprintf(stderr, "[gc-card] durable-write=%llu offset=%zu length=%zu elapsed-ms=%.3f\n",
                         static_cast<unsigned long long>(durableWrites_), offset, length, elapsedMs);
            std::fflush(stderr);
        }
    }
    FILE* file_ = nullptr;
    uint64_t persistedGeneration_ = 0;
    uint64_t durableWrites_ = 0;
    uint64_t pendingWriteGeneration_ = 0;
    uint64_t idleObservedGeneration_ = 0;
    size_t dirtyStart_ = 0, dirtyEnd_ = 0;
    bool batchWrites_ = false;
    std::vector<uint8_t> data_;
    std::array<uint8_t, 128> program_{};
    uint32_t position_ = 0, address_ = 0;
    uint8_t command_ = 0, status_ = 0x41;
    bool present_ = false, interruptEnabled_ = false, interruptPending_ = false, dmaWrite_ = false;
};
