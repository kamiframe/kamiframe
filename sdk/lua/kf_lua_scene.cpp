/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_lua_scene.h for the "why" of every decision named here. This file
 * is the "how" -- docs/architecture/adr-0041-lua-drawing-binding.md is the
 * long-form reasoning for the pieces that are not obvious from the code.
 */

#include "kf_lua_scene.h"

#include "kf/app.h"
#include "kf/assets.h"
#include "kf/budget.h"
#include "kf/hal/log.h"
#include "kf/scene.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdint>
#include <cstring>
#include <new>

namespace {

constexpr const char *TAG = "lua-scene";
constexpr const char *kObjMeta = "kf.SceneObject";

/* An object here mirrors, kind-agnostic, everything kf/scene.h's own
 * RenderState does -- and for the identical reason (see that struct's
 * comment in hakoniwaos/src/scene.cpp): a flat shape means no method has to
 * reach into a union by kind, and 64 objects times a few always-unused
 * fields costs nothing worth a second struct type over. This is the
 * userdata's ENTIRE payload -- one per object, sized once, no further
 * allocation per property the way "a table of closures" (rejected in the
 * plan; see ADR 0041) would need.
 *
 * WHY THIS DUPLICATES STATE kf/scene.h ALSO HOLDS: Task 2's Core API is
 * write-only past kf_scene_bounds() (a rectangle, not the individual
 * fields) -- there is no kf_scene_get_pos()/_get_layer()/_get_text()/etc.
 * The no-arg-read half of "no-arg reads, args write" therefore has nowhere
 * to read FROM except a copy this binding keeps itself. Keeping that copy
 * in the userdata that already exists for the id, rather than a second Lua
 * table, costs zero extra allocations. */
enum class LuaObjKind : uint8_t { kSprite, kText, kBox };

struct LuaSceneObject {
    kf_scene_id id = 0;
    LuaObjKind kind = LuaObjKind::kSprite;
    /* True from :remove() onward -- see check_live_obj() below for the one
     * place this is enforced, so no method has to remember to check it
     * itself. */
    bool removed = false;

    bool visible = true;
    int16_t x = 0;
    int16_t y = 0;
    int8_t layer = 0;

    /* Sprite only. */
    char sprite_name[KF_SCENE_SPRITE_NAME_MAX + 1] = {};
    bool mirrored = false;

    /* Text only. Already uppercased -- see prepare_text() below. */
    char text[KF_SCENE_TEXT_MAX + 1] = {};

    /* Text: both fg and bg. Box: fg only (bg unused, matching
     * kf_scene_set_colors()'s own contract). */
    kf_color fg = KF_WHITE;
    kf_color bg = KF_BLACK;

