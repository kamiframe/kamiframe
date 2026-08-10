/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_lua_alloc.h for what this is and why it exists.
 *
 * Layout: one KF_ARENA_LUA_BYTES block from kf_arena_alloc(), divided into
 * variable-size chunks, each
 *
 *     [Tag header][payload...][Tag footer]
 *
 * Tag is 16 bytes (payload size + a free flag, padded to keep payloads
 * 16-byte aligned) and is duplicated as a footer so a block being freed can
 * find its LEFT neighbour in O(1) -- its own header already gives it the
 * right neighbour, via payload size. That symmetry is what makes coalescing
 * in both directions cheap.
 *
 * uint32_t, not size_t, for the sizes stored in a Tag: this arena is
 * KF_ARENA_LUA_BYTES (1MB in kf/budget.h today), which fits uint32_t with
 * enormous headroom, and a fixed-width field means the layout this
 * allocator's own bookkeeping uses is identical between the 64-bit desktop
 * build and the 32-bit ESP32 build (ADR 0028) that both compile this exact
 * file today -- see kf_lua_port.h for where Lua is wired in on each target.
 */

#include "kf_lua_alloc.h"

#include "kf/arena.h"
#include "kf/budget.h"
#include "kf/hal/log.h"

#include <cstring>

namespace {

constexpr const char *TAG = "lua-alloc";
constexpr size_t kAlign = 16;
constexpr size_t kTagBytes = 16;

struct Tag {
    uint32_t payload_bytes;
    uint32_t free;
    uint32_t reserved[2]; /* padding only, keeps sizeof(Tag) == kTagBytes */
};
static_assert(sizeof(Tag) == kTagBytes,
              "Tag must be exactly kTagBytes, headers and footers below "
              "assume it");

/* g_base/g_end describe the one block this allocator will ever own, once
 * acquired -- set on the FIRST kf_lua_alloc_init() and never cleared,
 * including across a kf_lua_alloc_shutdown(). g_active is the thing that
 * actually toggles between init and shutdown; see kf_lua_alloc_init()'s
 * header comment for why the two are deliberately not the same variable. */
uint8_t *g_base = nullptr;
uint8_t *g_end = nullptr; /* one past the last byte of the arena block */
bool g_active = false;

size_t round_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

Tag *footer_of(Tag *header) {
    return reinterpret_cast<Tag *>(reinterpret_cast<uint8_t *>(header) +
                                    kTagBytes + header->payload_bytes);
}

uint8_t *payload_of(Tag *header) {
    return reinterpret_cast<uint8_t *>(header) + kTagBytes;
}

Tag *header_of(void *payload) {
    return reinterpret_cast<Tag *>(static_cast<uint8_t *>(payload) -
                                    kTagBytes);
}

/* One past this block's footer: where the next block's header would start,
 * if this is not the last block in the arena. */
Tag *block_end(Tag *header) {
    return reinterpret_cast<Tag *>(
        reinterpret_cast<uint8_t *>(footer_of(header)) + kTagBytes);
}

Tag *prev_footer_of(Tag *header) {
    return reinterpret_cast<Tag *>(reinterpret_cast<uint8_t *>(header) -
                                    kTagBytes);
}

bool in_range(const void *p) {
    return p >= static_cast<const void *>(g_base) &&
           p < static_cast<const void *>(g_end);
}

void write_tags(Tag *header, uint32_t payload_bytes, bool free) {
    header->payload_bytes = payload_bytes;
    header->free = free ? 1u : 0u;
    Tag *footer = footer_of(header);
    footer->payload_bytes = payload_bytes;
    footer->free = free ? 1u : 0u;
}

/* Minimum leftover worth turning into its own free block: header + footer
 * + at least one alignment quantum of usable payload. Below this, splitting
 * would create a sliver too small for any real request ever to use, so
 * whoever is handing out `header` just keeps the few extra bytes attached
 * instead -- a small, bounded amount of internal fragmentation, cheaper
 * than an unusable block cluttering every future search. */
constexpr size_t kMinSplitPayload = 2 * kTagBytes + kAlign;

/* Merges `header` with its immediate right neighbour, if that neighbour
 * exists and is free. Used both by free() (to shrink live blocks back into
 * the free list) and by realloc's grow path (to extend in place). */
void coalesce_forward(Tag *header) {
    Tag *next = block_end(header);
    if (reinterpret_cast<uint8_t *>(next) >= g_end || !next->free) {
        return;
    }
    const uint32_t merged =
        header->payload_bytes + 2u * static_cast<uint32_t>(kTagBytes) +
        next->payload_bytes;
    write_tags(header, merged, /*free=*/true);
}

void *raw_alloc(uint32_t want) {
    Tag *cur = reinterpret_cast<Tag *>(g_base);
    while (reinterpret_cast<uint8_t *>(cur) < g_end) {
        if (cur->free && cur->payload_bytes >= want) {
            const uint32_t remaining = cur->payload_bytes - want;
            if (remaining >= kMinSplitPayload) {
                write_tags(cur, want, /*free=*/false);
                Tag *rest = reinterpret_cast<Tag *>(
                    reinterpret_cast<uint8_t *>(cur) + kTagBytes + want +
                    kTagBytes);
                write_tags(rest,
                           remaining - 2u * static_cast<uint32_t>(kTagBytes),
                           /*free=*/true);
            } else {
                write_tags(cur, cur->payload_bytes, /*free=*/false);
            }
            return payload_of(cur);
        }
        cur = block_end(cur);
    }
    return nullptr; /* arena has nothing large enough right now */
}

void raw_free(void *p) {
    if (p == nullptr) {
        return;
    }
    KF_ASSERT(in_range(p),
              "kf_lua_alloc: free of a pointer outside the Lua arena -- "
              "memory corruption upstream, not a Lua bug");
    Tag *header = header_of(p);
    write_tags(header, header->payload_bytes, /*free=*/true);

    coalesce_forward(header);

    /* Coalesce backward via the footer immediately before this header, then
     * re-run coalesce_forward FROM the previous block: that call will see
     * `header` (already possibly forward-merged above) as ITS right
     * neighbour and merge again, so three adjacent free blocks collapse
     * into one in a single free() call rather than needing a second free()
     * next door to notice. */
    if (reinterpret_cast<uint8_t *>(header) > g_base) {
        Tag *prev_footer = prev_footer_of(header);
        if (prev_footer->free) {
            Tag *prev_header = reinterpret_cast<Tag *>(
                reinterpret_cast<uint8_t *>(prev_footer) -
                prev_footer->payload_bytes - kTagBytes);
            coalesce_forward(prev_header);
        }
    }
}

/* Grows `header` in place by absorbing its free right neighbour, if that
 * neighbour exists and the combined size covers `want`. Returns true and
 * leaves `header`'s address unchanged on success -- no copy, no free/alloc
 * churn, which matters because this is the common case for a table or
 * string buffer growing a little at a time. */
bool try_grow_in_place(Tag *header, uint32_t want) {
    Tag *next = block_end(header);
    if (reinterpret_cast<uint8_t *>(next) >= g_end || !next->free) {
        return false;
    }
    const uint32_t merged_payload =
        header->payload_bytes + 2u * static_cast<uint32_t>(kTagBytes) +
        next->payload_bytes;
    if (merged_payload < want) {
        return false;
    }
    const uint32_t remaining = merged_payload - want;
    if (remaining >= kMinSplitPayload) {
        write_tags(header, want, /*free=*/false);
        Tag *rest = reinterpret_cast<Tag *>(
            reinterpret_cast<uint8_t *>(header) + kTagBytes + want +
            kTagBytes);
        write_tags(rest, remaining - 2u * static_cast<uint32_t>(kTagBytes),
                   /*free=*/true);
    } else {
        write_tags(header, merged_payload, /*free=*/false);
    }
    return true;
}

} // namespace

