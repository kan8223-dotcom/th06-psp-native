// Linked static negative/positive oracle only. NEVER RUN, no EBOOT.
using u8 = unsigned char;
using u32 = unsigned int;
using Fn = void (*)(void *, u32, const u8 *, u32);
extern "C" __attribute__((noinline)) void IntegerSink(void *p, u32 t, const u8 *m, u32 n)
{ asm volatile("" : : "r"(p), "r"(t), "r"(m), "r"(n) : "memory"); }
Fn volatile gOutput = IntegerSink;
namespace tdls {
struct ZunSequencer {
    __attribute__((noinline)) void send(u8 a, u8 b, u8 c);
    __attribute__((noinline)) void processMsg(const u8 *m, u32 n);
};
void ZunSequencer::send(u8 a, u8 b, u8 c)
{
    const u8 m[3] = {a,b,c};
#if GATE_INDIRECT
    gOutput(this, 1, m, 3);
#else
    IntegerSink(this, 1, m, 3);
#endif
}
void ZunSequencer::processMsg(const u8 *m, u32 n)
{
#if GATE_INDIRECT
    gOutput(this, 2, m, n);
#else
    IntegerSink(this, 2, m, n);
#endif
}
}
extern "C" __attribute__((noinline)) void meLibOnProcess()
{
    tdls::ZunSequencer s;
    const u8 m[3] = {0x90,60,100};
#if !GATE_TAIL
    s.send(m[0],m[1],m[2]);
#endif
    s.processMsg(m,3); // GATE_TAIL defaults to direct j -> method's indirect jr
}
