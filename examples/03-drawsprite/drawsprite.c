#include "sx/os.h"
#include "sx/string.h"
#include "sx/timer.h"

#include "rizz/2dtools.h"
#include "rizz/imgui-extra.h"
#include "rizz/imgui.h"
#include "rizz/rizz.h"

#include "../common.h"

#define MAX_VERTICES 1000
#define MAX_INDICES 2000
#define NUM_SPRITES 6
#define SPRITE_WIDTH 3.5f

RIZZ_STATE static rizz_api_core* the_core;
RIZZ_STATE static rizz_api_gfx* the_gfx;
RIZZ_STATE static rizz_api_app* the_app;
RIZZ_STATE static rizz_api_imgui* the_imgui;
RIZZ_STATE static rizz_api_asset* the_asset;
RIZZ_STATE static rizz_api_camera* the_camera;
RIZZ_STATE static rizz_api_vfs* the_vfs;
RIZZ_STATE static rizz_api_sprite* the_sprite;
RIZZ_STATE static rizz_api_font* the_font;

typedef struct {
    sx_mat4 vp;
    sx_vec4 motion;
} drawsprite_params;

typedef struct {
    sx_vec2 pos;
    sx_vec2 uv;
    sx_vec4 transform;    // (x,y: pos) (z: rotation) (w: scale)
    sx_color color;
    sx_vec3 bc;
} drawsprite_vertex;

static rizz_vertex_layout k_vertex_layout = {
    .attrs[0] = { .semantic = "POSITION", .offset = offsetof(drawsprite_vertex, pos) },
    .attrs[1] = { .semantic = "TEXCOORD", .offset = offsetof(drawsprite_vertex, uv) },
    .attrs[2] = { .semantic = "TEXCOORD",
                  .semantic_idx = 1,
                  .offset = offsetof(drawsprite_vertex, transform) }
};

static rizz_vertex_layout k_vertex_layout_wire = {
    .attrs[0] = { .semantic = "POSITION", .offset = offsetof(drawsprite_vertex, pos) },
    .attrs[1] = { .semantic = "TEXCOORD", .offset = offsetof(drawsprite_vertex, uv) },
    .attrs[2] = { .semantic = "TEXCOORD",
                  .semantic_idx = 1,
                  .offset = offsetof(drawsprite_vertex, transform) },
    .attrs[3] = { .semantic = "TEXCOORD",
                  .semantic_idx = 2,
                  .offset = offsetof(drawsprite_vertex, bc) }
};

typedef struct {
    rizz_gfx_stage stage;
    sg_pipeline pip;
    sg_pipeline pip_wire;
    rizz_asset atlas;
    rizz_asset shader;
    rizz_asset shader_wire;
    sg_buffer vbuff;
    sg_buffer ibuff;
    rizz_camera_fps cam;
    rizz_sprite sprites[NUM_SPRITES];
    rizz_asset font;
    bool wireframe;
    bool custom;
} drawsprite_state;

RIZZ_STATE static drawsprite_state g_ds;

RIZZ_STATE static rizz_api_refl* the_refl;

#include <stdio.h>
#include "rizz/json.h"

typedef struct write_json_context {
    const char* filename;
    FILE* f;
    const char* newline;
    const char* tab;

    int _depth;
    bool _is_struct_array;
    char _tabs[128];
    int _array_count;
} write_json_context;

typedef struct read_json_context {
    rizz_refl_context* rctx;
    rizz_json* json;
    int cur_token;
    int last_token;
    int struct_array_parent;
} read_json_context;

static const char* test_refl_update_tabs(write_json_context* jctx)
{
    if (jctx->tab[0]) {
        sx_strcpy(jctx->_tabs, sizeof(jctx->_tabs), jctx->tab);
        for (int i = 0; i < jctx->_depth; i++) {
            sx_strcat(jctx->_tabs, sizeof(jctx->_tabs), jctx->tab);
        }
    } else {
        jctx->_tabs[0] = '\0';
    }

    return jctx->_tabs;
}

