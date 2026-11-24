// wa_coro.cpp - Clean-room stackful coroutine host bridge for Worms Armageddon.
// WA implements cooperative coroutines (AI/turn tasks) using setjmp/longjmp
// with stack-alloca offsets (0x00564760 resume, 0x00564730 yield). Because
// host C longjmp cannot resume a yielded stack frame after returning to the
// caller, we run each coroutine on its own host thread with a guest heap stack,
// strictly ping-ponging execution so caller and coroutine never run concurrently.

#include "x86.h"
#include "memory.h"
#include "imports.h"

#if __has_include("game_config.h")
#include "game_config.h"
#endif

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

constexpr uint32_t kCoroResumeAddr = 0x00564760u;
constexpr uint32_t kCoroYieldAddr  = 0x00564730u;

struct WaCoro {
    enum Turn { CALLER, WORKER };
    std::mutex mu;
    std::condition_variable cv;
    Turn turn = CALLER;
    bool finished = false;
    uint32_t coro_addr = 0;
    uint32_t alloc_addr = 0;
    X86 worker_cpu{};
    std::thread th;
};

std::mutex g_coros_mu;
std::unordered_map<uint32_t, std::shared_ptr<WaCoro>> g_coros;

std::shared_ptr<WaCoro> find_coro(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_coros_mu);
    auto it = g_coros.find(addr);
    return it != g_coros.end() ? it->second : nullptr;
}

void coro_worker_main(std::shared_ptr<WaCoro> co, uint32_t run_fn) {
    {
        std::unique_lock<std::mutex> lk(co->mu);
        co->cv.wait(lk, [&] { return co->turn == WaCoro::WORKER; });
    }

    recomp_call(&co->worker_cpu, run_fn);

    wr32(co->coro_addr + 0xb8u, 0u);
    wr32(co->coro_addr + 0x34u, 2u);
    {
        std::lock_guard<std::mutex> lk(co->mu);
        co->finished = true;
        co->turn = WaCoro::CALLER;
    }
    co->cv.notify_one();
}

