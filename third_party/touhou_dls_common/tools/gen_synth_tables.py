#!/usr/bin/env python3
"""合成器の定数表（src/synth_tables.cpp）を作る。実行時は整数だけで済むよう、
浮動小数はここ（PC の生成時）でだけ使う。出来た表は全部整数なので PC/PSP/PS3 で同じ値になる。

  python3 tools/gen_synth_tables.py > src/synth_tables.cpp

表の式:
  kPow2Cents[i]    = round(2^30 * 2^(i/1200))          i = 0..1199   （音程・時間の 2 のべき）
  kPow2Neg2048[i]  = round(32768 * 2^(-i/2048))         i = 0..2047   （減衰 → 振幅）
  kLog2Mant256[i]  = round(65536 * log2(1 + i/256))     i = 0..255    （振幅 → 減衰、アタック中だけ）
  kSine256[i]      = round(16384 * sin(2*pi*i/256))     i = 0..255    （LFO）
  kMidiToVrel[i]   = trunc(4000 * log10(i/127)), [0] = -9600   （ベロシティ・CC7・CC11、1/100 dB）
  kPanToVrel[i]    = trunc(1000 * log10(i/127)), [0] = -2500   （パン、1/100 dB）
最後の 2 つは Microsoft の WDK サンプル ddksynth（DirectMusic ソフトシンセ）の表のコメントに
書かれた式と同じ（式から作り直したもの。切り捨ては 0 方向）。
"""
import math


def rnd(x):
    return int(math.floor(x + 0.5))


def trunc0(x):
    return int(x)  # 0 方向の切り捨て


def emit(name, ctype, vals, per=8):
    out = ['const %s %s[%d] = {' % (ctype, name, len(vals))]
    for i in range(0, len(vals), per):
        out.append('    ' + ', '.join(str(v) for v in vals[i:i + per]) + ',')
    out.append('};')
    return '\n'.join(out)


def main():
    pow2c = [rnd((1 << 30) * 2.0 ** (i / 1200.0)) for i in range(1200)]
    assert pow2c[0] == 1 << 30 and pow2c[-1] < 1 << 31
    pow2n = [rnd(32768 * 2.0 ** (-i / 2048.0)) for i in range(2048)]
    assert pow2n[0] == 32768 and min(pow2n) > 16384
    log2m = [rnd(65536 * math.log2(1 + i / 256.0)) for i in range(256)]
    assert log2m[-1] < 65536
    sine = [rnd(16384 * math.sin(2 * math.pi * i / 256.0)) for i in range(256)]
    midi = [-9600] + [trunc0(4000 * math.log10(i / 127.0)) for i in range(1, 128)]
    pan = [-2500] + [trunc0(1000 * math.log10(i / 127.0)) for i in range(1, 128)]
    # Generic mathematical tables, generated independently of gm.dls / wdmaud.
    dbvol = [trunc0(4095 * 10.0 ** ((i - 1000) / 200.0)) for i in range(1001)]
    semitones = [trunc0(4096 * 2.0 ** ((i - 48) / 12.0)) for i in range(97)]
    cents = [trunc0(4096 * 2.0 ** ((i - 100) / 1200.0)) for i in range(201)]
    attack = [0] + [trunc0(1000 + 2000 * math.log10(i / 200.0) / 9.6) for i in range(1, 201)]
    sine100 = [trunc0(100 * math.sin(2 * math.pi * i / 256.0)) for i in range(256)]
    exp2frac = [rnd((1 << 48) * 2.0 ** (2.0 ** (-i))) for i in range(1, 33)]
    print('// 自動生成: tools/gen_synth_tables.py（手で直さない）。式は生成スクリプトの冒頭を参照。')
    print('#include "tdls/synth_tables.hpp"')
    print('')
    print('namespace tdls {')
    print('namespace tables {')
    print('')
    print(emit('kPow2Cents', 'u32', pow2c, 6))
    print('')
    print(emit('kPow2Neg2048', 'u16', pow2n, 12))
    print('')
    print(emit('kLog2Mant256', 'u16', log2m, 12))
    print('')
    print(emit('kSine256', 's16', sine, 12))
    print('')
    print(emit('kMidiToVrel', 's16', midi, 12))
    print('')
    print(emit('kPanToVrel', 's16', pan, 12))
    for name, ctype, vals in [('kDbToVolume', 'u16', dbvol), ('kSemiTones', 'u32', semitones),
                              ('kCents', 'u16', cents), ('kAttack', 'u16', attack),
                              ('kSine100', 's16', sine100)]:
        print('')
        print(emit(name, ctype, vals, 12))
    print('')
    print(emit('kExp2Frac48', 'u64', [str(v) + 'ull' for v in exp2frac], 4))
    print('')
    print('} // namespace tables')
    print('} // namespace tdls')


if __name__ == '__main__':
    main()
