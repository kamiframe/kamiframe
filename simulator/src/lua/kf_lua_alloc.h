/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Lua's heap, as a suballocator over ONE arena block.
 *
 * kf_arena_alloc() (kf/arena.h) hands out fixed blocks once and never takes
 * them back -- correct for the framebuffer, wrong for Lua. Lua's garbage
 * collector allocates and frees constantly as part of ordinary operation
 * (every short-lived string, every table that grows or shrinks), so a bump
 * allocator with no free would exhaust KF_ARENA_LUA_BYTES within the first
 * few hundred frames of any script that so much as concatenates a string.
 *
 * The fix, same shape LVGL already uses for KF_ARENA_LVGL (see
 * kf_lvgl_pool.cpp): call kf_arena_alloc() exactly ONCE, for the whole
 * arena, and run a real allocator -- with free and realloc -- inside that
 * one block. This file is that allocator: a boundary-tag (header + footer)
 * first-fit free list with immediate coalescing, handed to Lua as its
 * lua_Alloc. See docs/architecture/adr-0014-lua-embedding.md for why a
 * hand-rolled allocator was worth writing rather than vendoring a third
 * dependency for it.
 *
 * Not thread-safe. Nothing in this project is; the frame loop is
 * single-threaded end to end.
 */

#ifndef KF_LUA_ALLOC_H
#define KF_LUA_ALLOC_H

#include <cstddef>
#include <cstdint>

/* Acquires KF_ARENA_LUA_BYTES from KF_ARENA_LUA (kf/arena.h) and initialises
 * the free list kf_lua_alloc() below suballocates from. Call once, before
 * lua_newstate(). Safe to call again after kf_lua_alloc_shutdown() --
 * kf_lua_port.h's own contract promises a caller can retry
 * kf_lua_port_init() with different source after a
 * kf_lua_port_shutdown(), and this is what makes that true for the
 * allocator specifically: the underlying arena block is acquired from
 * kf_arena_alloc() (a bump allocator, no free -- see kf/arena.h) only
 * once, ever; every later re-init reuses that SAME block and just resets
 * its free list, rather than trying to carve a second
 * KF_ARENA_LUA_BYTES-sized block out of an arena sized for exactly one. */
void kf_lua_alloc_init();

/* Marks the allocator inactive so kf_lua_alloc_init() can be called again.
 * Does NOT release the underlying arena block back to KF_ARENA_LUA -- see
 * kf_lua_alloc_init()'s comment for why that would not even be possible. A
 * script that leaked memory the GC never reclaimed is simply gone along
 * with the rest of that Lua VM's state; the next kf_lua_alloc_init() starts
 * the free list over from one whole block regardless. */
void kf_lua_alloc_shutdown();

/* Matches lua_Alloc's signature exactly (see lua.h's typedef) without this
 * header depending on Lua -- kf_lua_port.cpp is the only caller, and it
 * already includes lua.h, where the cast to lua_Alloc happens.
 *
 * Lua's contract (see the manual's description of lua_Alloc), and what this
 * implements:
 *   nsize == 0                    : free ptr (may be NULL). Always succeeds.
 *   ptr == NULL, nsize > 0        : fresh allocation. MAY return NULL.
 *   nsize <= current block size   : shrink in place. MUST NOT fail, and
 *                                    does not: no new memory is needed.
 *   nsize >  current block size   : grow. Tries to extend into a free
 *                                    neighbour first; falls back to
 *                                    allocate + copy + free. MAY return
 *                                    NULL if the arena has no room, which
 *                                    Lua surfaces as a catchable
 *                                    "not enough memory" script error, not
 *                                    a crash.
 * Getting the "must not fail when shrinking" half wrong is the kind of bug
 * that only shows up as an intermittent VM corruption under memory
 * pressure -- see the comment in the .cpp where the shrink path is
 * implemented for exactly how that is guaranteed here. */
void *kf_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* For the constraint HUD and the headless stress check: bytes and block
 * count currently live (i.e. handed to Lua and not yet freed), not
 * counting header/footer overhead. Walks the free list, so O(blocks) --
 * call it for reporting, not from a hot path. */
struct kf_lua_alloc_stats {
    size_t live_bytes;
    size_t live_blocks;
};
kf_lua_alloc_stats kf_lua_alloc_get_stats();

#endif /* KF_LUA_ALLOC_H */
