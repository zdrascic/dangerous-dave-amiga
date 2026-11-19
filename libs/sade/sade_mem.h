#pragma once

#include "sade.h"

// SADE Memory Pools — deterministic level-lifetime allocation
// (AMOS: Reserve As Work / Erase).
//
// A MemPool is one big AllocMem block carved up by a bump allocator: no
// per-object bookkeeping, no fragmentation, and PoolReset() reclaims
// EVERYTHING in O(1). The intended shape for a multi-level game:
//
//   program lifetime : screens, fonts, the player's Bobs   → plain AllocMem
//   level lifetime   : tilesets, maps, canvases, banks,
//                      the current music module             → pools
//
// PoolActivate() routes the SADE asset headers' allocations (sade_ilbm,
// sade_bob, sade_bank, sade_tilemap — via the SadeAlloc/SadeFree hooks in
// sade.h) into a pool, one active pool per memory type:
//
//   MemPool *chipPool = PoolCreate(300 * 1024, MEMF_CHIP);
//   MemPool *pubPool  = PoolCreate( 16 * 1024, MEMF_PUBLIC);
//   PoolActivate(chipPool);
//   PoolActivate(pubPool);
//
//   // per level — no individual frees, ever:
//   ModStop();                     // nothing may still USE pool memory!
//   PoolReset(chipPool);
//   PoolReset(pubPool);
//   sheet  = IlbmLoad(...);        // lands in chipPool (+ pubPool structs)
//   ...decode the rest of the level...
//
// While a pool is active, the matching Free calls (IlbmFree, BobFree, ...)
// become no-ops for pool memory — they are safe to call but pointless;
// PoolReset is the real deallocator. Anything allocated BEFORE
// PoolActivate (or after PoolDeactivate) behaves exactly as before.
//
// Rule: after PoolReset, every pointer previously handed out by that pool
// is invalid. Stop the consumers first (ModStop, drop Bob image pointers,
// re-render), then reset.

typedef struct MemPool {
    struct MemPool *next;     // registry link (for the free hook)
    UBYTE *base;
    ULONG  size;
    ULONG  used;
    ULONG  memFlags;          // MEMF_CHIP or MEMF_PUBLIC (+ anything else)
} MemPool;

static MemPool *_sade_poolList   = NULL;  // all live pools
static MemPool *_sade_poolChip   = NULL;  // active capture pool, chip
static MemPool *_sade_poolPublic = NULL;  // active capture pool, non-chip

// -------------------------------------------------------------------------
// Manual allocation: bump the pool pointer. 8-byte aligned like AllocMem
// (word alignment is what the blitter/Paula need). Memory is NOT cleared.
// Returns NULL when the pool is full.
// -------------------------------------------------------------------------
static void *PoolAlloc(MemPool *p, ULONG size) {
    size = (size + 7) & ~7UL;
    if (p->used + size > p->size) return NULL;
    void *r = p->base + p->used;
    p->used += size;
    return r;
}

static inline void PoolReset(MemPool *p) { p->used = 0; }

static inline ULONG PoolAvail(const MemPool *p) { return p->size - p->used; }

// -------------------------------------------------------------------------
// The SadeAlloc/SadeFree hooks (see sade.h)
// -------------------------------------------------------------------------
static APTR _PoolHookAlloc(ULONG size, ULONG flags) {
    MemPool *p = (flags & MEMF_CHIP) ? _sade_poolChip : _sade_poolPublic;
    if (!p) return NULL;                       // fall back to AllocMem
    void *r = PoolAlloc(p, size);
    if (r && (flags & MEMF_CLEAR)) memset(r, 0, size);
    return r;                                  // NULL when full → AllocMem
}

static int _PoolHookFree(APTR ptr, ULONG size) {
    (void)size;
    for (MemPool *p = _sade_poolList; p; p = p->next)
        if ((UBYTE*)ptr >= p->base && (UBYTE*)ptr < p->base + p->size)
            return 1;                          // pool memory: no-op
    return 0;                                  // not ours: real FreeMem
}

// -------------------------------------------------------------------------
// PoolCreate / PoolFree
// -------------------------------------------------------------------------
static MemPool *PoolCreate(ULONG size, ULONG memFlags) {
    MemPool *p = (MemPool*)AllocMem(sizeof(MemPool), MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) return NULL;
    p->base = (UBYTE*)AllocMem(size, memFlags);
    if (!p->base) { FreeMem(p, sizeof(MemPool)); return NULL; }
    p->size     = size;
    p->memFlags = memFlags;

    p->next = _sade_poolList;                  // register for the free hook
    _sade_poolList  = p;
    _sade_allocHook = _PoolHookAlloc;
    _sade_freeHook  = _PoolHookFree;
    return p;
}

static void PoolDeactivate(MemPool *p) {
    if (_sade_poolChip   == p) _sade_poolChip   = NULL;
    if (_sade_poolPublic == p) _sade_poolPublic = NULL;
}

// Route SADE asset allocations of the pool's memory type into it.
static void PoolActivate(MemPool *p) {
    if (p->memFlags & MEMF_CHIP) _sade_poolChip   = p;
    else                         _sade_poolPublic = p;
}

static void PoolFree(MemPool *p) {
    if (!p) return;
    PoolDeactivate(p);
    for (MemPool **pp = &_sade_poolList; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    if (!_sade_poolList) {                     // last pool gone: unhook
        _sade_allocHook = NULL;
        _sade_freeHook  = NULL;
    }
    FreeMem(p->base, p->size);
    FreeMem(p, sizeof(MemPool));
}