static bool test_refl_on_begin(const char* type_name, void* user) 
{
    write_json_context* jctx = user;
    sx_assertf(jctx->filename, "must provide a valid json filename");
    sx_assert(jctx->f == NULL);

    if (!jctx->newline) {
        jctx->newline = "";
    }
    if (!jctx->tab) {
        jctx->tab = "";
    }
    
    jctx->f = fopen(jctx->filename, "wt");
    if (!jctx->f) {
        return false;
    }
    fprintf(jctx->f, "{%s", jctx->newline);

    test_refl_update_tabs(jctx);
    return true;
}

static void test_refl_on_end(void* user)
{
    write_json_context* jctx = user;
    fprintf(jctx->f, "}%s", jctx->newline);
    fclose(jctx->f);
    jctx->f = NULL;
}

#define COMMA() (!last_in_parent ? "," : "")

static void test_refl_on_builtin(const char* name, rizz_refl_variant value, void* user, const void* meta, 
                                 bool last_in_parent)
{
    write_json_context* jctx = user;
    const char* tabs = jctx->_tabs;
    switch (value.type) {
    case RIZZ_REFL_VARIANTTYPE_FLOAT:   
        fprintf(jctx->f, "%s\"%s\": %f%s%s", tabs, name, value.f, COMMA(), jctx->newline);   
        break;
    case RIZZ_REFL_VARIANTTYPE_INT32:   
        fprintf(jctx->f, "%s\"%s\": %d%s%s", tabs, name, value.i32, COMMA(), jctx->newline); 
        break;
    case RIZZ_REFL_VARIANTTYPE_BOOL:    
        fprintf(jctx->f, "%s\"%s\": %s%s%s", tabs, name, value.b ? "true" : "false", COMMA(), jctx->newline);   
        break;
    case RIZZ_REFL_VARIANTTYPE_CSTRING: 
        fprintf(jctx->f, "%s\"%s\": \"%s\"%s%s", tabs, name, value.str, COMMA(), jctx->newline); 
        break;
    default:    
        break;
    }
    
}

static void test_refl_on_builtin_array(const char* name, const rizz_refl_variant* vars, int count,
                                       void* user, const void* meta, bool last_in_parent)
{
    write_json_context* jctx = user;
    fprintf(jctx->f, "%s\"%s\": [", jctx->_tabs, name);
    for (int i = 0; i < count; i++) {
        switch (vars[i].type) {
        case RIZZ_REFL_VARIANTTYPE_FLOAT:
            fprintf(jctx->f, "%f%s", vars[i].f, (i < count - 1) ? "," : "");
            break;
        case RIZZ_REFL_VARIANTTYPE_INT32:
            fprintf(jctx->f, "%d%s", vars[i].i32, (i < count - 1) ? "," : "");
            break;
        case RIZZ_REFL_VARIANTTYPE_BOOL:
            fprintf(jctx->f, "%s%s", vars[i].b ? "true" : "false", (i < count - 1) ? "," : "");
            break;
        case RIZZ_REFL_VARIANTTYPE_CSTRING:
            fprintf(jctx->f, "\"%s\"%s", vars[i].str, (i < count - 1) ? "," : "");
            break;
        default:
            break;
        }
    }
    fprintf(jctx->f, "]%s%s", COMMA(), jctx->newline);
}

static void test_refl_on_struct_begin(const char* name, const char* type_name, int size, int count,
                                      void* user, const void* meta)
{
    write_json_context* jctx = user;

    if (count == 1) {
        fprintf(jctx->f, "%s\"%s\": {%s", jctx->_tabs, name, jctx->newline);

    } else {
        fprintf(jctx->f, "%s\"%s\": [{%s", jctx->_tabs, name, jctx->newline);
        jctx->_is_struct_array = true;
    }

    ++jctx->_depth;
    test_refl_update_tabs(jctx);
    
    jctx->_array_count = count;
}

