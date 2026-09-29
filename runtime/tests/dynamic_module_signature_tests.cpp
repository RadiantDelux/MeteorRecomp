#if defined(NDEBUG)
#error "Signature tests require enabled assertions"
#endif
#include "dynamic_module_signature.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main() {
    assert(DynamicModule::MatchMaskedImage(nullptr, nullptr, nullptr, 0));
    std::mt19937 rng(12345);
    std::array<uint8_t, 320> actual{}, expected{}, mask{};
    for (size_t alignment = 0; alignment < 16; ++alignment) {
        for (size_t length = 1; length <= 257; ++length) {
            for (size_t at = 0; at < length; ++at) {
                const auto i = at + alignment;
                expected[i] = static_cast<uint8_t>(rng());
                mask[i] = static_cast<uint8_t>(rng());
                actual[i] = expected[i] ^ (static_cast<uint8_t>(rng()) & ~mask[i]);
            }
            const auto matches = [&] {
                return DynamicModule::MatchMaskedImage(actual.data()+alignment,
                    expected.data()+alignment, mask.data()+alignment, length);
            };
            assert(matches());
            // Every bit in every byte, including vector boundaries and tails.
            for (size_t at = 0; at < length; ++at) {
                for (uint8_t bit = 1; bit != 0; bit = static_cast<uint8_t>(bit << 1)) {
                    actual[at+alignment] ^= bit;
                    assert(matches() == ((mask[at+alignment] & bit) == 0));
                    actual[at+alignment] ^= bit;
                }
            }
        }
    }
    const uint8_t be[] = {0x00,0x12,0x34,0x56,0x78,0x9a};
    assert(DynamicModule::ReadImage32(be,1) == 0x12345678u);
    assert(DynamicModule::ReadImage16(be,3) == 0x5678u);
#ifdef _WIN32
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto page = static_cast<size_t>(system.dwPageSize);
    auto* buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    assert(buffer != nullptr);
    DWORD old = 0; assert(VirtualProtect(buffer+page,page,PAGE_NOACCESS,&old));
    for (size_t length = 0; length < 128; ++length) {
        std::vector<uint8_t> ones(length,0xffu);
        auto* last = buffer + page - length;
        std::memset(last,0xff,length);
        assert(DynamicModule::MatchMaskedImage(last,ones.data(),ones.data(),length));
        assert(DynamicModule::MatchMaskedImage(ones.data(),last,ones.data(),length));
        assert(DynamicModule::MatchMaskedImage(ones.data(),ones.data(),last,length));
    }
    assert(VirtualFree(buffer,0,MEM_RELEASE));
#endif
    std::puts("PASS: every masked bit, 16 alignments, vector tails, BE fields and inaccessible next page");
}
