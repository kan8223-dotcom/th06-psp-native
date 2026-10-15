// 合成器の定数表（中身は tools/gen_synth_tables.py が作る src/synth_tables.cpp）。
// 実行時に浮動小数を使わないための表。値は全部整数で、どの機種でも同じ。
#ifndef TDLS_SYNTH_TABLES_HPP
#define TDLS_SYNTH_TABLES_HPP

#include "types.hpp"

namespace tdls {
namespace tables {

extern const u32 kPow2Cents[1200];   // Q30: 2^(i/1200)
extern const u16 kPow2Neg2048[2048]; // Q15: 2^(-i/2048)（[0] = 32768）
extern const u16 kLog2Mant256[256];  // Q16: log2(1 + i/256)
extern const s16 kSine256[256];      // Q14: sin(2πi/256)
extern const s16 kMidiToVrel[128];   // 1/100 dB: 4000*log10(i/127)（0 方向切り捨て）、[0] = -9600
extern const s16 kPanToVrel[128];    // 1/100 dB: 1000*log10(i/127)（0 方向切り捨て）、[0] = -2500
extern const u16 kDbToVolume[1001]; // Q12: trunc(4095*10^((i-1000)/200))
extern const u32 kSemiTones[97];    // Q12: trunc(4096*2^((i-48)/12))
extern const u16 kCents[201];       // Q12: trunc(4096*2^((i-100)/1200))
extern const u16 kAttack[201];      // EG attack: amplitude 0..1000 -> dB level 0..1000
extern const s16 kSine100[256];     // LFO: trunc(100*sin(2*pi*i/256))
extern const u64 kExp2Frac48[32];   // Q48: round(2^48 * 2^(2^(-i-1))), for full 16.16 time cents

} // namespace tables
} // namespace tdls

#endif