void kf_lua_alloc_init() {
    KF_ASSERT(!g_active, "kf_lua_alloc_init called twice without an "
                         "intervening kf_lua_alloc_shutdown()");
    KF_ASSERT(KF_ARENA_LUA_BYTES > 2u * kTagBytes,
              "KF_ARENA_LUA_BYTES (kf/budget.h) is too small to hold even "
              "one Lua allocator block header+footer");

    if (g_base == nullptr) {
        /* First call ever: acquire the one block this allocator will ever
         * own. See this function's header comment in kf_lua_alloc.h for
         * why every later re-init reuses this same block instead of
         * calling kf_arena_alloc() again. */
        void *block = kf_arena_alloc(KF_ARENA_LUA, KF_ARENA_LUA_BYTES, kAlign);
        g_base = static_cast<uint8_t *>(block);
        g_end = g_base + KF_ARENA_LUA_BYTES;
    }

    /* Reset the free list to one whole free block spanning the arena,
     * whether this block is fresh or being reused -- a second (or third...)
     * Lua VM lifetime starts with a clean heap, not whatever the previous
     * VM's GC happened to leave behind. */
    const uint32_t payload =
        static_cast<uint32_t>(KF_ARENA_LUA_BYTES - 2u * kTagBytes);
    write_tags(reinterpret_cast<Tag *>(g_base), payload, /*free=*/true);

    g_active = true;

    KF_LOGI(TAG, "ready: %u bytes usable for Lua's heap (KF_ARENA_LUA_BYTES "
                 "%u, %zu bytes of that is this allocator's own bookkeeping)",
            static_cast<unsigned>(payload), static_cast<unsigned>(KF_ARENA_LUA_BYTES),
            2u * kTagBytes);
}