    /* Box only. */
    int16_t w = 0;
    int16_t h = 0;
};

/* ---------------------------------------------------------------------
 * Clamping helpers. Every one of these exists because a script's argument
 * is untrusted input (Global Constraints: "bad input from Lua must never
 * crash the firmware") and every field it feeds is a narrower C type than
 * lua_Integer -- LUA_32BITS on the ESP32 build still hands this code a
 * signed value that can be far outside int16_t/int8_t/kf_color's range.
 * Clamping, not wrapping: a script that passes 99999 for a coordinate gets
 * a sprite pinned at the edge of the panel, not one that silently wrapped
 * to a small negative number and vanished off-screen in a way nobody could
 * explain from the script alone.
 * --------------------------------------------------------------------- */

uint8_t clamp_u8(lua_Integer v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

kf_color clamp_color(lua_Integer v) {
    if (v < 0) return 0;
    if (v > 0xFFFF) return 0xFFFF;
    return static_cast<kf_color>(v);
}

int16_t clamp_i16(lua_Integer v) {
    if (v < INT16_MIN) return INT16_MIN;
    if (v > INT16_MAX) return INT16_MAX;
    return static_cast<int16_t>(v);
}

int8_t clamp_i8(lua_Integer v) {
    if (v < INT8_MIN) return INT8_MIN;
    if (v > INT8_MAX) return INT8_MAX;
    return static_cast<int8_t>(v);
}

/* ---------------------------------------------------------------------
 * Text: uppercase, and warn once per distinct unsupported character.
 *
 * kf/font.h's glyph set is space, 0-9, A-Z, and ". , : - / % + ( )" --
 * uppercase only (see that header's own comment on why lowercase was left
 * out). A script that never learns this would see its own text render as a
 * row of blank cells, which is exactly the silent failure the audience
 * constraint (CLAUDE.md) forbids. So this binding uppercases ASCII letters
 * on every text write, and for anything else the font genuinely cannot
 * draw, logs once (not once per frame) naming the character.
 * --------------------------------------------------------------------- */

constexpr const char *kSupportedPunctuation = ".,:-/%+()";

bool char_supported(char c) {
    if (c == ' ') return true;
    if (c >= '0' && c <= '9') return true;
    if (c >= 'A' && c <= 'Z') return true;
    for (const char *p = kSupportedPunctuation; *p != '\0'; ++p) {
        if (*p == c) return true;
    }
    return false;
}

/* Indexed by unsigned char value, process lifetime -- "log once" means once
 * ever, not once per script or per object, so a HUD full of the same emoji
 * does not spam the log 64 times over. */
bool g_warned_char[256] = {};

void uppercase_and_warn(char *s) {
    for (char *p = s; *p != '\0'; ++p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = static_cast<char>(*p - 'a' + 'A');
            continue;
        }
        if (!char_supported(*p)) {
            const unsigned char uc = static_cast<unsigned char>(*p);
            if (!g_warned_char[uc]) {
                g_warned_char[uc] = true;
                KF_LOGW(TAG,
                        "text contains a character the font has no glyph "
                        "for (0x%02x) -- it will draw as a blank cell",
                        uc);
            }
        }
    }
}

/* `work` is deliberately larger than KF_SCENE_TEXT_MAX: kf_scene_add_text()
 * /kf_scene_set_text() do their OWN truncation and their own "too long"
 * log (hakoniwaos/src/scene.cpp's copy_truncated()) when handed the
 * uppercased string below, and this function must not rob them of the
 * chance to -- pre-truncating to KF_SCENE_TEXT_MAX here first would make
 * Core see a string that is never longer than its own limit, so its length
 * warning would never fire. 256 is "long enough that no script's real
 * label ever needs Core's warning suppressed by hitting this second, much
 * higher ceiling first", not a considered protocol limit. `shadow` (the
 * userdata's own copy, for :set()'s no-arg read) is capped to
 * KF_SCENE_TEXT_MAX -- what Core will actually keep. */
void prepare_text(const char *src, char *work, size_t work_cap, char *shadow,
                   size_t shadow_cap) {
    std::strncpy(work, src, work_cap - 1u);
    work[work_cap - 1u] = '\0';
    uppercase_and_warn(work);
    std::strncpy(shadow, work, shadow_cap - 1u);
    shadow[shadow_cap - 1u] = '\0';
}

constexpr size_t kTextWorkBufSize = 256;

/* ---------------------------------------------------------------------
 * The scene declared -- see kf_lua_scene.h's own comment on
 * kf_lua_scene_declared_anything() for what this guards against.
 * --------------------------------------------------------------------- */
bool g_declared_anything = false;

void mark_declared() { g_declared_anything = true; }

/* ---------------------------------------------------------------------
 * kf.on_button -- one Lua function reference per button, in the registry.
 * --------------------------------------------------------------------- */
struct ButtonEntry {
    const char *name;
    kf_button bit;
};
constexpr ButtonEntry kButtonTable[] = {
    {"a", KF_BTN_A},         {"b", KF_BTN_B},     {"menu", KF_BTN_MENU},
    {"up", KF_BTN_UP},       {"down", KF_BTN_DOWN},
    {"left", KF_BTN_LEFT},   {"right", KF_BTN_RIGHT},
};
constexpr int kButtonCount =
    static_cast<int>(sizeof(kButtonTable) / sizeof(kButtonTable[0]));
static_assert(kButtonCount == KF_BUTTON_COUNT,
              "kButtonTable must name every kf_button exactly once");

int g_button_ref[kButtonCount];

int find_button_index(const char *name) {
    for (int i = 0; i < kButtonCount; ++i) {
        if (std::strcmp(kButtonTable[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Object userdata: creation and the two "am I allowed to touch this"
 * checks every method routes through.
 * --------------------------------------------------------------------- */

LuaSceneObject *push_new_object(lua_State *L, LuaObjKind kind,
                                 kf_scene_id id) {
    void *raw = lua_newuserdatauv(L, sizeof(LuaSceneObject), 0);
    /* Placement new, not a bare assignment over raw bytes: lua_newuserdatauv
     * hands back memory Lua's allocator owns but has not constructed
     * anything in -- this is what actually begins this LuaSceneObject's
     * lifetime, not just zero-fills it. No destructor is ever run on it (no
     * __gc metamethod is registered -- see this file's own header comment
     * on why: nothing here holds a resource beyond inline fixed-size
     * arrays, so there is nothing to release), and Lua's GC frees the raw
     * bytes directly when the userdata becomes unreachable. */
    LuaSceneObject *obj = new (raw) LuaSceneObject();
    obj->kind = kind;
    obj->id = id;
    luaL_setmetatable(L, kObjMeta);
    return obj;
}

/* Any object, live or removed -- used only by :remove() itself, which has
 * to be safe to call twice (kf_scene_remove() already is, see kf/scene.h).
 */
LuaSceneObject *check_obj(lua_State *L, int idx) {
    return static_cast<LuaSceneObject *>(luaL_checkudata(L, idx, kObjMeta));
}

/* Every OTHER method routes through this. A removed object raising a
 * named Lua error here -- rather than quietly no-opping the way
 * kf_scene_*() setters do on a not-found id (kf/scene.h's own comment on
 * why THAT is the right behaviour for Core) -- is deliberate at this
 * layer: Core's silent no-op exists so a stray call landing on a slot
 * something else now owns cannot corrupt that new object, but a Lua
 * script calling a method on a handle IT ITSELF removed is a script bug,
 * and this audience gets told about script bugs by name, not left to
 * wonder why a sprite stopped moving (CLAUDE.md's own "a mistyped sprite
 * name should say so" rule, generalised to every mistake this binding can
 * see coming). */
LuaSceneObject *check_live_obj(lua_State *L, int idx) {
    LuaSceneObject *obj = check_obj(L, idx);
    if (obj->removed) {
        luaL_error(L, "this object was already removed with :remove()");
    }
    return obj;
}

/* ---------------------------------------------------------------------
 * Object methods. No-arg reads, args write -- see this project's own
 * jQuery framing (docs/superpowers/plans/2026-08-12-lua-game-layer.md,
 * "The jQuery accessor convention") for why this is one rule rather than
 * get_ / set_ prefixed pairs.
 * --------------------------------------------------------------------- */

int obj_move(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    const int16_t x = clamp_i16(luaL_checkinteger(L, 2));
    const int16_t y = clamp_i16(luaL_checkinteger(L, 3));
    obj->x = x;
    obj->y = y;
    kf_scene_set_pos(obj->id, x, y);
    return 0;
}

int obj_x(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (lua_gettop(L) < 2) {
        lua_pushinteger(L, obj->x);
        return 1;
    }
    obj->x = clamp_i16(luaL_checkinteger(L, 2));
    kf_scene_set_pos(obj->id, obj->x, obj->y);
    return 0;
}

int obj_y(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (lua_gettop(L) < 2) {
        lua_pushinteger(L, obj->y);
        return 1;
    }
    obj->y = clamp_i16(luaL_checkinteger(L, 2));
    kf_scene_set_pos(obj->id, obj->x, obj->y);
    return 0;
}

int obj_show(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    obj->visible = true;
    kf_scene_set_visible(obj->id, true);
    return 0;
}

int obj_hide(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    obj->visible = false;
    kf_scene_set_visible(obj->id, false);
    return 0;
}

int obj_visible(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (lua_gettop(L) < 2) {
        lua_pushboolean(L, obj->visible ? 1 : 0);
        return 1;
    }
    obj->visible = lua_toboolean(L, 2) != 0;
    kf_scene_set_visible(obj->id, obj->visible);
    return 0;
}

int obj_layer(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (lua_gettop(L) < 2) {
        lua_pushinteger(L, obj->layer);
        return 1;
    }
    obj->layer = clamp_i8(luaL_checkinteger(L, 2));
    kf_scene_set_layer(obj->id, obj->layer);
    return 0;
}

int obj_remove(lua_State *L) {
    LuaSceneObject *obj = check_obj(L, 1);
    if (!obj->removed) {
        kf_scene_remove(obj->id);
        obj->removed = true;
    }
    return 0;
}

int obj_sprite(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (obj->kind != LuaObjKind::kSprite) {
        return luaL_error(L, "':sprite' is only valid on a sprite object "
                              "(created with kf.sprite())");
    }
    if (lua_gettop(L) < 2) {
        lua_pushstring(L, obj->sprite_name);
        return 1;
    }
    const char *name = luaL_checkstring(L, 2);
    std::strncpy(obj->sprite_name, name, sizeof(obj->sprite_name) - 1u);
    obj->sprite_name[sizeof(obj->sprite_name) - 1u] = '\0';
    kf_scene_set_sprite(obj->id, name);
    return 0;
}

int obj_flip(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (obj->kind != LuaObjKind::kSprite) {
        return luaL_error(L, "':flip' is only valid on a sprite object "
                              "(created with kf.sprite())");
    }
    if (lua_gettop(L) < 2) {
        lua_pushboolean(L, obj->mirrored ? 1 : 0);
        return 1;
    }
    obj->mirrored = lua_toboolean(L, 2) != 0;
    kf_scene_set_mirrored(obj->id, obj->mirrored);
    return 0;
}

int obj_set(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (obj->kind != LuaObjKind::kText) {
        return luaL_error(L, "':set' is only valid on a text object "
                              "(created with kf.text())");
    }
    if (lua_gettop(L) < 2) {
        lua_pushstring(L, obj->text);
        return 1;
    }
    const char *str = luaL_checkstring(L, 2);
    char work[kTextWorkBufSize];
    prepare_text(str, work, sizeof(work), obj->text, sizeof(obj->text));
    kf_scene_set_text(obj->id, work);
    return 0;
}

int obj_color(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (obj->kind == LuaObjKind::kSprite) {
        return luaL_error(L, "':color' is only valid on a text or box "
                              "object -- a sprite's colours come from its "
                              "own pack data");
    }
    const int nargs = lua_gettop(L);
    if (obj->kind == LuaObjKind::kText) {
        if (nargs < 2) {
            lua_pushinteger(L, obj->fg);
            lua_pushinteger(L, obj->bg);
            return 2;
        }
        obj->fg = clamp_color(luaL_checkinteger(L, 2));
        if (nargs >= 3) {
            obj->bg = clamp_color(luaL_checkinteger(L, 3));
        }
        kf_scene_set_colors(obj->id, obj->fg, obj->bg);
        return 0;
    }
    /* Box: fg only -- kf_scene_set_colors()'s own contract ignores bg for
     * a box object (kf/scene.h), so this passes fg for both rather than
     * inventing a value bg's argument slot has no use for. */
    if (nargs < 2) {
        lua_pushinteger(L, obj->fg);
        return 1;
    }
    obj->fg = clamp_color(luaL_checkinteger(L, 2));
    kf_scene_set_colors(obj->id, obj->fg, obj->fg);
    return 0;
}

int obj_size(lua_State *L) {
    LuaSceneObject *obj = check_live_obj(L, 1);
    if (obj->kind != LuaObjKind::kBox) {
        return luaL_error(L, "':size' is only valid on a box object "
                              "(created with kf.box())");
    }
    if (lua_gettop(L) < 2) {
        lua_pushinteger(L, obj->w);
        lua_pushinteger(L, obj->h);
        return 2;
    }
    obj->w = clamp_i16(luaL_checkinteger(L, 2));
    obj->h = clamp_i16(luaL_checkinteger(L, 3));
    kf_scene_set_size(obj->id, obj->w, obj->h);
    return 0;
}

const luaL_Reg kObjMethods[] = {
    {"move", obj_move},       {"x", obj_x},
    {"y", obj_y},             {"show", obj_show},
    {"hide", obj_hide},       {"visible", obj_visible},
    {"layer", obj_layer},     {"remove", obj_remove},
    {"sprite", obj_sprite},   {"flip", obj_flip},
    {"set", obj_set},         {"color", obj_color},
    {"size", obj_size},       {nullptr, nullptr},
};

void create_object_metatable(lua_State *L) {
    luaL_newmetatable(L, kObjMeta);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, kObjMethods, 0);
    lua_pop(L, 1);
}

/* ---------------------------------------------------------------------
 * kf.* free functions.
 * --------------------------------------------------------------------- */

int lua_kf_sprite(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const kf_scene_id id = kf_scene_add_sprite(name);
    if (id == 0) {
        return luaL_error(L,
                           "the scene already holds %d objects (the "
                           "maximum) -- cannot create sprite '%s'",
                           KF_SCENE_MAX_OBJECTS, name);
    }
    mark_declared();
    LuaSceneObject *obj = push_new_object(L, LuaObjKind::kSprite, id);
    std::strncpy(obj->sprite_name, name, sizeof(obj->sprite_name) - 1u);
    obj->sprite_name[sizeof(obj->sprite_name) - 1u] = '\0';
    return 1;
}

int lua_kf_text(lua_State *L) {
    const char *str = luaL_checkstring(L, 1);
    char work[kTextWorkBufSize];
    char shadow[KF_SCENE_TEXT_MAX + 1];
    prepare_text(str, work, sizeof(work), shadow, sizeof(shadow));
    const kf_scene_id id = kf_scene_add_text(work);
    if (id == 0) {
        return luaL_error(L,
                           "the scene already holds %d objects (the "
                           "maximum) -- cannot create a text object",
                           KF_SCENE_MAX_OBJECTS);
    }
    mark_declared();
    LuaSceneObject *obj = push_new_object(L, LuaObjKind::kText, id);
    std::strcpy(obj->text, shadow);
    return 1;
}

int lua_kf_box(lua_State *L) {
    const int16_t w = clamp_i16(luaL_checkinteger(L, 1));
    const int16_t h = clamp_i16(luaL_checkinteger(L, 2));
    const kf_color c = clamp_color(luaL_checkinteger(L, 3));
    const kf_scene_id id = kf_scene_add_box(w, h, c);
    if (id == 0) {
        return luaL_error(L,
                           "the scene already holds %d objects (the "
                           "maximum) -- cannot create a box object",
                           KF_SCENE_MAX_OBJECTS);
    }
    mark_declared();
    LuaSceneObject *obj = push_new_object(L, LuaObjKind::kBox, id);
    obj->w = w < 0 ? static_cast<int16_t>(0) : w;
    obj->h = h < 0 ? static_cast<int16_t>(0) : h;
    obj->fg = c;
    return 1;
}

int lua_kf_background(lua_State *L) {
    if (lua_type(L, 1) == LUA_TSTRING) {
        kf_scene_set_background_sprite(lua_tostring(L, 1));
    } else {
        kf_scene_set_background_color(clamp_color(luaL_checkinteger(L, 1)));
    }
    mark_declared();
    return 0;
}

int lua_kf_color(lua_State *L) {
    const uint8_t r = clamp_u8(luaL_checkinteger(L, 1));
    const uint8_t g = clamp_u8(luaL_checkinteger(L, 2));
    const uint8_t b = clamp_u8(luaL_checkinteger(L, 3));
    lua_pushinteger(L, KF_RGB(r, g, b));
    return 1;
}

int lua_kf_on_button(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const int index = find_button_index(name);
    if (index < 0) {
        return luaL_error(L,
                           "kf.on_button: unknown button '%s' (expected "
                           "a, b, menu, up, down, left, right)",
                           name);
    }
    if (g_button_ref[index] != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_button_ref[index]);
    }
    lua_pushvalue(L, 2);
    g_button_ref[index] = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

int lua_kf_width(lua_State *L) {
    lua_pushinteger(L, KF_DISPLAY_WIDTH);
    return 1;
}

int lua_kf_height(lua_State *L) {
    lua_pushinteger(L, KF_DISPLAY_HEIGHT);
    return 1;
}

int lua_kf_sprites(lua_State *L) {
    lua_newtable(L);
    const int n = kf_assets_count();
    int out = 1;
    for (int i = 0; i < n; ++i) {
        const char *name = kf_assets_name_at(i);
        if (name == nullptr) {
            continue;
        }
        lua_pushstring(L, name);
        lua_rawseti(L, -2, out++);
    }
    return 1;
}

const luaL_Reg kKfSceneFuncs[] = {
    {"sprite", lua_kf_sprite},
    {"text", lua_kf_text},
    {"box", lua_kf_box},
    {"background", lua_kf_background},
    {"color", lua_kf_color},
    {"on_button", lua_kf_on_button},
    {"width", lua_kf_width},
    {"height", lua_kf_height},
    {"sprites", lua_kf_sprites},
    {nullptr, nullptr},
};

} // namespace

void kf_lua_scene_register(lua_State *L) {
    for (int i = 0; i < kButtonCount; ++i) {
        g_button_ref[i] = LUA_NOREF;
    }
    g_declared_anything = false;

    create_object_metatable(L);

    lua_getglobal(L, "kf");
    luaL_setfuncs(L, kKfSceneFuncs, 0);
    lua_pushinteger(L, KF_WHITE);
    lua_setfield(L, -2, "WHITE");
    lua_pushinteger(L, KF_BLACK);
    lua_setfield(L, -2, "BLACK");
    lua_pushinteger(L, KF_RGB(255, 0, 0));
    lua_setfield(L, -2, "RED");
    lua_pushinteger(L, KF_RGB(0, 255, 0));
    lua_setfield(L, -2, "GREEN");
    lua_pushinteger(L, KF_RGB(0, 0, 255));
    lua_setfield(L, -2, "BLUE");
    lua_pushinteger(L, KF_RGB(255, 255, 0));
    lua_setfield(L, -2, "YELLOW");
    lua_pop(L, 1); /* the `kf` table itself */
}

void kf_lua_scene_dispatch_buttons(lua_State *L) {
    const uint32_t pressed = kf_app_buttons_pressed();
    for (int i = 0; i < kButtonCount; ++i) {
        if (g_button_ref[i] == LUA_NOREF ||
            (pressed & static_cast<uint32_t>(kButtonTable[i].bit)) == 0u) {
            continue;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_button_ref[i]);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            KF_LOGE(TAG, "kf.on_button('%s') handler raised an error: %s",
                    kButtonTable[i].name, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

bool kf_lua_scene_declared_anything() { return g_declared_anything; }