static void test_refl_on_struct_array_element(int index, void* user, const void* meta)
{
    write_json_context* jctx = user;
    if (index != 0) {
        char tabs[128];
        if (jctx->tab[0]) {
            sx_strncpy(tabs, sizeof(tabs), jctx->_tabs, sx_strlen(jctx->_tabs) - sx_strlen(jctx->tab));
        } else {
            tabs[0] = '\0';
        }
        fprintf(jctx->f, "%s},%s%s{%s", tabs, jctx->newline, tabs, jctx->newline);
    } 
}

static void test_refl_on_struct_end(void* user, const void* meta, bool last_in_parent)
{
    write_json_context* jctx = user;

    char tabs[128];
    if (jctx->tab[0]) {
        sx_strncpy(tabs, sizeof(tabs), jctx->_tabs, sx_strlen(jctx->_tabs) - sx_strlen(jctx->tab));
    } else {
        tabs[0] = '\0';
    }

    if (jctx->_is_struct_array) {
        fprintf(jctx->f, "%s}]%s%s", tabs, COMMA(), jctx->newline);
        jctx->_is_struct_array = false;
    } else {
        fprintf(jctx->f, "%s}%s%s", tabs, COMMA(), jctx->newline);
    }
    --jctx->_depth;
    test_refl_update_tabs(jctx);
}

static void test_refl_on_enum(const char* name, int value, const char* value_name, void* user,
                              const void* meta, bool last_in_parent)
{
    write_json_context* jctx = user;

    fprintf(jctx->f, "%s\"%s\": \"%s\"%s%s", jctx->_tabs, name, value_name, COMMA(), jctx->newline);
}

static bool on_serialize_begin(const char* type_name, void* user)
{
    read_json_context* jctx = user;
    jctx->cur_token = 0;
    jctx->struct_array_parent = -1;
    return true;
}

static void on_serialize_end(void* user)
{

}

static void on_serialize_builtin(const char* name, void* data, rizz_refl_variant_type type, int size, 
                                 void* user, const void* meta, bool last_in_parent)
{
    read_json_context* jctx = user;
    cj5_result* r = &jctx->json->result;

    switch (type) {
    case RIZZ_REFL_VARIANTTYPE_INT32:   
        sx_assert(size == sizeof(int));
        *((int*)data) = cj5_seekget_int(r, jctx->cur_token, name, 0);
        break;
    case RIZZ_REFL_VARIANTTYPE_FLOAT:
        sx_assert(size == sizeof(float));
        *((float*)data) = cj5_seekget_float(r, jctx->cur_token, name, 0);
        break;
    case RIZZ_REFL_VARIANTTYPE_BOOL:
        sx_assert(size == sizeof(bool));
        *((bool*)data) = cj5_seekget_bool(r, jctx->cur_token, name, 0);
        break;
    case RIZZ_REFL_VARIANTTYPE_CSTRING: 
    {
        char* str = alloca(size);
        sx_assert_always(str);
        sx_strcpy(data, size, cj5_seekget_string(r, jctx->cur_token, name, str, size+1, ""));
        break;                          
    }
    }
}

static void on_serialize_builtin_array(const char* name, void* data, rizz_refl_variant_type type, 
                                       int count, int stride, void* user, const void* meta, bool last_in_parent)
{
    sx_assert(0);
}

static void on_serialize_struct_begin(const char* name, const char* type_name, int size, int count, 
                                      void* user, const void* meta)
{
    read_json_context* jctx = user;
    cj5_result* r = &jctx->json->result;
    jctx->last_token = jctx->cur_token;
    jctx->cur_token = cj5_seek(r, jctx->cur_token, name);
    if (count > 1) {
        jctx->struct_array_parent = jctx->cur_token;
    }
}

static void on_serialize_struct_array_element(int index, void* user, const void* meta)
{
    read_json_context* jctx = user;
    cj5_result* r = &jctx->json->result;
    
    jctx->cur_token = cj5_get_array_elem(r, jctx->struct_array_parent, index);
}

