#pragma once
#include "ax_internal.h"

namespace AxDspHle {
// Retail GC AX (e2136399): 122 halfwords, five update counts, a stream
// loop counter. Layout reference: Dolphin DSPHLE/UCodes/AXStructs.h.
using GameCubePB = std::array<uint16_t, 122>;
template <typename T>
inline void GcPBField(GameCubePB& raw, unsigned word, T& field, bool encode) {
    if (encode) std::memcpy(raw.data() + word, &field, sizeof(T));
    else std::memcpy(&field, raw.data() + word, sizeof(T));
}
inline void ConvertGameCubePB(GameCubePB& raw, AXPBWii& pb, bool encode) {
#define GC_FIELD(word, field) GcPBField(raw, word, pb.field, encode)
    GC_FIELD(0, next_pb_hi); GC_FIELD(1, next_pb_lo);
    GC_FIELD(2, this_pb_hi); GC_FIELD(3, this_pb_lo);
    GC_FIELD(4, src_type); GC_FIELD(5, coef_select);
    GC_FIELD(7, running); GC_FIELD(8, is_stream);
    GC_FIELD(9, mixer.main_left); GC_FIELD(11, mixer.main_right);
    GC_FIELD(13, mixer.auxA_left); GC_FIELD(15, mixer.auxA_right);
    GC_FIELD(17, mixer.auxB_left); GC_FIELD(19, mixer.auxB_right);
    GC_FIELD(21, mixer.auxB_surround); GC_FIELD(23, mixer.main_surround);
    GC_FIELD(25, mixer.auxA_surround); GC_FIELD(27, initial_time_delay);
    GC_FIELD(41, dpop.main_left); GC_FIELD(42, dpop.auxA_left);
    GC_FIELD(43, dpop.auxB_left); GC_FIELD(44, dpop.main_right);
    GC_FIELD(45, dpop.auxA_right); GC_FIELD(46, dpop.auxB_right);
    GC_FIELD(47, dpop.main_surround); GC_FIELD(48, dpop.auxA_surround);
    GC_FIELD(49, dpop.auxB_surround); GC_FIELD(50, vol_env);
    GC_FIELD(55, audio_addr); GC_FIELD(63, adpcm); GC_FIELD(83, src);
    GC_FIELD(90, adpcm_loop_info); GC_FIELD(93, lpf);
#undef GC_FIELD
}
inline AXMixControl GameCubeMixControl(uint16_t c) {
    uint32_t out = 0;
    if (c & 1) out |= MIX_MAIN_L;
    if (c & 2) out |= MIX_MAIN_R;
    if (c & 4) out |= MIX_MAIN_S;
    if (c & 8) out |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
    if (c & 0x10) out |= MIX_AUXA_L;
    if (c & 0x20) out |= MIX_AUXA_R;
    if (c & 0x40) out |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (c & 0x80) out |= MIX_AUXA_S;
    if (c & 0x100) out |= MIX_AUXA_S_RAMP;
    if (c & 0x200) out |= MIX_AUXB_L;
    if (c & 0x400) out |= MIX_AUXB_R;
    if (c & 0x800) out |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (c & 0x1000) out |= MIX_AUXB_S;
    if (c & 0x2000) out |= MIX_AUXB_S_RAMP;
    return static_cast<AXMixControl>(out);
}
} // namespace AxDspHle
