#include "../../psp/midi/MidiFailurePolicy.hpp"
#include "../../psp/midi/MidiExitPolicy.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

int main()
{
    using th06midi::MeBorrowedPayloads;
    std::vector<unsigned char> current(2048), next(3000), unused(4000);
    MeBorrowedPayloads reads;
    assert(reads.canPlay(current.data()));
    assert(reads.play(current.data(), current.size()));
    reads.applied();
    assert(reads.play(next.data(), next.size()));
    // Success with an old buffered PCM remainder does NOT prove consumption:
    // the adapter only calls applied() when elapsed changes.
    assert(reads.contains(current.data()) && reads.contains(next.data()));
    assert(!reads.contains(unused.data()));
    reads.freeze();
    reads.applied(); reads.stop(); reads.reinitialized();
    assert(reads.contains(current.data()) && reads.contains(next.data()));
    assert(reads.bytes() == 5048u && !reads.canPlay(unused.data()));
    assert(!reads.play(unused.data(), unused.size()));

    MeBorrowedPayloads stopPlay;
    assert(stopPlay.play(current.data(), current.size()));
    stopPlay.applied();
    stopPlay.stop();
    assert(stopPlay.play(next.data(), next.size()));
    stopPlay.applied();
    assert(!stopPlay.contains(current.data()) && stopPlay.contains(next.data()));
    stopPlay.stop(); stopPlay.applied();
    assert(stopPlay.bytes() == 0 && !stopPlay.contains(next.data()));

    unsigned char slots[MeBorrowedPayloads::kCapacity + 1]{};
    MeBorrowedPayloads full;
    for (unsigned i = 0; i < MeBorrowedPayloads::kCapacity; ++i)
        assert(full.play(&slots[i], i + 1u));
    assert(!full.canPlay(&slots[MeBorrowedPayloads::kCapacity]));
    assert(!full.play(&slots[MeBorrowedPayloads::kCapacity], 999));
    full.applied();
    assert(full.contains(&slots[MeBorrowedPayloads::kCapacity - 1]));
    assert(!full.contains(&slots[MeBorrowedPayloads::kCapacity]));
    // Two exit/shutdown callers must recheck started under exclusive owner,
    // and publish started=0 before unlock; the residual STOP command is not
    // a reason to poison the second caller.
    std::mutex owner;
    bool started = true, poisoned = false;
    unsigned stops = 0;
    auto shutdown = [&] {
        std::lock_guard<std::mutex> lock(owner);
        if (th06_me_exit_action(false, started, poisoned) == TH06_ME_EXIT_STOP)
        {
            ++stops;
            started = false;
        }
    };
    std::thread first(shutdown), second(shutdown);
    first.join(); second.join();
    assert(stops == 1 && !poisoned && !started);
    assert(th06_me_exit_action(true, false, false) == TH06_ME_EXIT_HOLD);
    assert(th06_me_exit_action(false, true, true) == TH06_ME_EXIT_HOLD);
    assert(th06_me_exit_action(false, false, false) == TH06_ME_EXIT_NONE);
    // Pre-start shutdown prevents entry; shutdown after start-publication
    // is HOLD, never a false NOWORK/normal exit.
    assert(th06_me_exit_action(true, true, false) == TH06_ME_EXIT_HOLD);
    for (unsigned trial = 0; trial < 1000; ++trial)
    {
        std::atomic<bool> initializing{false}, shuttingDown{false};
        bool initSawShutdown = false, exitSawInit = false;
        std::thread init([&] { initializing.store(true); initSawShutdown = shuttingDown.load(); });
        std::thread exit([&] { shuttingDown.store(true); exitSawInit = initializing.load(); });
        init.join(); exit.join();
        assert(initSawShutdown || exitSawInit); // SC handshake forbids both seeing 0
    }
    std::puts("adapter read-set ACK/unacked/STOP-PLAY/freeze/capacity-before-push controls OK");
}