static void on_serialize_struct_end(void* user, const void* meta, bool last_in_parent)
{
    read_json_context* jctx = user;
    cj5_result* r = &jctx->json->result;
    sx_assert(jctx->cur_token != -1);
    
    jctx->cur_token = jctx->last_token;
    jctx->struct_array_parent = -1;
    jctx->last_token = -1;
}

static void on_serialize_enum(const char* name, int* out_value, void* user, const void* meta, bool last_in_parent)
{
    read_json_context* jctx = user;
    cj5_result* r = &jctx->json->result;
    char str[64];
    *out_value = the_refl->get_enum(jctx->rctx, 
        cj5_seekget_string(r, jctx->cur_token, name, str, sizeof(str), ""), 0);
}


static void test_refl(void)
{
    rizz_refl_context* ctx = the_refl->create_context(the_core->heap_alloc());

    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_FLOAT, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_FLOAT2, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_FLOAT3, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_FLOAT4, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_BYTE4, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_BYTE4N, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_UBYTE4, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_UBYTE4N, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_SHORT2, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_SHORT2N, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_SHORT4, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_SHORT4N, NULL);
    rizz_refl_reg_enum(ctx, sg_vertex_format, SG_VERTEXFORMAT_UINT10_N2, NULL);

    rizz_refl_reg_field(ctx, rizz_shader_refl_input, char[32], name, "shader input name", NULL);
    rizz_refl_reg_field(ctx, rizz_shader_refl_input, char[32], semantic, "shader semantic name", NULL);
    rizz_refl_reg_field(ctx, rizz_shader_refl_input, int, semantic_index, "shader semantic index", NULL);
    rizz_refl_reg_field(ctx, rizz_shader_refl_input, sg_vertex_format, type, "shader input type", NULL);

    rizz_refl_reg_field(ctx, rizz_shader_info, rizz_shader_refl_input[SG_MAX_VERTEX_ATTRIBUTES], inputs, "shader inputs", NULL);
    rizz_refl_reg_field(ctx, rizz_shader_info, int, num_inputs, "shader input count", NULL);
    
    rizz_shader* shader = the_asset->obj(g_ds.shader).ptr;
    sx_memset(&shader->info.inputs[shader->info.num_inputs], 0x0, 
              (SG_MAX_VERTEX_ATTRIBUTES-shader->info.num_inputs)*sizeof(rizz_shader_refl_input));
    {
        write_json_context jctx = {
            .filename = "test.json",
            .newline = "\n",
            .tab = "\t",
        };   
        the_refl->deserialize(ctx, "rizz_shader_info", &shader->info, &jctx, &(rizz_refl_deserialize_callbacks) {
            .on_begin = test_refl_on_begin,
            .on_end = test_refl_on_end,
            .on_builtin = test_refl_on_builtin,
            .on_builtin_array = test_refl_on_builtin_array,
            .on_struct_begin = test_refl_on_struct_begin,
            .on_struct_array_element = test_refl_on_struct_array_element,
            .on_struct_end = test_refl_on_struct_end,
            .on_enum = test_refl_on_enum
        });
    }

    // now serialize back
    {
        rizz_asset a = the_asset->load("json", "test.json", &(rizz_json_load_params){0}, 
                        RIZZ_ASSET_LOAD_FLAG_ABSOLUTE_PATH|RIZZ_ASSET_LOAD_FLAG_WAIT_ON_LOAD, NULL, 0);
        read_json_context jctx = {
            .rctx = ctx, 
            .json = the_asset->obj(a).ptr,
        };

        rizz_shader_info info;
        the_refl->serialize(ctx, "rizz_shader_info", &info, &jctx, &(rizz_refl_serialize_callbacks) {
            .on_begin = on_serialize_begin,
            .on_end = on_serialize_end,
            .on_enum = on_serialize_enum,
            .on_builtin = on_serialize_builtin,
            .on_builtin_array = on_serialize_builtin_array,
            .on_struct_begin = on_serialize_struct_begin,
            .on_struct_end = on_serialize_struct_end,
            .on_struct_array_element = on_serialize_struct_array_element,
        });

        rizz_log_debug("end");
    }
}


