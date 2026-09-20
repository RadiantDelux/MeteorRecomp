// Included inside AXWii to reuse its decoder, filters and buses.
// Command reference: Dolphin DSPHLE/UCodes/AX.{h,cpp}.
    void ProcessGameCubePBs(uint32_t address) {
        for (unsigned voices = 0; address && voices < 256; ++voices) {
            if (!MixResolveRange(address, sizeof(GameCubePB)))
                throw std::runtime_error("GameCube AX parameter block outside RAM");
            GameCubePB raw{};
            for (unsigned i = 0; i < raw.size(); ++i) raw[i] = MixRead16(address + i * 2);
            const uint32_t updatesAddress = Hilo(raw[39], raw[40]);
            std::array<PBUpdate, 32> updates{};
            for (unsigned i = 0; updatesAddress && i < updates.size(); ++i)
                updates[i] = {MixRead16(updatesAddress + i * 4), MixRead16(updatesAddress + i * 4 + 2)};
            AXBuffers buffers = MixBuffers();
            unsigned update = 0;
            for (unsigned ms = 0; ms < 5; ++ms) {
                const unsigned count = raw[34 + ms];
                for (unsigned i = 0; i < count && update < updates.size(); ++i, ++update)
                    if (updates[update].pb_offset < raw.size())
                        raw[updates[update].pb_offset] = updates[update].new_value;
                AXPBWii pb{};
                ConvertGameCubePB(raw, pb, false);
                ProcessVoice(pb, buffers, 32, GameCubeMixControl(raw[6]), BaseCoefficients(), false, &raw[97]);
                ConvertGameCubePB(raw, pb, true);
                for (unsigned bus = 0; bus < 9; ++bus) buffers[bus] += 32;
            }
            for (unsigned i = 0; i < raw.size(); ++i) MixWrite16(address + i * 2, raw[i]);
            address = Hilo(raw[0], raw[1]);
        }
    }

    void ExecuteGameCubeCommands() {
        uint32_t index = 0, pbAddress = 0;
        auto word = [&]() -> uint16_t {
            if (index >= m_cmdListSize) throw std::runtime_error("Truncated GameCube AX command");
            return m_cmdList[index++];
        };
        auto address = [&]() { const auto hi = word(); return Hilo(hi, word()); };
        auto upload = [&](uint32_t dest, unsigned bus, unsigned count) {
            for (unsigned ch = 0; ch < count; ++ch)
                WriteGuestS32Buffer(dest + ch * 640, m_bus[bus + ch].data(), 160);
        };
        auto add = [&](uint32_t src, unsigned bus) {
            for (unsigned i = 0; i < 160; ++i)
                m_bus[bus][i] += static_cast<int32_t>(MixRead32(src + i * 4));
        };
        for (unsigned commands = 0; commands < 4096; ++commands) {
            const auto command = word();
            switch (command) {
            case 0: {
                const auto init = address();
                for (unsigned bus = 0; bus < 9; ++bus) {
                    const auto value = static_cast<int32_t>(MixRead32(init + bus * 6));
                    const auto delta = static_cast<int16_t>(MixRead16(init + bus * 6 + 4));
                    for (unsigned i = 0; i < 160; ++i) m_bus[bus][i] = value ? value + i * delta : 0;
                }
                break;
            }
            case 1: {
                const auto src = address();
                const uint16_t volumes[3] = {word(), word(), word()};
                for (unsigned group = 0; group < 3; ++group)
                    for (unsigned ch = 0; ch < 3; ++ch)
                        for (unsigned i = 0; i < 160; ++i)
                            m_bus[group * 3 + ch][i] += static_cast<int32_t>(
                                (int64_t(static_cast<int32_t>(MixRead32(src + ch * 640 + i * 4))) * volumes[group]) >> 15);
                break;
            }
            case 2: pbAddress = address(); break;
            case 3: ProcessGameCubePBs(pbAddress); break;
            case 4: case 5: case 9: {
                const auto dest = command == 9 ? 0 : address(); const auto src = address();
                if (dest) upload(dest, command == 4 ? 3 : 6, 3);
                for (unsigned ch = 0; ch < 3; ++ch) add(src + ch * 640, ch);
                break;
            }
            case 6: upload(address(), 0, 3); break;
            case 7: case 17: {
                const auto src = address();
                for (unsigned i = 0; i < 160; ++i) {
                    const auto value = static_cast<int32_t>(MixRead32(src + i * 4));
                    m_bus[0][i] = command == 17 ? -value : value;
                    m_bus[1][i] = value; m_bus[2][i] = 0;
                }
                break;
            }
            case 8: throw std::runtime_error("Unsupported GameCube AX command 08");
            case 10: case 11: case 12: break;
            case 13: {
                const auto next = address(); const auto length = word();
                CopyCmdList(next, length); m_cmdListSize = length; index = 0; break;
            }
            case 14: {
                const auto surround = address(); const auto output = address();
                OutputSamples(output, surround, 0x8000, false); break;
            }
            case 15: return;
            case 16: {
                const auto dest = address(); const auto src = address(); upload(dest, 6, 2);
                for (unsigned ch = 0; ch < 2; ++ch)
                    for (unsigned i = 0; i < 160; ++i) {
                        const auto value = static_cast<int32_t>(MixRead32(src + ch * 640 + i * 4));
                        m_bus[6 + ch][i] = value; m_bus[ch][i] += value;
                    }
                break;
            }
            case 18: {
                const auto threshold = word(); const auto frames = word();
                RunCompressor(threshold, frames, address()); break;
            }
            case 19: {
                const auto auxA = address(); const auto auxB = address();
                uint32_t inputs[4]; for (auto& input : inputs) input = address();
                upload(auxA, 3, 3); upload(auxB, 8, 1);
                const unsigned buses[4] = {0, 1, 6, 7};
                for (unsigned ch = 0; ch < 4; ++ch) add(inputs[ch], buses[ch]);
                break;
            }
            default: throw std::runtime_error("Unknown GameCube AX command " + std::to_string(command));
            }
        }
        throw std::runtime_error("GameCube AX command chain did not terminate");
    }