void hook_coro_resume(X86 *c, uint32_t /*index*/) {
    uint32_t ret_eip = rd32(c->r[R_ESP]);
    uint32_t coro = rd32(c->r[R_ESP] + 4u);
    c->r[R_ESP] += 8u; // RET 0x4
    c->eip = ret_eip;

    uint32_t state = rd32(coro + 0x34u);
    if (state == 2u) {
        c->r[R_EAX] = 0u;
        recomp_return(c);
        return;
    }
    if (state != 0u && state != 1u) {
        wr32(coro + 0x34u, 2u);
        c->r[R_EAX] = 1u;
        recomp_return(c);
        return;
    }

    std::shared_ptr<WaCoro> co;
    if (state == 0u) {
        std::shared_ptr<WaCoro> old;
        {
            std::lock_guard<std::mutex> lk(g_coros_mu);
            auto it = g_coros.find(coro);
            if (it != g_coros.end()) {
                old = it->second;
                g_coros.erase(it);
            }
        }
        if (old && old->th.joinable()) {
            if (old->finished)
                old->th.join();
            else
                old->th.detach();
        }

        uint32_t pages = rd32(coro + 0xc4u);
        uint32_t stack_bytes = std::max(pages * 0x2000u, 0x20000u);
        uint32_t teb_bytes = 0x100u;
        uint32_t alloc = heap_alloc(stack_bytes + teb_bytes);
        if (!alloc) {
            LOGW("wa_coro: heap_alloc failed for coro %08x", coro);
            wr32(coro + 0x34u, 2u);
            c->r[R_EAX] = 1u;
            recomp_return(c);
            return;
        }

        uint32_t stack_lo = alloc;
        uint32_t stack_hi = (alloc + stack_bytes) & ~15u;
        uint32_t teb = alloc + stack_bytes;
        if (c->fs_base && gm_valid(c->fs_base, teb_bytes))
            memcpy(gm_ptr(teb), gm_ptr(c->fs_base), teb_bytes);
        else
            memset(gm_ptr(teb), 0, teb_bytes);
        wr32(teb + 0x00u, 0xffffffffu);
        wr32(teb + 0x04u, stack_hi);
        wr32(teb + 0x08u, stack_lo);
        wr32(teb + 0x18u, teb);

        co = std::make_shared<WaCoro>();
        co->coro_addr = coro;
        co->alloc_addr = alloc;
        co->worker_cpu = *c;
        co->worker_cpu.fs_base = teb;
        co->worker_cpu.r[R_ESP] = stack_hi - 4u;
        wr32(co->worker_cpu.r[R_ESP], 0u);
        co->worker_cpu.r[R_EBP] = 0u;
        co->worker_cpu.r[R_ECX] = coro;

        wr32(coro + 0x34u, 1u);
        wr32(coro + 0xb8u, 1u);

        uint32_t vtbl = rd32(coro);
        uint32_t run_fn = rd32(vtbl + 0x1cu);

        {
            std::lock_guard<std::mutex> lk(g_coros_mu);
            g_coros[coro] = co;
        }
        co->th = std::thread(coro_worker_main, co, run_fn);
    } else {
        co = find_coro(coro);
        if (!co) {
            LOGW("wa_coro: resume on unknown coro %08x (state=1)", coro);
            wr32(coro + 0x34u, 2u);
            c->r[R_EAX] = 1u;
            recomp_return(c);
            return;
        }
        wr32(coro + 0xb8u, 1u);
    }

    {
        std::lock_guard<std::mutex> lk(co->mu);
        co->turn = WaCoro::WORKER;
    }
    co->cv.notify_one();

    bool done = false;
    {
        std::unique_lock<std::mutex> lk(co->mu);
        co->cv.wait(lk, [&] { return co->turn == WaCoro::CALLER; });
        done = co->finished;
    }

    wr32(coro + 0xb8u, 0u);
    if (done) {
        if (co->th.joinable())
            co->th.join();
        if (co->alloc_addr)
            heap_free(co->alloc_addr);
        std::lock_guard<std::mutex> lk(g_coros_mu);
        auto it = g_coros.find(coro);
        if (it != g_coros.end() && it->second == co)
            g_coros.erase(it);
    }

    c->r[R_EAX] = 1u;
    recomp_return(c);
}

void hook_coro_yield(X86 *c, uint32_t /*index*/) {
    uint32_t ret_eip = rd32(c->r[R_ESP]);
    uint32_t coro = rd32(c->r[R_ESP] + 4u);
    c->r[R_ESP] += 8u; // RET 0x4
    c->eip = ret_eip;

    auto co = find_coro(coro);
    if (!co) {
        LOGW("wa_coro: yield on unknown coro %08x", coro);
        recomp_return(c);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(co->mu);
        co->turn = WaCoro::CALLER;
    }
    co->cv.notify_one();

    {
        std::unique_lock<std::mutex> lk(co->mu);
        co->cv.wait(lk, [&] { return co->turn == WaCoro::WORKER; });
    }
    recomp_return(c);
}

struct WaCoroInstaller {
    WaCoroInstaller() {
#ifdef RECOMP_GAME_ID
        if (strcmp(RECOMP_GAME_ID, "wa") != 0)
            return;
#endif
        int32_t r_idx = recomp_index_of(kCoroResumeAddr);
        int32_t y_idx = recomp_index_of(kCoroYieldAddr);
        if (r_idx >= 0 && y_idx >= 0) {
            recomp_hook_ptrs[r_idx] = hook_coro_resume;
            __atomic_store_n(&recomp_hooked[r_idx], 1u, __ATOMIC_RELEASE);
            recomp_hook_ptrs[y_idx] = hook_coro_yield;
            __atomic_store_n(&recomp_hooked[y_idx], 1u, __ATOMIC_RELEASE);
        }
    }
} g_wa_coro_installer;

} // namespace
