#include "gamecube_memory_card.h"
#include <chrono>
#include <iostream>

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void Command(GameCubeMemoryCard& card, std::initializer_list<uint8_t> bytes) {
    card.Select(true);
    for (uint8_t byte : bytes) card.Transfer(byte);
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("catrat-card-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = root / "CardA.raw";
    try {
        std::array<uint8_t, 128> payload{};
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i ^ 0xA5);
        {
            GameCubeMemoryCard card;
            card.Open(path);
            Require(card.Present(), "Card not present");
            Require(card.Data().size() == 2 * 1024 * 1024, "Wrong card size");
            Require(BigEndian::Read16(card.Data().data() + 34) == 16, "Wrong header size");
            Require(BigEndian::Read16(card.Data().data() + 3 * 8192 + 6) == 251, "Wrong free blocks");
            const auto flash = card.FlashId();
            Require(std::memcmp(flash.data(), "DOLPHINSLOTA", 12) == 0, "Flash serial mismatch");
            bool locked = false;
            try { GameCubeMemoryCard second; second.Open(path); }
            catch (const std::runtime_error&) { locked = true; }
            Require(locked, "A second writer could corrupt the card");
            Command(card, {0, 0});
            uint32_t id = 0;
            for (int i = 0; i < 4; ++i) id = (id << 8) | card.Transfer(0);
            card.Select(false);
            Require(id == 16, "EXI card ID incorrect");
            Command(card, {0x81, 1}); card.Select(false);
            Command(card, {0xF2, 0, 0x50, 0, 0}); // First file block: 0xA000
            card.DmaWrite(payload.data(), payload.size());
            card.Select(false);
            Require(card.InterruptPending(), "Program completion interrupt missing");
            Command(card, {0x89}); card.Select(false);
            Require(!card.InterruptPending(), "Clear status did not acknowledge command");
        }
        {
            GameCubeMemoryCard card; card.Open(path);
            Command(card, {0x52, 0, 0x50, 0, 0, 0, 0, 0, 0});
            std::array<uint8_t, 8192> read{};
            card.DmaRead(read.data(), read.size()); card.Select(false);
            Require(std::equal(payload.begin(), payload.end(), read.begin()), "Save did not survive reopen");
            Require(read[512] == 0xFF, "DMA incorrectly wraps every 512 bytes");
            Command(card, {0xF1, 0, 0x50}); card.Select(false);
        }
        {
            GameCubeMemoryCard card; card.Open(path);
            Require(card.Data()[0xA000] == 0xFF, "Erase did not survive reopen");
        }
        std::filesystem::remove_all(root);
        std::cout << "Card probe, DMA, interrupts, exclusive writer and persistence passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (test files: " << root << ")\n";
        return 1;
    }
}