static bool init()
{
#if SX_PLATFORM_ANDROID || SX_PLATFORM_IOS
    the_vfs->mount_mobile_assets("/assets");
#else
    // mount `/asset` directory
    char asset_dir[RIZZ_MAX_PATH];
    sx_os_path_join(asset_dir, sizeof(asset_dir), EXAMPLES_ROOT, "assets");    // "/examples/assets"
    the_vfs->mount(asset_dir, "/assets");
#endif

    // register main graphics stage.
    // at least one stage should be registered if you want to draw anything
    g_ds.stage = the_gfx->stage_register("main", (rizz_gfx_stage){ .id = 0 });
    sx_assert(g_ds.stage.id);
    
    // load font
    rizz_font_load_params fparams = { 0 };
    g_ds.font = the_asset->load("font", "/assets/fonts/sponge_bob.ttf", &fparams, 0, NULL, 0);

    // sprite device objects
    g_ds.vbuff =
        the_gfx->make_buffer(&(sg_buffer_desc){ .usage = SG_USAGE_STREAM,
                                                .type = SG_BUFFERTYPE_VERTEXBUFFER,
                                                .size = sizeof(drawsprite_vertex) * MAX_VERTICES });

    g_ds.ibuff = the_gfx->make_buffer(&(sg_buffer_desc){ .usage = SG_USAGE_STREAM,
                                                         .type = SG_BUFFERTYPE_INDEXBUFFER,
                                                         .size = sizeof(uint16_t) * MAX_INDICES });

    char shader_path[RIZZ_MAX_PATH];
    g_ds.shader = the_asset->load("shader",
        ex_shader_path(shader_path, sizeof(shader_path), "/assets/shaders", "drawsprite.sgs"), NULL, 0, NULL, 0);
    g_ds.shader_wire = the_asset->load("shader",
        ex_shader_path(shader_path, sizeof(shader_path), "/assets/shaders", "drawsprite_wire.sgs"), NULL, 0, NULL, 0);

    // pipeline
    sg_pipeline_desc pip_desc = { .layout.buffers[0].stride = sizeof(drawsprite_vertex),
                                  .index_type = SG_INDEXTYPE_UINT16,
                                  .rasterizer = { .cull_mode = SG_CULLMODE_BACK },
                                  .blend = { .enabled = true,
                                             .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                             .dst_factor_rgb =
                                                 SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA } };
    g_ds.pip = the_gfx->make_pipeline(the_gfx->shader_bindto_pipeline(
        the_gfx->shader_get(g_ds.shader), &pip_desc, &k_vertex_layout));

    // pipeline
    sg_pipeline_desc pip_desc_wire = {
        .layout.buffers[0].stride = sizeof(drawsprite_vertex),
        .rasterizer = { .cull_mode = SG_CULLMODE_BACK },
        .blend = { .enabled = true,
                   .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                   .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA },
    };
    g_ds.pip_wire = the_gfx->make_pipeline(the_gfx->shader_bindto_pipeline(
        the_gfx->shader_get(g_ds.shader_wire), &pip_desc_wire, &k_vertex_layout_wire));

    // camera
    // projection: setup for ortho, total-width = 10 units
    // view: Y-UP
    sx_vec2 screen_size = the_app->sizef();
    const float view_width = 5.0f;
    const float view_height = screen_size.y * view_width / screen_size.x;
    the_camera->fps_init(&g_ds.cam, 50.0f,
                         sx_rectf(-view_width, -view_height, view_width, view_height), -5.0f, 5.0f);
    the_camera->fps_lookat(&g_ds.cam, sx_vec3f(0, 0.0f, 1.0), SX_VEC3_ZERO, SX_VEC3_UNITY);

    // sprites and atlases
    rizz_atlas_load_params aparams = { .min_filter = SG_FILTER_LINEAR,
                                       .mag_filter = SG_FILTER_LINEAR };
    g_ds.atlas = the_asset->load("atlas", "/assets/textures/handicraft.json", &aparams,
                                 RIZZ_ASSET_LOAD_FLAG_WAIT_ON_LOAD, NULL, 0);

    for (int i = 0; i < NUM_SPRITES; i++) {
        char name[32];
        sx_snprintf(name, sizeof(name), "test/handicraft_%d.png", i + 1);
        g_ds.sprites[i] = the_sprite->create(&(rizz_sprite_desc){ .name = name,
                                                                  .atlas = g_ds.atlas,
                                                                  .size = sx_vec2f(SPRITE_WIDTH, 0),
                                                                  .color = sx_colorn(0xffffffff) });
    }

    test_refl();    

    return true;
}

