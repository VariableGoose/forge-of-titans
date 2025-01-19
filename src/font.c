#include "font.h"
#include "freetype/freetype.h"
#include "renderer.h"
#include "waddle.h"

QuadtreeAtlas quadtree_atlas_init(WDL_Arena* arena) {
    QuadtreeAtlas atlas = {
        .arena = arena,
        .root = {
            .size = wdl_iv2(1024, 1024),
        },
        .bitmap = wdl_arena_push(arena, 1024*1024),
    };
    return atlas;
}

u32 align_value_up(u32 value, u32 align) {
    u64 aligned = value + align - 1;
    u64 mod = aligned % align;
    aligned = aligned - mod;
    return aligned;
}

QuadtreeAtlasNode* quadtree_atlas_insert_helper(WDL_Arena* arena, QuadtreeAtlasNode* node, WDL_Ivec2 size) {
    if (node == NULL || node->occupied || node->size.x < size.x || node->size.y < size.y) {
        return NULL;
    }

    if (!node->split) {
        if (node->size.x == size.x && node->size.y == size.y) {
            node->occupied = true;
            return node;
        }

        node->children = wdl_arena_push_no_zero(arena, 4 * sizeof(QuadtreeAtlasNode));
        node->split = true;

        // Dynamic split
        if (node->size.x / 2 < size.x || node->size.y / 2 < size.y) {
            node->children[0] = (QuadtreeAtlasNode) {
                .size = size,
                .pos = node->pos,
                .occupied = true,
            };

            {
                WDL_Ivec2 new_size = node->size;
                new_size.x -= size.x;
                new_size.y = size.y;
                WDL_Ivec2 pos = node->pos;
                pos.x += size.x;
                node->children[1] = (QuadtreeAtlasNode) {
                    .size = new_size,
                    .pos = pos,
                };
            }

            {
                WDL_Ivec2 new_size = node->size;
                new_size.x = size.x;
                new_size.y -= size.y;
                WDL_Ivec2 pos = node->pos;
                pos.y += size.y;
                node->children[2] = (QuadtreeAtlasNode) {
                    .size = new_size,
                    .pos = pos,
                };
            }

            return &node->children[0];
        }

        WDL_Ivec2 half_size = wdl_iv2_divs(node->size, 2);
        node->children[0] = (QuadtreeAtlasNode) {
            .size = half_size,
                .pos = node->pos,
        };
        node->children[1] = (QuadtreeAtlasNode) {
            .size = half_size,
                .pos = wdl_iv2(node->pos.x + half_size.x, node->pos.y),
        };
        node->children[2] = (QuadtreeAtlasNode) {
            .size = half_size,
                .pos = wdl_iv2(node->pos.x, node->pos.y + half_size.y),
        };
        node->children[3] = (QuadtreeAtlasNode) {
            .size = half_size,
                .pos = wdl_iv2_add(node->pos, half_size),
        };
    }

    for (u8 i = 0; i < 4; i++) {
        QuadtreeAtlasNode* result = quadtree_atlas_insert_helper(arena, &node->children[i], size);
        if (result != NULL) {
            return result;
        }
    }

    return NULL;
}

QuadtreeAtlasNode* quadtree_atlas_insert(QuadtreeAtlas* atlas, WDL_Ivec2 size) {
    size.x = align_value_up(size.x, 4);
    size.y = align_value_up(size.y, 4);
    return quadtree_atlas_insert_helper(atlas->arena, &atlas->root, size);
}

void quadtree_atlas_debug_draw_helper(WDL_Ivec2 atlas_size, QuadtreeAtlasNode* node, Quad quad, Camera cam) {
    if (node == NULL) {
        return;
    }

    WDL_Vec2 size = wdl_v2(
            (f32) node->size.x / (f32) atlas_size.x,
            (f32) node->size.y / (f32) atlas_size.y
        );
    size = wdl_v2_mul(size, quad.size);

    WDL_Vec2 pos = wdl_v2(
            (f32) node->pos.x / (f32) atlas_size.x,
            -(f32) node->pos.y / (f32) atlas_size.y
        );
    pos = wdl_v2_mul(pos, quad.size);
    pos = wdl_v2_add(pos, quad.pos);

    debug_draw_quad_outline((Quad) {
            .pos = pos,
            .size = size,
            .color = GFX_COLOR_WHITE,
            .pivot = wdl_v2(-0.5f, 0.5f),
            }, cam);

    if (node->split) {
        for (u8 i = 0; i < 4; i++) {
            quadtree_atlas_debug_draw_helper(atlas_size, &node->children[i], quad, cam);
        }
    }
}

void quadtree_atlas_debug_draw(QuadtreeAtlas atlas, Quad quad, Camera cam) {
    debug_draw_quad((Quad) {
            .pos = quad.pos,
            .size = quad.size,
            .pivot = wdl_v2(-0.5f, 0.5f),
            .color = quad.color,
            .texture = quad.texture,
        }, cam);
    quadtree_atlas_debug_draw_helper(atlas.root.size, &atlas.root, quad, cam);
}

// -- FreeType2 font provider --------------------------------------------------

typedef struct FT2Internal FT2Internal;
struct FT2Internal {
    FT_Library lib;
    FT_Face face;
};

static void* ft2_init(WDL_Arena*arena, WDL_Str filename) {
    FT2Internal* internal = wdl_arena_push_no_zero(arena, sizeof(FT2Internal));
    FT_Init_FreeType(&internal->lib);

    WDL_Scratch scratch = wdl_scratch_begin(&arena, 1);
    const char* cstr = wdl_str_to_cstr(scratch.arena, filename);
    FT_New_Face(internal->lib, cstr, 0, &internal->face);
    wdl_scratch_end(scratch);

    return internal;
}

static void ft2_terminate(void* internal) {
    FT2Internal* ft2 = internal;
    FT_Done_Face(ft2->face);
    FT_Done_FreeType(ft2->lib);
}

static FPGlyph ft2_get_glyph(void* internal, WDL_Arena* arena, u32 codepoint, u32 size) {
    (void) arena;

    FT2Internal* ft2 = internal;
    FT_Face face = ft2->face;
    FT_Set_Pixel_Sizes(face, 0, size);

    FT_Load_Char(face, codepoint, FT_LOAD_RENDER);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap bm = slot->bitmap;
    FT_Glyph_Metrics metrics = slot->metrics;

    FPGlyph glyph = {
        .bitmap = {
            .size = wdl_iv2(bm.width, bm.rows),
            .buffer = bm.buffer,
        },
        .size = wdl_v2(metrics.width >> 6, metrics.height >> 6),
        .offset = wdl_v2(metrics.horiBearingX >> 6, metrics.horiBearingY >> 6),
        .advance = metrics.horiAdvance >> 6,
    };

    return glyph;
}

static const FontProvider FT2_PROVIDER = {
    .init = ft2_init,
    .terminate = ft2_terminate,
    .get_glyph = ft2_get_glyph,
};

FontProvider font_provider_get_ft2(void) {
    return FT2_PROVIDER;
}
