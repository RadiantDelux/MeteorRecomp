#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace MeteorWiiExtensionCrypto {
struct KeyTables {
    std::array<uint8_t, 8> ft{};
    std::array<uint8_t, 8> sb{};
};

bool GenerateFirstPartyKeyTables(const std::array<uint8_t, 16>& extensionKey, KeyTables& out);
void Encrypt(uint8_t* data, uint32_t address, std::size_t size, const KeyTables& tables);
} // namespace MeteorWiiExtensionCrypto