static void shutdown()
{
    for (int i = 0; i < NUM_SPRITES; i++) {
        if (g_ds.sprites[i].id) {
            the_sprite->destroy(g_ds.sprites[i]);
        }
    }
    if (g_ds.vbuff.id)
        the_gfx->destroy_buffer(g_ds.vbuff);
    if (g_ds.ibuff.id)
        the_gfx->destroy_buffer(g_ds.ibuff);
    if (g_ds.atlas.id)
        the_asset->unload(g_ds.atlas);
    if (g_ds.shader.id)
        the_asset->unload(g_ds.shader);
    if (g_ds.shader_wire.id)
        the_asset->unload(g_ds.shader_wire);
    if (g_ds.pip_wire.id)
        the_gfx->destroy_pipeline(g_ds.pip_wire);
    if (g_ds.font.id) 
        the_asset->unload(g_ds.font);
}

static void update(float dt) {}

// Custom drawing uses `make_drawdata_batch` API function
// which basically returns vertex-buffer/index-buffer and batch data needed to draw the input
// sprites effieciently.
// As an example, we modify vertices and use custom shader with the draw-data
static void draw_custom(const drawsprite_params* params)
{
    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();
    rizz_sprite_drawdata* dd =
        the_sprite->make_drawdata_batch(g_ds.sprites, NUM_SPRITES, tmp_alloc);

    sg_bindings bindings = { .vertex_buffers[0] = g_ds.vbuff };

    // populate new vertex buffer
    if (!g_ds.wireframe) {
        bindings.index_buffer = g_ds.ibuff;
        drawsprite_vertex* verts = sx_malloc(tmp_alloc, sizeof(drawsprite_vertex) * dd->num_verts);
        sx_assert(verts);

        float start_x = -3.0f;
        float start_y = -1.5f;
        for (int i = 0; i < dd->num_sprites; i++) {
            rizz_sprite_drawsprite* dspr = &dd->sprites[i];

            sx_vec4 transform = sx_vec4f(start_x, start_y, 0, 1.0f);
            int end_vertex = dspr->start_vertex + dspr->num_verts;
            for (int v = dspr->start_vertex; v < end_vertex; v++) {
                verts[v].pos = dd->verts[v].pos;
                verts[v].uv = dd->verts[v].uv;
                verts[v].transform = transform;
                verts[v].color = dd->verts[v].color;
            }
            start_x += sx_rect_width(the_sprite->bounds(g_ds.sprites[i])) * 0.8f;

            if ((i + 1) % 3 == 0) {
                start_y += 3.0f;
                start_x = -3.0f;
            }
        }

        the_gfx->staged.update_buffer(g_ds.vbuff, verts, sizeof(drawsprite_vertex) * dd->num_verts);
        the_gfx->staged.update_buffer(g_ds.ibuff, dd->indices, sizeof(uint16_t) * dd->num_indices);
        the_gfx->staged.apply_pipeline(g_ds.pip);
    } else {
        drawsprite_vertex* verts =
            sx_malloc(tmp_alloc, sizeof(drawsprite_vertex) * dd->num_indices);
        sx_assert(verts);
        const sx_vec3 bcs[] = { { { 1.0f, 0, 0 } }, { { 0, 1.0f, 0 } }, { { 0, 0, 1.0f } } };

        float start_x = -3.0f;
        float start_y = -1.5f;
        int vindex = 0;
        for (int i = 0; i < dd->num_sprites; i++) {
            rizz_sprite_drawsprite* dspr = &dd->sprites[i];

            sx_vec4 transform = sx_vec4f(start_x, start_y, 0, 1.0f);
            int end_index = dspr->start_index + dspr->num_indices;
            for (int ii = dspr->start_index; ii < end_index; ii++) {
                int v = dd->indices[ii];
                verts[vindex].pos = dd->verts[v].pos;
                verts[vindex].uv = dd->verts[v].uv;
                verts[vindex].transform = transform;
                verts[vindex].color = dd->verts[v].color;
                verts[vindex].bc = bcs[vindex % 3];
                vindex++;
            }
            start_x += sx_rect_width(the_sprite->bounds(g_ds.sprites[i])) * 0.8f;

            if ((i + 1) % 3 == 0) {
                start_y += 3.0f;
                start_x = -3.0f;
            }
        }

        the_gfx->staged.update_buffer(g_ds.vbuff, verts,
                                      sizeof(drawsprite_vertex) * dd->num_indices);
        the_gfx->staged.apply_pipeline(g_ds.pip_wire);
    }

    bindings.fs_images[0] = the_gfx->texture_get(dd->batches[0].texture)->img;
    the_gfx->staged.apply_bindings(&bindings);

    the_gfx->staged.apply_uniforms(SG_SHADERSTAGE_VS, 0, params, sizeof(*params));

    the_gfx->staged.draw(0, dd->num_indices, 1);
    the_core->tmp_alloc_pop();
}

