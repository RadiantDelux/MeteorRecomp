#include "gamecube_memory_card.h"
#include <chrono>
#include <fstream>
#include <iostream>

void Check(bool ok) { if (!ok) throw std::runtime_error("Card checkpoint test failed"); }

void Address(GameCubeMemoryCard& card, uint8_t command, uint32_t address) {
    card.Select(true);
    card.Transfer(command);
    card.Transfer(uint8_t(address >> 17));
    card.Transfer(uint8_t(address >> 9));
    card.Transfer(uint8_t((address >> 7) & 3));
    card.Transfer(uint8_t(address & 127));
    if (command == 0x52) for (int i = 0; i < 4; ++i) card.Transfer(0);
}

int main() {
    std::vector<uint8_t> gci(64 + 2 * GameCubeMemoryCard::BlockSize);
    std::copy_n("GTJE5L", 6, gci.begin());
    BigEndian::Write16(gci.data() + 0x38, 2);
    for (size_t i = 64; i < gci.size(); ++i) gci[i] = uint8_t(i * 7);
    const auto imported = GameCubeMemoryCard::FormattedImageWithSave(gci);
    Check(imported.size() == GameCubeMemoryCard::DefaultSize);
    Check(std::equal(gci.begin() + 64, gci.end(), imported.begin() + 5 * GameCubeMemoryCard::BlockSize));
    for (size_t block : {1u, 2u}) {
        const auto* directory = imported.data() + block * GameCubeMemoryCard::BlockSize;
        Check(std::equal(gci.begin(), gci.begin() + 0x36, directory));
        Check(BigEndian::Read16(directory + 0x36) == 5 && BigEndian::Read16(directory + 0x38) == 2);
    }
    for (size_t block : {3u, 4u}) {
        const auto* bat = imported.data() + block * GameCubeMemoryCard::BlockSize;
        Check(BigEndian::Read16(bat + 6) == 249 && BigEndian::Read16(bat + 8) == 6);
        Check(BigEndian::Read16(bat + 10) == 6 && BigEndian::Read16(bat + 12) == 0xFFFF);
        uint16_t sum = 0, inverse = 0;
        for (size_t i = 4; i < GameCubeMemoryCard::BlockSize; i += 2) {
            const auto word = BigEndian::Read16(bat + i);
            sum += word; inverse += uint16_t(~word);
        }
        Check(BigEndian::Read16(bat) == (sum == 0xFFFF ? 0 : sum));
        Check(BigEndian::Read16(bat + 2) == (inverse == 0xFFFF ? 0 : inverse));
    }
    gci.pop_back();
    bool invalid = false;
    try { GameCubeMemoryCard::FormattedImageWithSave(gci); } catch (const std::invalid_argument&) { invalid = true; }
    Check(invalid);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::current_path() / ("card-checkpoint-" + std::to_string(suffix));
    const auto path = directory / "test.raw";
    {
        GameCubeMemoryCard card;
        card.Open(path);
        const auto beforeWrite = card.CaptureState();
        std::array<uint8_t, 512> payload;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7);
        Address(card, 0xF2, 0x10000);
        card.DmaWrite(payload.data(), payload.size());
        card.Select(false);
        card.SetBatchWrites(true);
        const auto priorBatch = card.CaptureState();
        const auto writes = card.DurableWriteCount();
        for (uint32_t page = 0; page < 128; ++page) {
            Address(card, 0xF2, 0x20000 + page * 512);
            card.DmaWrite(payload.data(), payload.size());
            card.Select(false);
        }
        Check(!priorBatch.CanRestore() && !card.CaptureState());
        Check(card.DurableWriteCount() == writes);
        // First quiet boundary only observes the final write generation. A
        // second boundary with no new card program makes the whole burst durable
        // with one host commit.
        card.FlushPendingWritesIfIdle();
        Check(card.DurableWriteCount() == writes);
        card.FlushPendingWritesIfIdle();
        Check(card.DurableWriteCount() == writes + 1 && bool(card.CaptureState()));
        card.FlushPendingWrites();
        Check(card.DurableWriteCount() == writes + 1);
        Check(!beforeWrite.CanRestore() && !beforeWrite.Restore());
        Check(std::equal(payload.begin(), payload.end(), card.Data().begin() + 0x10000));

        Address(card, 0x52, 0x10000);
        card.Transfer(0);
        const auto reading = card.CaptureState();
        std::array<uint8_t, 8> expected;
        for (auto& byte : expected) byte = card.Transfer(0);
        for (int pass = 0; pass < 3; ++pass) {
            Check(reading.Restore());
            for (auto byte : expected) Check(card.Transfer(0) == byte);
        }
        card.Select(false);
    }
    {
        std::ifstream saved(path, std::ios::binary);
        saved.seekg(0x10000);
        for (int i = 0; i < 512; ++i) Check(saved.get() == uint8_t(i * 7));
        saved.seekg(0x20000);
        for (int i = 0; i < 128 * 512; ++i) Check(saved.get() == uint8_t((i % 512) * 7));
    }
    const auto closePath = directory / "close-flush.raw";
    {
        GameCubeMemoryCard card;
        card.Open(closePath);
        card.SetBatchWrites(true);
        std::array<uint8_t, 512> payload;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(0xA5 ^ i);
        Address(card, 0xF2, 0x18000);
        card.DmaWrite(payload.data(), payload.size());
        card.Select(false);
        // Deliberately do not call either flush method. Orderly destruction is
        // the final durability boundary if the process closes immediately after
        // a completed guest CARD write.
    }
    {
        std::ifstream saved(closePath, std::ios::binary);
        saved.seekg(0x18000);
        for (int i = 0; i < 512; ++i) Check(saved.get() == uint8_t(0xA5 ^ i));
    }
    std::filesystem::remove(path);
    std::filesystem::remove(closePath);
    std::filesystem::remove(directory);
    std::cout << "Card checkpoints: protocol cursor replay and refusal to rewind committed disk writes passed\n";
}