void kf_lua_alloc_shutdown() { g_active = false; }

void *kf_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize; /* this allocator tracks a block's real size itself (in its
                  * Tag), rather than trusting the caller's osize, so a
                  * mismatch between what Lua thinks it handed back and what
                  * this allocator actually recorded can never desync the
                  * two -- see kf_lua_alloc.h's contract notes. */

    if (nsize == 0u) {
        raw_free(ptr);
        return nullptr;
    }

    if (nsize > KF_ARENA_LUA_BYTES) {
        /* Cannot possibly fit, whole-arena-in-one-block or not. Fail fast
         * rather than walking the free list to find that out, and rather
         * than risk `nsize` not fitting the uint32_t this allocator's
         * bookkeeping uses on a 64-bit host. */
        KF_LOGW(TAG,
                "refusing a %zu-byte request outright: larger than the "
                "entire %u-byte KF_ARENA_LUA_BYTES arena",
                nsize, static_cast<unsigned>(KF_ARENA_LUA_BYTES));
        return nullptr;
    }

    const uint32_t want = static_cast<uint32_t>(round_up(nsize, kAlign));

    if (ptr == nullptr) {
        void *p = raw_alloc(want);
        if (p == nullptr) {
            KF_LOGW(TAG,
                    "out of Lua heap: wanted %zu bytes, KF_ARENA_LUA_BYTES "
                    "is %u (kf/budget.h). Returning NULL to Lua, which "
                    "raises a catchable script-level error -- see ADR 0014 "
                    "for why this arena does not KF_PANIC like every other "
                    "one.",
                    nsize, static_cast<unsigned>(KF_ARENA_LUA_BYTES));
        }
        return p;
    }

    KF_ASSERT(in_range(ptr),
              "kf_lua_alloc: realloc of a pointer outside the Lua arena -- "
              "memory corruption upstream, not a Lua bug");
    Tag *header = header_of(ptr);

    if (want <= header->payload_bytes) {
        /* Shrinking, or no real change. Per Lua's manual, frealloc MUST NOT
         * fail here -- and by construction it cannot: this block already
         * owns at least `want` bytes, so there is nothing left to allocate.
         * Hand back the tail to the free list only if it is big enough to
         * be useful; see kMinSplitPayload. */
        const uint32_t remaining = header->payload_bytes - want;
        if (remaining >= kMinSplitPayload) {
            write_tags(header, want, /*free=*/false);
            Tag *rest = reinterpret_cast<Tag *>(
                reinterpret_cast<uint8_t *>(header) + kTagBytes + want +
                kTagBytes);
            write_tags(rest,
                       remaining - 2u * static_cast<uint32_t>(kTagBytes),
                       /*free=*/true);
            coalesce_forward(rest);
        }
        return ptr;
    }

    /* Growing: the only case allowed to fail. Try extending into a free
     * right neighbour first -- the common shape for a table or string
     * buffer growing incrementally -- before paying for an allocate + copy
     * + free. */
    if (try_grow_in_place(header, want)) {
        return ptr;
    }

    void *fresh = raw_alloc(want);
    if (fresh == nullptr) {
        KF_LOGW(TAG,
                "out of Lua heap on grow: wanted %zu bytes, had %u. "
                "Returning NULL, which Lua surfaces as a catchable "
                "script-level error.",
                nsize, static_cast<unsigned>(header->payload_bytes));
        return nullptr;
    }
    const size_t copy_bytes =
        header->payload_bytes < nsize ? header->payload_bytes : nsize;
    std::memcpy(fresh, ptr, copy_bytes);
    raw_free(ptr);
    return fresh;
}

kf_lua_alloc_stats kf_lua_alloc_get_stats() {
    kf_lua_alloc_stats s{};
    Tag *cur = reinterpret_cast<Tag *>(g_base);
    while (reinterpret_cast<uint8_t *>(cur) < g_end) {
        if (!cur->free) {
            s.live_bytes += cur->payload_bytes;
            s.live_blocks += 1u;
        }
        cur = block_end(cur);
    }
    return s;
}