static void render()
{
    sg_pass_action pass_action = { .colors[0] = { SG_ACTION_CLEAR, { 0.25f, 0.5f, 0.75f, 1.0f } },
                                   .depth = { SG_ACTION_CLEAR, 1.0f } };

    the_gfx->staged.begin(g_ds.stage);
    the_gfx->staged.begin_default_pass(&pass_action, the_app->width(), the_app->height());

    // draw sprite
    sx_mat4 proj = the_camera->ortho_mat(&g_ds.cam.cam);
    sx_mat4 view = the_camera->view_mat(&g_ds.cam.cam);
    sx_mat4 vp = sx_mat4_mul(&proj, &view);

    drawsprite_params params = {
        .vp = vp, .motion = { .x = (float)sx_tm_sec(the_core->elapsed_tick()), .y = 0.5f }
    };

    sx_mat3 mats[NUM_SPRITES];
    float start_x = -3.0f;
    float start_y = -1.5f;

    for (int i = 0; i < NUM_SPRITES; i++) {
        mats[i] = sx_mat3_translate(start_x, start_y);

        start_x += sx_rect_width(the_sprite->bounds(g_ds.sprites[i])) * 0.8f;
        if ((i + 1) % 3 == 0) {
            start_y += 3.0f;
            start_x = -3.0f;
        }
    }

    if (!g_ds.custom) {
        the_sprite->draw_batch(g_ds.sprites, NUM_SPRITES, &vp, mats, NULL);
        if (g_ds.wireframe)
            the_sprite->draw_wireframe_batch(g_ds.sprites, NUM_SPRITES, &vp, mats);
    } else {
        draw_custom(&params);
    }

    // draw sample font
    {
        const rizz_font* font = the_font->font_get(g_ds.font);
        the_font->push_state(font);
        // note: setup ortho matrix in a way that the Y is reversed (top-left = origin)
        float w = (float)the_app->width();
        float h = (float)the_app->height();
        sx_mat4 vp = sx_mat4_ortho_offcenter(0, h, w, 0, -1.0f, 1.0f, 0, the_gfx->GL_family());

        the_font->set_viewproj_mat(font, &vp);
        the_font->set_size(font, 30.0f);
        rizz_font_vert_metrics metrics = the_font->vert_metrics(font);

        float y = metrics.lineh + 15.0f;
        the_font->draw(font, sx_vec2f(15.0f, y), "DrawSprite Example");

        the_font->push_state(font);
        the_font->set_size(font, 16.0f);
        the_font->draw(font, sx_vec2f(15.0f, y + metrics.lineh), "This text is drawn by font API");
        the_font->pop_state(font);

        the_font->pop_state(font);
    }


    the_gfx->staged.end_pass();
    the_gfx->staged.end();

    // UI
    static bool show_debugger = false;
    show_debugmenu(the_imgui, the_core);

    the_imgui->SetNextWindowContentSize(sx_vec2f(140.0f, 120.0f));
    if (the_imgui->Begin("drawsprite", NULL, 0)) {
        the_imgui->LabelText("Fps", "%.3f", the_core->fps());
        the_imgui->Checkbox("Show Debugger", &show_debugger);
        the_imgui->Checkbox("Wireframe", &g_ds.wireframe);
        the_imgui->Checkbox("Custom Drawing", &g_ds.custom);
    }
    the_imgui->End();

    if (show_debugger) {
        the_sprite->show_debugger(&show_debugger);
    }
}

rizz_plugin_decl_main(drawsprite, plugin, e)
{
    switch (e) {
    case RIZZ_PLUGIN_EVENT_STEP:
        update(the_core->delta_time());
        render();
        break;

    case RIZZ_PLUGIN_EVENT_INIT:
        // runs only once for application. Retreive needed APIs
        the_core = plugin->api->get_api(RIZZ_API_CORE, 0);
        the_gfx = plugin->api->get_api(RIZZ_API_GFX, 0);
        the_app = plugin->api->get_api(RIZZ_API_APP, 0);
        the_vfs = plugin->api->get_api(RIZZ_API_VFS, 0);
        the_asset = plugin->api->get_api(RIZZ_API_ASSET, 0);
        the_camera = plugin->api->get_api(RIZZ_API_CAMERA, 0);

        the_imgui = plugin->api->get_api_byname("imgui", 0);
        the_sprite = plugin->api->get_api_byname("sprite", 0);
        the_font = plugin->api->get_api_byname("font", 0);
        sx_assertf(the_sprite, "sprite plugin is not loaded!");

        the_refl = plugin->api->get_api(RIZZ_API_REFLECT, 0);

        if (!init())
            return -1;
        break;

    case RIZZ_PLUGIN_EVENT_LOAD:
        break;

    case RIZZ_PLUGIN_EVENT_UNLOAD:
        break;

    case RIZZ_PLUGIN_EVENT_SHUTDOWN:
        shutdown();
        break;
    }

    return 0;
}

rizz_plugin_decl_event_handler(drawsprite, e)
{
    switch (e->type) {
    case RIZZ_APP_EVENTTYPE_SUSPENDED:
        break;
    case RIZZ_APP_EVENTTYPE_RESTORED:
        break;
    case RIZZ_APP_EVENTTYPE_MOUSE_DOWN:
        break;
    case RIZZ_APP_EVENTTYPE_MOUSE_UP:
        break;
    case RIZZ_APP_EVENTTYPE_MOUSE_MOVE:
        break;
    default:
        break;
    }
}

rizz_game_decl_config(conf)
{
    conf->app_name = "drawsprite";
    conf->app_version = 1000;
    conf->app_title = "03 - DrawSprite";
    conf->app_flags |= RIZZ_APP_FLAG_HIGHDPI;
    conf->log_level = RIZZ_LOG_LEVEL_DEBUG;
    conf->window_width = 1280;
    conf->window_height = 800;
    conf->swap_interval = 2;
    conf->plugins[0] = "imgui";
    conf->plugins[1] = "2dtools";
}
