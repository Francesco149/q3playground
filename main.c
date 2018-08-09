/*
 * quake 3 bsp renderer in c89 and opengl 1.x
 *
 * ![](https://i.imgur.com/Ig4hFPQ.gif)
 *
 * dependencies: libGL, libGLU, SDL2
 *
 * should be compatible at least with x86/x86\_64 windows, linux, freebsd
 * with gcc, clang, msvc
 *
 * I might or might not add more features in the future, for now I have:
 *
 * * rendering meshes and patches
 * * vertex lighting
 * * collision detection with brushes (no patches aka curved surfaces yet)
 * * cpm-like physics
 * * sliding against bushes (no patches though)
 *
 * the current priority is getting patches collisions and implement steps
 * so we can actually walk up stairs
 *
 * # compiling
 * just run ```./build``` . it's aware of ```CC```, ```CFLAGS```,
 * ```LDFLAGS``` in case you need to override anything
 *
 * windows build script is TODO
 *
 * # usage
 * unzip the .pk3 files from your copy of quake 3. some of these will
 * contain .bsp files for the maps. you can run q3playground on them
 * like so:
 *
 * ```
 * q3playground /path/to/map.bsp
 * ```
 *
 * on *nix you might want to disable vsync by running it like
 *
 * ```
 * env vblank_mode=0 q3playground /path/to/map.bsp
 * ```
 *
 * controls are WASD, space, mouse, right click. toggle noclip with F
 *
 * run ```q3playground``` with no arguments for more info
 *
 * # license
 * this is free and unencumbered software released into the public domain.
 * refer to the attached UNLICENSE or http://unlicense.org/
 *
 * # references
 * * unofficial bsp format spec: http://www.mralligator.com/q3/
 * * tessellation implementation:
 *   http://graphics.cs.brown.edu/games/quake/quake3.html
 * * collision detection:
 *   https://web.archive.org/web/20041206085743/http://www.nathanostgard.com:80/tutorials/quake3/collision/
 */

#include <SDL2/SDL.h>

#define degrees(rad) ((rad) * (180.0f / M_PI))
#define radians(deg) ((deg) * (M_PI / 180.0f))
#define eq3(a, b) \
    ((a)[0] == (b)[0] && (a)[1] == (b)[1] && (a)[2] == (b)[2])
#define clr3(a) ((a)[0] = 0, (a)[1] = 0, (a)[2] = 0)
#define cpy3(a, b) ((a)[0] = (b)[0], (a)[1] = (b)[1], (a)[2] = (b)[2])
#define dot3(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
#define mag3(v) (float)SDL_sqrt(dot3(v, v))
#define cross3(a, b, dst) ( \
    (dst)[0] = (a)[1] * (b)[2] - (a)[2] * (b)[1], \
    (dst)[1] = (a)[2] * (b)[0] - (a)[0] * (b)[2], \
    (dst)[2] = (a)[0] * (b)[1] - (a)[1] * (b)[0] \
)
#define add2(a, b) ((a)[0] += (b)[0], (a)[1] += (b)[1])
#define add3(a, b) ((a)[0] += (b)[0], (a)[1] += (b)[1], (a)[2] += (b)[2])
#define mul2_scalar(a, b) ((a)[0] *= b, (a)[1] *= b)
#define mul3_scalar(a, b) ((a)[0] *= b, (a)[1] *= b, (a)[2] *= b)
#define div3_scalar(a, b) ((a)[0] /= b, (a)[1] /= b, (a)[2] /= b)
#define expand2(x) (x)[0], (x)[1]
#define expand3(x) (x)[0], (x)[1], (x)[2]

#define lninfo __FILE__, __LINE__
#define log_puts(x) log_print(lninfo, "%s", x)
#define log_dump(spec, var) log_print(lninfo, #var " = %" spec, var)

void log_print(char const* file, int line, char* fmt, ...)
{
    va_list va;
    char* msg;
    int msg_len;
    char* p;
    char* end;

    msg_len = 0;
    msg_len += SDL_snprintf(0, 0, "[%s:%d] ", file, line);
    va_start(va, fmt);
    msg_len += SDL_vsnprintf(0, 0, fmt, va);
    va_end(va);
    msg_len += 2;

    msg = SDL_malloc(msg_len);
    if (!msg) {
        SDL_Log("log_print alloc failed: %s", SDL_GetError());
        return;
    }

    p = msg;
    end = msg + msg_len;
    p += SDL_snprintf(p, end - p, "[%s:%d] ", file, line);

    va_start(va, fmt);
    p += SDL_vsnprintf(p, end - p, fmt, va);
    va_end(va);

    for (p = msg; p < end; p += SDL_MAX_LOG_MESSAGE - 7) {
        SDL_Log("%s", p);
    }

    SDL_free(msg);
}

struct vec_header
{
    int n;
    int cap;
};

/* cover your eyes, don't look at the macro abominations */

#define vec_hdr(v) (v ? ((struct vec_header*)(v) - 1) : 0)
#define vec_len(v) (v ? vec_hdr(v)->n : 0)
#define vec_cap(v) (v ? vec_hdr(v)->cap : 0)
#define vec_grow(v, n) (*(void**)&(v) = vec_fit(v, n, sizeof((v)[0])))
#define vec_reserve(v, n) ( \
    vec_grow(v, vec_len(v) + n), \
    &(v)[vec_len(v)] \
)
#define vec_append(v, x) (*vec_append_p(v) = (x))
#define vec_append_p(v) ( \
    vec_reserve(v, 1), \
    &(v)[vec_hdr(v)->n++] \
)
#define vec_cat(v, array, array_size) ( \
    memcpy(vec_reserve(v, array_size), array, array_size * sizeof(v)[0]), \
    vec_hdr(v)->n += array_size \
)
#define vec_clear(v) (v ? vec_hdr(v)->n = 0 : 0)
#define vec_free(v) (SDL_free(vec_hdr(v)), v = 0)

void* vec_fit(void* v, int n, int element_size)
{
    struct vec_header* hdr;

    hdr = vec_hdr(v);

    if (!hdr || SDL_max(n, vec_len(v)) >= vec_cap(v))
    {
        int new_cap;
        int alloc_size;

        new_cap = SDL_max(n, vec_cap(v) * 2);
        alloc_size = sizeof(struct vec_header) + new_cap * element_size;

        if (hdr) {
            hdr = SDL_realloc(hdr, alloc_size);
        } else {
            hdr = SDL_malloc(alloc_size);
            hdr->n = 0;
        }

        hdr->cap = new_cap;
    }

    return hdr + 1;
}

void nrm3(float* v)
{
    float squared_len;
    float len;

    squared_len = dot3(v, v);

    if (squared_len < 0.0001f) {
        return;
    }

    len = (float)SDL_sqrt(squared_len);
    div3_scalar(v, len);
}

char* snprintf_alloc(char* fmt, ...)
{
    va_list va;
    char* str;
    int len;

    va_start(va, fmt);
    len = SDL_vsnprintf(0, 0, fmt, va) + 1;
    va_end(va);

    str = SDL_malloc(len);
    if (str) {
        va_start(va, fmt);
        SDL_vsnprintf(str, len, fmt, va);
        va_end(va);
    }

    return str;
}

SDL_RWops* open_data_file(char* file, char* mode)
{
    static char* data_path = 0;
    SDL_RWops* io;
    char* real_path;

    if (!data_path)
    {
        data_path = SDL_GetBasePath();
        if (!data_path) {
            data_path = "./";
        }

        log_dump("s", data_path);
    }

    real_path = snprintf_alloc("%s/%s", data_path, file);
    io = SDL_RWFromFile(real_path, mode);
    SDL_free(real_path);

    return io;
}

char* read_entire_file(char* file)
{
    SDL_RWops* io;
    char* res = 0;
    char buf[1024];
    size_t n;

    io = open_data_file(file, "rb");
    if (!io) {
        log_puts(SDL_GetError());
        SDL_ClearError();
        return 0;
    }

    while (1)
    {
        n = SDL_RWread(io, buf, 1, sizeof(buf));

        if (!n)
        {
            if (*SDL_GetError())
            {
                log_print(lninfo, "SDL_RWread failed: %s", SDL_GetError());
                SDL_ClearError();
                vec_free(res);
                goto cleanup;
            }

            break;
        }

        vec_cat(res, buf, n);
    }

cleanup:
    SDL_RWclose(io);
    return res;
}

/* --------------------------------------------------------------------- */

#include <SDL2/SDL_opengl.h>
#include <GL/glu.h>

int gl_display;
int gl_window_mode;
int gl_width;
int gl_height;

SDL_Window* gl_window;
SDL_GLContext* gl_context;

int gl_major_version;
int gl_sl_major_version;
int gl_max_texture_size;

#define glGetCString (char*)glGetString
#define log_glstring(i) log_print(lninfo, #i " = %s", glGetCString(i))

void gl_init()
{
    int flags;

    if (gl_window) {
        SDL_SetWindowSize(gl_window, gl_width, gl_height);
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    flags = SDL_WINDOW_OPENGL;

    if (!gl_window_mode)
    {
        SDL_DisplayMode mode;

        flags |= SDL_WINDOW_FULLSCREEN;

        if (!SDL_GetDesktopDisplayMode(gl_display, &mode)) {
            gl_width = mode.w;
            gl_height = mode.h;
        }

        else {
            log_print(lninfo, "SDL_GetDesktopDisplayMode failed: %s",
                SDL_GetError());
        }
    }

    gl_window = SDL_CreateWindow("opengl",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        gl_width, gl_height, flags);

    gl_context = SDL_GL_CreateContext(gl_window);

    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max_texture_size);

    log_glstring(GL_EXTENSIONS);
    log_glstring(GL_VERSION);
    log_glstring(GL_RENDERER);
    log_glstring(GL_SHADING_LANGUAGE_VERSION);
    log_glstring(GL_VENDOR);
    log_dump("d", gl_max_texture_size);
}

void gl_perspective(float horizontal_fov, float near, float far)
{
    float aspect;
    float vertical_fov;
    float tan_half_fov;

    aspect = (float)gl_width / gl_height;
    tan_half_fov = SDL_tanf(radians(horizontal_fov) * 0.5f);
    vertical_fov = 2 * (float)SDL_atan(tan_half_fov / aspect);
    gluPerspective(degrees(vertical_fov), aspect, near, far);
}

/* --------------------------------------------------------------------- */

/*
 * using packed structs isn't portable in multiple ways but I'd rather keep
 * the code short and simple
 */

#ifdef _MSC_VER
#define packed(declaration) \
    __pragma pack(push, 1) \
    declaration \
    __pragma pack(pop)
#else
#define packed(declaration) \
    declaration __attribute__((__packed__))
#endif

packed(
struct bsp_dirent
{
    int offset;
    int length;
});

packed(
struct bsp_header
{
    char magic[4];
    int version;
    struct bsp_dirent dirents[17];
});

enum bsp_contents
{
    CONTENTS_SOLID = 1,
    LAST_CONTENTS
};

packed(
struct bsp_texture
{
    char name[64];
    int flags;
    int contents;
});

packed(
struct bsp_plane
{
    float normal[3];
    float dist;
});

packed(
struct bsp_node
{
    int plane;
    int child[2]; /* front, back */
    int mins[3];
    int maxs[3];
});

packed(
struct bsp_leaf
{
    int cluster;
    int area;
    int mins[3];
    int maxs[3];
    int leafface;
    int n_leaffaces;
    int leafbrush;
    int n_leafbrushes;
});

packed(
struct bsp_model
{
    float mins[3];
    float maxs[3];
    int face;
    int n_faces;
    int brush;
    int n_brushes;
});

packed(
struct bsp_brush
{
    int brushside;
    int n_brushsides;
    int texture;
});

packed(
struct bsp_brushside
{
    int plane;
    int texture;
});

packed(
struct bsp_vertex
{
    float position[3];
    float texcoord[2][2];
    float normal[3];
    int color;
});

packed(
struct bsp_effect
{
    char name[64];
    int brush;
    int unknown;
});

packed(
struct bsp_face
{
    int texture;
    int effect;
    int type;
    int vertex;
    int n_vertices;
    int meshvert;
    int n_meshverts;
    int lm_index;
    int lm_start[2];
    int lm_size[2];
    float lm_origin[3];
    float lm_vecs[2][3];
    float normal[3];
    int size[2];
});

enum bsp_face_type
{
    BSP_POLYGON = 1,
    BSP_PATCH,
    BSP_MESH,
    BSP_BILLBOARD
};

packed(
struct bsp_lightmap
{
    unsigned char map[128][128][3];
});

packed(
struct bsp_lightvol
{
    unsigned char ambient[3];
    unsigned char directional[3];
    unsigned char dir[2];
});

packed(
struct bsp_visdata
{
    int n_vecs;
    int sz_vecs;
});

struct bsp_file
{
    char* raw_data;
    struct bsp_header* header;

    char* entities;
    int entities_len;

    struct bsp_texture* textures;
    int n_textures;

    struct bsp_plane* planes;
    int n_planes;

    struct bsp_node* nodes;
    int n_nodes;

    struct bsp_leaf* leaves;
    int n_leaves;

    int* leaffaces;
    int n_leaffaces;

    int* leafbrushes;
    int n_leafbrushes;

    struct bsp_model* models;
    int n_models;

    struct bsp_brush* brushes;
    int n_brushes;

    struct bsp_brushside* brushsides;
    int n_brushsides;

    struct bsp_vertex* vertices;
    int n_vertices;

    int* meshverts;
    int n_meshverts;

    struct bsp_effect* effects;
    int n_effects;

    struct bsp_face* faces;
    int n_faces;

    struct bsp_lightmap* lightmaps;
    int n_lightmaps;

    struct bsp_lightvol* lightvols;
    int n_lightvols;

    struct bsp_visdata* visdata;
    unsigned char* visdata_vecs;
};

int bsp_load(struct bsp_file* file, char* path)
{
    char* p;
    struct bsp_dirent* dirents;

    log_puts(path);

    p = read_entire_file(path);
    if (!p) {
        return 0;
    }

    if (vec_len(p) < (int)sizeof(struct bsp_header)) {
        log_puts("E: file is too small, truncated header data");
        return 0;
    }

    file->raw_data = p;
    file->header = (struct bsp_header*)p;

    dirents = file->header->dirents;
    file->entities = p + dirents[0].offset;
    file->entities_len = dirents[0].length;

#define lump(i, name, type) \
    file->name = (type*)(p + dirents[i].offset); \
    file->n_##name = dirents[i].length / sizeof(type); \

    lump(1, textures, struct bsp_texture)
    lump(2, planes, struct bsp_plane)
    lump(3, nodes, struct bsp_node)
    lump(4, leaves, struct bsp_leaf)
    lump(5, leaffaces, int)
    lump(6, leafbrushes, int)
    lump(7, models, struct bsp_model)
    lump(8, brushes, struct bsp_brush)
    lump(9, brushsides, struct bsp_brushside)
    lump(10, vertices, struct bsp_vertex)
    lump(11, meshverts, int)
    lump(12, effects, struct bsp_effect)
    lump(13, faces, struct bsp_face)
    lump(14, lightmaps, struct bsp_lightmap)
    lump(15, lightvols, struct bsp_lightvol)

#undef lump

    file->visdata = (struct bsp_visdata*)(p + dirents[16].offset);
    file->visdata_vecs = (unsigned char*)&file->visdata[1];

    return 1;
}

/*
 * the plane dist is the distance from the origin along the normal so if
 * we project the camera position onto the normal with the dot product and
 * subtract dist, we get the distance between the camera position and the
 * plane
 *
 * note that leaf indices are negative, and that's how we tell we hit a
 * leaf while navigating the bsp tree
 *
 * since 0 is taken by positive indices, leaf index -1 maps to leaf 0,
 * -2 to 1, and so on
 */

int bsp_find_leaf(struct bsp_file* file, float* camera_pos)
{
    int index;

    index = 0;

    while (index >= 0)
    {
        float distance;
        struct bsp_node* node;
        struct bsp_plane* plane;

        node = &file->nodes[index];
        plane = &file->planes[node->plane];

        distance = dot3(camera_pos, plane->normal) - plane->dist;

        if (distance >= 0) {
            index = node->child[0];
        } else {
            index = node->child[1];
        }
    }

    return (-index) - 1;
}

/*
 * the visdata vecs are huge bitmasks. each bit is one y step and each
 * sz_vecs bytes is one x step
 *
 * cluster x is visible from cluster y if the y-th bit in the x-th bitmask
 * is set. it's basically a visibility matrix with every possible
 * combination of clusters packed as bitmask
 */

int bsp_cluster_visible(struct bsp_file* file, int from, int target)
{
    int index;

    index = from * file->visdata->sz_vecs + target / 8;
    return (file->visdata_vecs[index] & (1 << (target % 8))) != 0;
}

/* --------------------------------------------------------------------- */

/*
 * tiny lexer for the quake 3 entity syntax
 *
 * {
 * "key1" "value1"
 * "key2" "value2"
 * }
 * {
 * "key1" "value1"
 * "key2" "value2"
 * }
 * ...
 */

enum entities_token
{
    ENTITIES_LAST_LITERAL_TOKEN = 255,
    ENTITIES_STRING,
    ENTITIES_LAST_TOKEN
};

void describe_entities_token(char* dst, int dst_size, int kind)
{
    switch (kind)
    {
    case ENTITIES_STRING:
        SDL_snprintf(dst, dst_size, "%s", "string");
        break;

    default:
        if (kind >= 0 && kind <= ENTITIES_LAST_LITERAL_TOKEN) {
            SDL_snprintf(dst, dst_size, "'%c'", (char)kind);
        } else {
            SDL_snprintf(dst, dst_size, "unknown (%d)", kind);
        }
    }
}

struct entities_lexer
{
    char* p;
    int token_kind;
    char* str;
    int n_lines;
};

int lex_entities(struct entities_lexer* lex)
{
again:
    switch (*lex->p)
    {
    case '\n':
        ++lex->n_lines;
    case '\t':
    case '\v':
    case '\f':
    case '\r':
    case ' ':
        ++lex->p;
        goto again;

    case '"':
        lex->token_kind = ENTITIES_STRING;
        ++lex->p;
        lex->str = lex->p;

        for (; *lex->p != '"'; ++lex->p)
        {
            if (!*lex->p)
            {
                log_print(lninfo,
                    "W: unterminated string \"%s\" at line %d",
                    lex->str, lex->n_lines);
                break;
            }
        }

        if (*lex->p) {
            *lex->p++ = 0;
        }
        break;

    default:
        lex->token_kind = (int)*lex->p;
        ++lex->p;
    }

    return lex->token_kind;
}

/* --------------------------------------------------------------------- */

struct entity_field
{
    char* key;
    char* value;
};

struct entity_field** entities;

char* entity_get(struct entity_field* entity, char* key)
{
    int i;

    for (i = 0; i < vec_len(entity); ++i)
    {
        if (!strcmp(entity[i].key, key)) {
            return entity[i].value;
        }
    }

    return 0;
}

struct entity_field* entity_by_classname(char* classname)
{
    int i;

    for (i = 0; i < vec_len(entities); ++i)
    {
        char* cur_classname;

        cur_classname = entity_get(entities[i], "classname");

        if (cur_classname && !strcmp(cur_classname, classname)) {
            return entities[i];
        }
    }

    return 0;
}

int entities_expect(struct entities_lexer* lex, int kind)
{
    if (lex->token_kind != kind)
    {
        char got[64];
        char exp[64];

        describe_entities_token(got, sizeof(got), lex->token_kind);
        describe_entities_token(exp, sizeof(exp), kind);

        log_print(lninfo, "W: got %s, expected %s at line %d",
            got, exp, lex->n_lines);

        return 0;
    }

    lex_entities(lex);

    return 1;
}

void parse_entities(char* data)
{
    int i;
    struct entities_lexer lex;

    for (i = 0; i < vec_len(entities); ++i) {
        vec_free(entities[i]);
    }

    vec_clear(entities);

    memset(&lex, 0, sizeof(lex));
    lex.p = data;
    lex_entities(&lex);

    do
    {
        struct entity_field* fields = 0;

        if (!entities_expect(&lex, '{')) {
            return;
        }

        while (lex.token_kind == ENTITIES_STRING)
        {
            struct entity_field field;

            field.key = lex.str;
            lex_entities(&lex);

            if (lex.token_kind != ENTITIES_STRING) {
                return;
            }

            field.value = lex.str;
            vec_append(fields, field);

            lex_entities(&lex);
        }

        vec_append(entities, fields);

        if (!entities_expect(&lex, '}')) {
            return;
        }
    }
    while (lex.token_kind);
}

/* --------------------------------------------------------------------- */

/*
 * quake3 uses a different coordinate system, so we use quake's matrix
 * as identity, where x, y, z are forward, left and up
 */

float quake_matrix[] = {
    0, 0, -1, 0,
    -1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 0, 1
};

char* argv0;
int running = 1;
float delta_time;

struct patch
{
    int n_vertices;
    struct bsp_vertex* vertices;
    int n_indices;
    int* indices;
    int n_rows;
    int triangles_per_row;
};

char* map_file;
struct bsp_file map;
int* visible_faces;
unsigned char* visible_faces_mask;
struct patch** patches;

enum plane_type
{
    PLANE_X,
    PLANE_Y,
    PLANE_Z,
    PLANE_NONAXIAL,
    LAST_PLANE_TYPE
};

struct plane
{
    int signbits;
    int type;
};

struct plane* planes;

int tessellation_level;
float camera_pos[3];
float camera_angle[2]; /* yaw, pitch */
float velocity[3];
int noclip;

int movement;
float wishdir[3]; /* movement inputs in local player space, not unit */
int wishlook[2]; /* look inputs in screen space, not unit */
float* ground_normal;

enum movement_bits
{
    MOVEMENT_JUMP = 1<<1,
    MOVEMENT_JUMP_THIS_FRAME = 1<<2,
    MOVEMENT_JUMPING = 1<<3,
    LAST_MOVEMENT_BIT
};

float player_mins[] = { -15, -15, -24 };
float player_maxs[] = { 15, 15, 32 };

float cl_forwardspeed = 400;
float cl_sidespeed = 350;
float cl_movement_accelerate = 15;
float cl_movement_airaccelerate = 7;
float cl_movement_friction = 8;
float sv_gravity = 800;
float sv_max_speed = 320;
float cl_stop_speed = 200;
float cpm_air_stop_acceleration = 2.5f;
float cpm_air_control_amount = 150;
float cpm_strafe_acceleration = 70;
float cpm_wish_speed = 30;

void print_usage()
{
    SDL_Log(
        "usage: %s [options] /path/to/file.bsp\n"
        "\n"
        "available options:\n"
        "    -window: window mode | default: off | example: -window\n"
        "    -d: main display index | default: 0 | example: -d 0\n"
        "    -t: tessellation level | default: 5 | example: -t 10\n"
        "    -w: window width | default: 1280 | example: -w 800\n"
        "    -h: window height | default: 720 | example: -h 600\n",
        argv0
    );
}

void parse_args(int argc, char* argv[])
{
    argv0 = argv[0];

    ++argv, --argc;

    for (; argc > 0; ++argv, --argc)
    {
        if (!strcmp(argv[0], "-window")) {
            gl_window_mode = 1;
        }

        else if (!strcmp(argv[0], "-d") && argc >= 2) {
            gl_display = SDL_atoi(argv[1]);
            ++argv, --argc;
        }

        else if (!strcmp(argv[0], "-t") && argc >= 2) {
            tessellation_level = SDL_atoi(argv[1]);
            ++argv, --argc;
        }

        else if (!strcmp(argv[0], "-w") && argc >= 2) {
            gl_width = SDL_atoi(argv[1]);
            ++argv, --argc;
        }

        else if (!strcmp(argv[0], "-h") && argc >= 2) {
            gl_height = SDL_atoi(argv[1]);
            ++argv, --argc;
        }

        else {
            break;
        }
    }

    if (argc >= 1) {
        map_file = argv[0];
    } else {
        print_usage();
        exit(1);
    }

    if (tessellation_level <= 0) {
        tessellation_level = 5;
    }

    if (gl_width <= 0) {
        gl_width = 1280;
    }

    if (gl_height <= 0) {
        gl_height = 720;
    }
}

void update_fps()
{
    static float one_second = 1;
    static int ticks_per_second = 0;

    one_second -= delta_time;

    while (one_second <= 0)
    {
        log_dump("d", ticks_per_second);
        log_dump("f", mag3(velocity));
        one_second = 1;
        ticks_per_second = 0;
    }

    ++ticks_per_second;
}

/*
 * this is slow but it makes code look nicer and we only use it on init
 * anyway so it's not a real performance hit
 */

struct bsp_vertex mul_vertex(struct bsp_vertex* vertex, float scalar)
{
    struct bsp_vertex res;

    res = *vertex;
    mul3_scalar(res.position, scalar);
    mul2_scalar(res.texcoord[0], scalar);
    mul2_scalar(res.texcoord[1], scalar);
    mul3_scalar(res.normal, scalar);

    return res;
}

struct bsp_vertex add_vertices(struct bsp_vertex a, struct bsp_vertex b)
{
    add3(a.position, b.position);
    add2(a.texcoord[0], b.texcoord[0]);
    add2(a.texcoord[1], b.texcoord[1]);
    add3(a.normal, b.normal);

    return a;
}

#define add_vertices3(a, b, c) add_vertices(add_vertices(a, b), c)

void tessellate(struct patch* patch, struct bsp_vertex* controls,
    int level)
{
    int i, j;
    int l1;
    struct bsp_vertex* vertices;
    int* indices;

    l1 = level + 1;

    patch->n_vertices = l1 * l1;
    patch->vertices = (struct bsp_vertex*)
        SDL_malloc(sizeof(struct bsp_vertex) * patch->n_vertices);
    vertices = patch->vertices;

    for (i = 0; i <= level; ++i)
    {
        float a, b;

        a = (float)i / level;
        b = 1 - a;

        vertices[i] = add_vertices3(
            mul_vertex(&controls[0], b * b),
            mul_vertex(&controls[3], 2 * b * a),
            mul_vertex(&controls[6], a * a));
    }

    for (i = 1; i <= level; ++i)
    {
        float a, b;
        struct bsp_vertex sum[3];

        a = (float)i / level;
        b = 1 - a;

        for (j = 0; j < 3; ++j)
        {
            sum[j] = add_vertices3(
                mul_vertex(&controls[3 * j + 0], b * b),
                mul_vertex(&controls[3 * j + 1], 2 * b * a),
                mul_vertex(&controls[3 * j + 2], a * a));
        }

        for (j = 0; j <= level; ++j)
        {
            float c, d;

            c = (float)j / level;
            d = 1 - c;

            vertices[i * l1 + j] = add_vertices3(
                mul_vertex(&sum[0], d * d),
                mul_vertex(&sum[1], 2 * c * d),
                mul_vertex(&sum[2], c * c));
        }
    }

    patch->indices = SDL_malloc(sizeof(int) * level * l1 * 2);
    indices = patch->indices;

    for (i = 0; i < level; ++i)
    {
        for (j = 0; j <= level; ++j)
        {
            indices[(i * l1 + j) * 2 + 1] = i * l1 + j;
            indices[(i * l1 + j) * 2] = (i + 1) * l1 + j;
        }
    }

    patch->triangles_per_row = 2 * l1;
    patch->n_rows = level;
}

void tessellate_face(int face_index)
{
    struct bsp_face* face;
    int width, height;
    int x, y, row, col;

    face = &map.faces[face_index];

    if (face->type != BSP_PATCH) {
        return;
    }

    /* theres' multiple sets of bezier control points per face */
    width = (face->size[0] - 1) / 2;
    height = (face->size[1] - 1) / 2;

    patches[face_index] =
        SDL_malloc(width * height * sizeof(patches[0][0]));

    /* TODO: there's way too much nesting in here, improve it */
    for (y = 0; y < height; ++y)
    {
        for (x = 0; x < width; ++x)
        {
            struct patch* patch;
            struct bsp_vertex controls[9];

            for (row = 0; row < 3; ++row)
            {
                for (col = 0; col < 3; ++col)
                {
                    int index;

                    index = face->vertex +
                        y * 2 * face->size[0] + x * 2 +
                        row * face->size[0] + col;

                    controls[row * 3 + col] = map.vertices[index];
                }
            }

            patch = &patches[face_index][y * width + x];
            tessellate(patch, controls, tessellation_level);
        }
    }
}

#define SURF_CLIP_EPSILON 0.125f

enum tw_flags
{
    TW_STARTS_OUT = 1<<1,
    TW_ENDS_OUT = 1<<2,
    TW_ALL_SOLID = 1<<3,
    TW_LAST_FLAG
};

struct trace_work
{
    float start[3];
    float end[3];
    float endpos[3];
    float frac;
    int flags;
    float mins[3];
    float maxs[3];
    float offsets[8][3];
    struct bsp_plane* plane;
};

/*
 * - adjust plane dist to account for the bounding box
 * - if both points are in front of the plane, we're done with this brush
 * - if both points are behind the plane, we continue looping expecting
 *   other planes to clip us
 * - if we are entering the brush, clip start_frac to the distance
 *   between the starting point and the brush minus the epsilon so we don't
 *   actually touch
 * - if we are exiting the brush, clip end_frac to the distance between
 *   the starting point and the brush plus the epsilon so we don't
 *   actually touch
 * - keep collecting the maximum start_frac and minimum end_faction so
 *   we get as close as possible to touching the brush but not quite
 * - store the minimum start_frac out of all the brushes so we
 *   clip against the closest brush
 * - if the trace starts and ends inside the brush, negate the move
 *
 * (I assume this means that the brush sides are sorted from back to front)
 */

void trace_brush(struct trace_work* work, struct bsp_brush* brush)
{
    int i;
    float start_frac;
    float end_frac;
    struct bsp_plane* closest_plane;

    /* TODO: do optimized check for the first 6 planes which are axial */

    start_frac = -1;
    end_frac = 1;

    for (i = 0; i < brush->n_brushsides; ++i)
    {
        int side_index;
        int plane_index;
        struct bsp_plane* plane;
        int signbits;

        float dist;
        float start_distance, end_distance;
        float frac;

        side_index = brush->brushside + i;
        plane_index = map.brushsides[side_index].plane;
        plane = &map.planes[plane_index];
        signbits = planes[plane_index].signbits;

        dist = plane->dist - dot3(work->offsets[signbits], plane->normal);

        start_distance = dot3(work->start, plane->normal) - dist;
        end_distance = dot3(work->end, plane->normal) - dist;

        /* TODO:
         * for some reason these checks incorrectly report all solid
         * when they shouldn't. for now I'm just ignoring them
         */

        if (start_distance > 0) {
            work->flags |= TW_STARTS_OUT;
        }

        if (end_distance > 0) {
            work->flags |= TW_ENDS_OUT;
        }

        if (start_distance > 0 &&
            (end_distance >= SURF_CLIP_EPSILON ||
             end_distance >= start_distance))
        {
            return;
        }

        if (start_distance <= 0 && end_distance <= 0) {
            continue;
        }

        if (start_distance > end_distance)
        {
            frac = (start_distance - SURF_CLIP_EPSILON) /
                (start_distance - end_distance);

            if (frac > start_frac) {
                start_frac = frac;
                closest_plane = plane;
            }
        }

        else
        {
            frac = (start_distance + SURF_CLIP_EPSILON) /
                (start_distance - end_distance);

            end_frac = SDL_min(end_frac, frac);
        }
    }

    if (start_frac < end_frac &&
        start_frac > -1 && start_frac < work->frac)
    {
        work->frac = SDL_max(start_frac, 0);
        work->plane = closest_plane;
    }

    if (!(work->flags & (TW_STARTS_OUT | TW_ENDS_OUT))) {
        work->frac = 0;
    }
}

/*
 * - for leaves, only trace brush if the contents are solid and the brush
 *   has sides
 * - for nodes, recurse into front/back if both start and end are in front
 *   of, or behind the node's plane
 * - if start -> end crosses over two nodes, we need to recurse into both
 *   nodes and split the start -> end segment into two segment that are
 *   just short of crossing over by adding SURFACE_CLIP_EPSILON
 * - planes contain an enum that can tell us if they are axis aligned.
 *   when they are axis aligned, we can skip some calculations because
 *   the bounding box is also axis aligned
 */

void trace_leaf(struct trace_work* work, int index)
{
    int i;
    struct bsp_leaf* leaf;

    leaf = &map.leaves[index];

    for (i = 0; i < leaf->n_leafbrushes; ++i)
    {
        struct bsp_brush* brush;
        int contents;
        int brush_index;

        brush_index = map.leafbrushes[leaf->leafbrush + i];
        brush = &map.brushes[brush_index];
        contents = map.textures[brush->texture].contents;

        if (brush->n_brushsides && (contents & CONTENTS_SOLID))
        {
            trace_brush(work, brush);

            if (!work->frac) {
                return;
            }
        }
    }

    /* TODO: collision with patches */
}

void trace_node(struct trace_work* work, int index, float start_frac,
    float end_frac, float* start, float* end)
{
    int i;
    struct bsp_node* node;
    struct bsp_plane* plane;
    int plane_type;

    float start_distance;
    float end_distance;
    float offset;

    int side;
    float idistance;
    float frac1;
    float frac2;
    float mid_frac;
    float mid[3];

    if (index < 0) {
        trace_leaf(work, (-index) - 1);
        return;
    }

    node = &map.nodes[index];
    plane = &map.planes[node->plane];
    plane_type = planes[node->plane].type;

    if (plane_type < 3)
    {
        start_distance = start[plane_type] - plane->dist;
        end_distance = end[plane_type] - plane->dist;
        offset = work->maxs[plane_type];
    }
    else
    {
        start_distance = dot3(start, plane->normal) - plane->dist;
        end_distance = dot3(end, plane->normal) - plane->dist;

        if (eq3(work->mins, work->maxs)) {
            offset = 0;
        } else {
            /* "this is silly" - id Software */
            offset = 2048;
        }
    }

    if (start_distance >= offset + 1 && end_distance >= offset + 1) {
        trace_node(work, node->child[0], start_frac, end_frac, start, end);
        return;
    }

    if (start_distance < -offset - 1 && end_distance < -offset - 1) {
        trace_node(work, node->child[1], start_frac, end_frac, start, end);
        return;
    }

    if (start_distance < end_distance)
    {
        side = 1;
        idistance = 1.0f / (start_distance - end_distance);
        frac1 = (start_distance - offset + SURF_CLIP_EPSILON) * idistance;
        frac2 = (start_distance + offset + SURF_CLIP_EPSILON) * idistance;
    }

    else if (start_distance > end_distance)
    {
        side = 0;
        idistance = 1.0f / (start_distance - end_distance);
        frac1 = (start_distance + offset + SURF_CLIP_EPSILON) * idistance;
        frac2 = (start_distance - offset - SURF_CLIP_EPSILON) * idistance;
    }

    else
    {
        side = 0;
        frac1 = 1;
        frac2 = 0;
    }

    frac1 = SDL_max(0, SDL_min(1, frac1));
    frac2 = SDL_max(0, SDL_min(1, frac2));

    mid_frac = start_frac + (end_frac - start_frac) * frac1;

    for (i = 0; i < 3; ++i) {
        mid[i] = start[i] + (end[i] - start[i]) * frac1;
    }

    trace_node(work, node->child[side], start_frac, mid_frac, start, mid);

    mid_frac = start_frac + (end_frac - start_frac) * frac2;

    for (i = 0; i < 3; ++i) {
        mid[i] = start[i] + (end[i] - start[i]) * frac2;
    }

    trace_node(work, node->child[side^1], mid_frac, end_frac, mid, end);
}

/*
 * - adjust bounding box so it's symmetric. this is simply done by finding
 *   the middle point and moving start/end to align with it
 * - initialize offsets. this is a lookup table for mins/maxs with any
 *   sign combination for the plane's normal. it ensures that we account
 *   for the hitbox in the right orientation in trace_brush
 * - do the tracing
 * - if we hit anything, calculate end from the unmodified start/end
 */

void trace(struct trace_work* work, float* start, float* end, float* mins,
    float* maxs)
{
    int i;

    work->frac = 1;
    work->flags = 0;

    for (i = 0; i < 3; ++i)
    {
        float offset;

        offset = (mins[i] + maxs[i]) * 0.5f;
        work->mins[i] = mins[i] - offset;
        work->maxs[i] = maxs[i] - offset;
        work->start[i] = start[i] + offset;
        work->end[i] = end[i] + offset;
    }

    work->offsets[0][0] = work->mins[0];
    work->offsets[0][1] = work->mins[1];
    work->offsets[0][2] = work->mins[2];

    work->offsets[1][0] = work->maxs[0];
    work->offsets[1][1] = work->mins[1];
    work->offsets[1][2] = work->mins[2];

    work->offsets[2][0] = work->mins[0];
    work->offsets[2][1] = work->maxs[1];
    work->offsets[2][2] = work->mins[2];

    work->offsets[3][0] = work->maxs[0];
    work->offsets[3][1] = work->maxs[1];
    work->offsets[3][2] = work->mins[2];

    work->offsets[4][0] = work->mins[0];
    work->offsets[4][1] = work->mins[1];
    work->offsets[4][2] = work->maxs[2];

    work->offsets[5][0] = work->maxs[0];
    work->offsets[5][1] = work->mins[1];
    work->offsets[5][2] = work->maxs[2];

    work->offsets[6][0] = work->mins[0];
    work->offsets[6][1] = work->maxs[1];
    work->offsets[6][2] = work->maxs[2];

    work->offsets[7][0] = work->maxs[0];
    work->offsets[7][1] = work->maxs[1];
    work->offsets[7][2] = work->maxs[2];

    trace_node(work, 0, 0, 1, work->start, work->end);

    if (work->frac == 1) {
        cpy3(work->endpos, end);
    } else {
        int i;

        for (i = 0; i < 3; ++i) {
            work->endpos[i] = start[i] + work->frac * (end[i] - start[i]);
        }
    }
}

void trace_point(struct trace_work* work, float* start, float* end)
{
    float zero[3];

    clr3(zero);
    trace(work, start, end, zero, zero);
}

int plane_type_for_normal(float* normal)
{
    if (normal[0] == 1.0f || normal[0] == -1.0f) {
        return PLANE_X;
    }

    if (normal[1] == 1.0f || normal[1] == -1.0f) {
        return PLANE_Y;
    }

    if (normal[2] == 1.0f || normal[2] == -1.0f) {
        return PLANE_Z;
    }

    return PLANE_NONAXIAL;
}

int signbits_for_normal(float* normal)
{
    int i;
    int bits;

    bits = 0;

    for (i = 0; i < 3; ++i)
    {
        if (normal[i] < 0) {
            bits |= 1<<i;
        }
    }

    return bits;
}

void init_planes()
{
    int i;

    planes = (struct plane*)
        SDL_realloc(planes, map.n_planes * sizeof(struct plane));

    for (i = 0; i < map.n_planes; ++i) {
        planes[i].signbits = signbits_for_normal(map.planes[i].normal);
        planes[i].type = plane_type_for_normal(map.planes[i].normal);
    }
}

void init_patches()
{
    int i;

    for (i = 0; patches && i < map.n_faces; ++i)
    {
        int j;
        int npatches;

        npatches = (map.faces[i].size[0] - 1) / 2;
        npatches *= (map.faces[i].size[1] - 1) / 2;

        for (j = 0; j < npatches; ++j)
        {
            SDL_free(patches[i][j].vertices);
            SDL_free(patches[i][j].indices);
        }
    }

    patches = (struct patch**)
        SDL_realloc(patches, map.n_faces * sizeof(patches[0]));

    memset(patches, 0, map.n_faces * sizeof(patches[0]));

    for (i = 0; i < map.n_faces; ++i) {
        tessellate_face(i);
    }
}

void init_spawn()
{
    struct entity_field* spawn;
    char* angle;
    char* origin;
    int i;

    spawn = entity_by_classname("info_player_deathmatch");
    if (!spawn) {
        return;
    }

    angle = entity_get(spawn, "angle");
    if (angle) {
        camera_angle[0] = radians(atoi(angle));
    }

    origin = entity_get(spawn, "origin");

    for (i = 0; origin && *origin && i < 3; ++i) {
        camera_pos[i] = (float)SDL_strtod(origin, &origin);
    }

    camera_pos[2] += 60;

    log_print(lninfo, "[%f %f %f] %f degrees",
        expand3(camera_pos), degrees(camera_angle[0]));
}

void init_map()
{
    unsigned start;
    char* entities_str;

    start = SDL_GetTicks();

    if (!map_file) {
        return;
    }

    if (!bsp_load(&map, map_file)) {
        exit(1);
    }

    log_puts("preprocessing planes");
    init_planes();

    log_puts("tessellating geometry");
    init_patches();

    log_puts("parsing entities");
    entities_str = SDL_malloc(map.entities_len + 1);
    SDL_memcpy(entities_str, map.entities, map.entities_len);
    entities_str[map.entities_len] = 0;

    parse_entities(entities_str);
    init_spawn();

    SDL_free(entities_str);

    log_print(lninfo, "completed in %fs",
        (SDL_GetTicks() - start) / 1000.0f);
}

void init(int argc, char* argv[])
{
    parse_args(argc, argv);

    gl_init();

    SDL_ShowCursor(SDL_DISABLE);
    SDL_SetRelativeMouseMode(SDL_TRUE);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gl_perspective(110, 0.1f, 10000.0f);
    glMatrixMode(GL_MODELVIEW);

    init_map();

    visible_faces =
        (int*)SDL_realloc(visible_faces, sizeof(int) * map.n_faces);

    visible_faces_mask =
        (unsigned char*)SDL_realloc(visible_faces_mask, map.n_faces / 8);
}

void clamp_angles(float* angles, int n_angles)
{
    int i;

    for (i = 0; i < n_angles; ++i)
    {
        for (; angles[i] < 0; angles[i] += 2*M_PI);
        for (; angles[i] >= 2*M_PI; angles[i] -= 2*M_PI);
    }
}

void trace_ground()
{
    float point[3];
    struct trace_work work;

    point[0] = camera_pos[0];
    point[1] = camera_pos[1];
    point[2] = camera_pos[2] - 0.25;

    trace(&work, camera_pos, point, player_mins, player_maxs);

    if (work.frac == 1 || (movement & MOVEMENT_JUMP_THIS_FRAME)) {
        movement |= MOVEMENT_JUMPING;
        ground_normal = 0;
    } else {
        movement &= ~MOVEMENT_JUMPING;
        ground_normal = work.plane->normal;
    }
}

void apply_jump()
{
    if (!(movement & MOVEMENT_JUMP)) {
        return;
    }

    if ((movement & MOVEMENT_JUMPING) && !noclip) {
        return;
    }

    movement |= MOVEMENT_JUMP_THIS_FRAME;
    velocity[2] = 270;
    movement &= ~MOVEMENT_JUMP; /* no auto bunnyhop */
}

void apply_friction()
{
    float speed;
    float control;
    float new_speed;

    if (!noclip)
    {
        if ((movement & MOVEMENT_JUMPING) ||
            (movement & MOVEMENT_JUMP_THIS_FRAME))
        {
            return;
        }
    }

    speed = (float)SDL_sqrt(dot3(velocity, velocity));
    if (speed < 1) {
        velocity[0] = 0;
        velocity[1] = 0;
        return;
    }

    control = speed < cl_stop_speed ? cl_stop_speed : speed;
    new_speed = speed - control * cl_movement_friction * delta_time;
    new_speed = SDL_max(0, new_speed);
    mul3_scalar(velocity, new_speed / speed);
}

void apply_acceleration(float* direction, float wishspeed,
    float acceleration)
{
    float cur_speed;
    float add_speed;
    float accel_speed;
    float amount[3];

    if (!noclip && (movement & MOVEMENT_JUMPING)) {
        wishspeed = SDL_min(cpm_wish_speed, wishspeed);
    }

    cur_speed = dot3(velocity, direction);
    add_speed = wishspeed - cur_speed;

    if (add_speed <= 0) {
        return;
    }

    accel_speed = acceleration * delta_time * wishspeed;
    accel_speed = SDL_min(accel_speed, add_speed);

    cpy3(amount, direction);
    mul3_scalar(amount, accel_speed);
    add3(velocity, amount);
}

void apply_air_control(float* direction, float wishspeed)
{
    float zspeed;
    float speed;
    float dot;

    if (wishdir[0] == 0 || wishspeed == 0) {
        return;
    }

    zspeed = velocity[2];
    velocity[2] = 0;
    speed = mag3(velocity);
    if (speed >= 0.0001f) {
        div3_scalar(velocity, speed);
    }
    dot = dot3(velocity, direction);

    if (dot > 0) {
        /* can only change direction if we aren't trying to slow down */
        float k;
        float amount[3];

        k = 32 * cpm_air_control_amount * dot * dot * delta_time;
        mul3_scalar(velocity, speed);
        cpy3(amount, direction);
        mul3_scalar(amount, k);
        nrm3(velocity);
    }

    mul3_scalar(velocity, speed);
    velocity[2] = zspeed;
}

void apply_inputs()
{
    float direction[3];
    float pitch_sin, pitch_cos, yaw_sin, yaw_cos;
    float pitch_x;
    float wishspeed;
    float selected_acceleration;
    float base_wishspeed;

    /* camera look */
    camera_angle[0] += 0.002f * wishlook[0];
    camera_angle[1] += 0.002f * wishlook[1];
    clamp_angles(camera_angle, 2);

    if (noclip) {
        pitch_sin = SDL_sinf(2*M_PI - camera_angle[1]);
        pitch_cos = SDL_cosf(2*M_PI - camera_angle[1]);
    } else {
        pitch_sin = 0;
        pitch_cos = 1;
    }

    yaw_sin = SDL_sinf(2*M_PI - camera_angle[0]);
    yaw_cos = SDL_cosf(2*M_PI - camera_angle[0]);

    /* this applies 2 rotations, pitch first then yaw */
    pitch_x = wishdir[0] * pitch_cos + wishdir[2] * (-pitch_sin);
    direction[0] = pitch_x * yaw_cos + wishdir[1] * (-yaw_sin);
    direction[1] = pitch_x * yaw_sin + wishdir[1] * yaw_cos;
    direction[2] = wishdir[0] * pitch_sin + wishdir[2] * pitch_cos;

    /* movement */
    wishspeed = (float)SDL_sqrt(dot3(direction, direction));
    if (wishspeed >= 0.0001f) {
        div3_scalar(direction, wishspeed);
    }
    wishspeed = SDL_min(wishspeed, sv_max_speed);

    apply_jump();
    apply_friction();

    selected_acceleration = cl_movement_accelerate;
    base_wishspeed = wishspeed;

    /* cpm air acceleration | TODO: pull this out */
    if (noclip || (movement & MOVEMENT_JUMPING) ||
        (movement & MOVEMENT_JUMP_THIS_FRAME))
    {
        if (dot3(velocity, direction) < 0) {
            selected_acceleration = cpm_air_stop_acceleration;
        } else {
            selected_acceleration = cl_movement_airaccelerate;
        }

        if (wishdir[1] != 0 && wishdir[0] == 0) {
            wishspeed = SDL_min(wishspeed, cpm_wish_speed);
            selected_acceleration = cpm_strafe_acceleration;
        }
    }

    apply_acceleration(direction, wishspeed, selected_acceleration);
    apply_air_control(direction, base_wishspeed);
}

void clip_velocity(float* in, float* normal, float* out, float overbounce)
{
    float backoff;
    int i;

    backoff = dot3(in, normal);

    if (backoff < 0) {
        backoff *= overbounce;
    } else {
        backoff /= overbounce;
    }

    for (i = 0; i < 3; ++i)
    {
        float change;

        change = normal[i] * backoff;
        out[i] = in[i] - change;
    }
}

/*
 * clip the velocity against all brushes until we stop colliding. this
 * allows the player to slide against walls and the floor
 */

#define OVERCLIP 1.001f
#define MAX_CLIP_PLANES 5

int slide(int gravity)
{
    float end_velocity[3];
    float planes[MAX_CLIP_PLANES][3];
    int n_planes;
    float time_left;
    int n_bumps;
    float end[3];

    n_planes = 0;
    time_left = delta_time;

    if (gravity)
    {
        cpy3(end_velocity, velocity);
        end_velocity[2] -= sv_gravity * delta_time;

        /*
         * not 100% sure why this is necessary, maybe to avoid tunneling
         * through the floor when really close to it
         */

        velocity[2] = (end_velocity[2] + velocity[2]) * 0.5f;

        /* slide against floor */
        if (ground_normal) {
            clip_velocity(velocity, ground_normal, velocity, OVERCLIP);
        }
    }

    if (ground_normal) {
        cpy3(planes[n_planes], ground_normal);
        ++n_planes;
    }

    cpy3(planes[n_planes], velocity);
    nrm3(planes[n_planes]);
    ++n_planes;

    for (n_bumps = 0; n_bumps < 4; ++n_bumps)
    {
        struct trace_work work;
        int i;

        /* calculate future position and attempt the move */
        cpy3(end, velocity);
        mul3_scalar(end, time_left);
        add3(end, camera_pos);
        trace(&work, camera_pos, end, player_mins, player_maxs);

        if (work.frac > 0) {
            cpy3(camera_pos, work.endpos);
        }

        /* if nothing blocked us we are done */
        if (work.frac == 1) {
            break;
        }

        time_left -= time_left * work.frac;

        if (n_planes >= MAX_CLIP_PLANES) {
            clr3(velocity);
            return 1;
        }

        /*
         * if it's a plane we hit before, nudge velocity along it
         * to prevent epsilon issues and dont re-test it
         */

        for (i = 0; i < n_planes; ++i)
        {
            if (dot3(work.plane->normal, planes[i]) > 0.99) {
                add3(velocity, work.plane->normal);
                break;
            }
        }

        if (i < n_planes) {
            continue;
        }

        /*
         * entirely new plane, add it and clip velocity against all
         * planes that the move interacts with
         */

        cpy3(planes[n_planes], work.plane->normal);
        ++n_planes;

        for (i = 0; i < n_planes; ++i)
        {
            float clipped[3];
            float end_clipped[3];
            int j;

            if (dot3(velocity, planes[i]) >= 0.1) {
                continue;
            }

            clip_velocity(velocity, planes[i], clipped, OVERCLIP);
            clip_velocity(end_velocity, planes[i], end_clipped, OVERCLIP);

            /*
             * if the clipped move still hits another plane, slide along
             * the line where the two planes meet (cross product) with the
             * un-clipped velocity
             *
             * TODO: reduce nesting in here
             */

            for (j = 0; j < n_planes; ++j)
            {
                int k;
                float dir[3];
                float speed;

                if (j == i) {
                    continue;
                }

                if (dot3(clipped, planes[j]) >= 0.1) {
                    continue;
                }

                clip_velocity(clipped, planes[j], clipped, OVERCLIP);
                clip_velocity(end_clipped, planes[j], end_clipped,
                    OVERCLIP);

                if (dot3(clipped, planes[i]) >= 0) {
                    /* goes back into the first plane */
                    continue;
                }

                cross3(planes[i], planes[j], dir);
                nrm3(dir);

                speed = dot3(dir, velocity);
                cpy3(clipped, dir);
                mul3_scalar(clipped, speed);

                speed = dot3(dir, end_velocity);
                cpy3(end_clipped, dir);
                mul3_scalar(end_clipped, speed);

                /* if we still hit a plane, just give up and dead stop */

                for (k = 0; k < n_planes; ++k)
                {
                    if (k == j || k == i) {
                        continue;
                    }

                    if (dot3(clipped, planes[k]) >= 0.1) {
                        continue;
                    }

                    clr3(velocity);
                    return 1;
                }
            }

            /* resolved all collisions for this move */
            cpy3(velocity, clipped);
            cpy3(end_velocity, end_clipped);
            break;
        }
    }

    if (gravity) {
        cpy3(velocity, end_velocity);
    }

    return n_bumps != 0;
}

void update()
{
    float amount[3];

    update_fps();
    trace_ground();
    apply_inputs();

    if (!noclip) {
        slide((movement & MOVEMENT_JUMPING) != 0);
    }

    else
    {
        cpy3(amount, velocity);
        mul3_scalar(amount, delta_time);
        add3(camera_pos, amount);
    }

    movement &= ~MOVEMENT_JUMP_THIS_FRAME;
}

void render_mesh(struct bsp_face* face)
{
    int stride;

    stride = sizeof(struct bsp_vertex);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glVertexPointer(3, GL_FLOAT, stride,
        &map.vertices[face->vertex].position);

    glColorPointer(4, GL_UNSIGNED_BYTE, stride,
        &map.vertices[face->vertex].color);

    glDrawElements(GL_TRIANGLES, face->n_meshverts, GL_UNSIGNED_INT,
        &map.meshverts[face->meshvert]);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
}

void render_patch(struct patch* patch)
{
    int stride;
    int i;

    stride = sizeof(struct bsp_vertex);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glVertexPointer(3, GL_FLOAT, stride, &patch->vertices[0].position);
    glColorPointer(4, GL_UNSIGNED_BYTE, stride, &patch->vertices[0].color);

    for (i = 0; i < patch->n_rows; ++i)
    {
        glDrawElements(GL_TRIANGLE_STRIP,
            patch->triangles_per_row, GL_UNSIGNED_INT,
            &patch->indices[i * patch->triangles_per_row]);
    }

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
}

/*
 * - find the leaf and cluster we are in
 * - find out which leaves are visible from here
 * - for each of the visible leaves, append all of its faces to the list
 *   of faces to be rendered
 * - because faces can be included twice, we use a huge bitmask to keep
 *   track of which faces we already added, should be faster than searching
 *   the array every time
 * - render the faces, camera view frustum culling is handled by hardware
 * - z-order culling is also handled by the hardware but it might be
 *   a good idea to sort opaque triangles front to back
 * - with textures you would want to sort faces by texture to minimize
 *   texture switching (or build an atlas with all the textures)
 */

void render()
{
    int i, j;
    int leaf_index;
    struct bsp_leaf* leaf;
    int cluster;
    int n_visible_faces;

    leaf_index = bsp_find_leaf(&map, camera_pos);
    leaf = &map.leaves[leaf_index];
    cluster = leaf->cluster;

    n_visible_faces = 0;
    memset(visible_faces_mask, 0, map.n_faces / 8);

    for (i = 0; i < map.n_leaves; ++i)
    {
        int j;
        int first_face;
        int n_faces;

        if (!bsp_cluster_visible(&map, cluster, map.leaves[i].cluster)) {
            continue;
        }

        first_face = map.leaves[i].leafface;
        n_faces = map.leaves[i].n_leaffaces;

        for (j = first_face; j < first_face + n_faces; ++j)
        {
            int face_index;
            int face_bit;

            face_index = map.leaffaces[j];
            face_bit = 1 << (face_index % 8);

            if (visible_faces_mask[face_index / 8] & face_bit) {
                continue;
            }

            visible_faces[n_visible_faces++] = face_index;
            visible_faces_mask[face_index / 8] |= face_bit;
        }
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadMatrixf(quake_matrix);
    glRotatef(degrees(camera_angle[1]), 0, -1, 0);
    glRotatef(degrees(camera_angle[0]), 0, 0, 1);
    glTranslatef(-camera_pos[0], -camera_pos[1], -camera_pos[2] - 30);

    for (i = 0; i < n_visible_faces; ++i)
    {
        int face_index;
        struct bsp_face* face;
        int npatches;

        face_index = visible_faces[i];
        face = &map.faces[face_index];

        switch (face->type)
        {
        case BSP_BILLBOARD:
            /* TODO */
            break;

        case BSP_POLYGON:
        case BSP_MESH:
            render_mesh(face);
            break;

        case BSP_PATCH:
            npatches = (face->size[0] - 1) / 2;
            npatches *= (face->size[1] - 1) / 2;

            for (j = 0; j < npatches; ++j) {
                render_patch(&patches[face_index][j]);
            }
            break;
        }
    }

    SDL_GL_SwapWindow(gl_window);
}

void tick()
{
    update();
    render();
}

/* --------------------------------------------------------------------- */

void handle(SDL_Event* e)
{
    switch (e->type)
    {
    case SDL_QUIT:
        running = 0;
        break;

    case SDL_KEYDOWN:
        if (e->key.repeat) {
            break;
        }

        switch (e->key.keysym.sym)
        {
        case SDLK_ESCAPE:
            running = 0;
            break;
        case SDLK_w:
            wishdir[0] = cl_forwardspeed;
            break;
        case SDLK_s:
            wishdir[0] = -cl_forwardspeed;
            break;
        case SDLK_a:
            wishdir[1] = cl_sidespeed;
            break;
        case SDLK_d:
            wishdir[1] = -cl_sidespeed;
            break;
        case SDLK_f:
            noclip ^= 1;
            log_dump("d", noclip);
            break;
        case SDLK_SPACE:
            movement |= MOVEMENT_JUMP;
            break;
        }
        break;

    case SDL_KEYUP:
        switch (e->key.keysym.sym)
        {
        case SDLK_ESCAPE:
            running = 0;
            break;
        case SDLK_w:
            wishdir[0] = 0;
            break;
        case SDLK_s:
            wishdir[0] = 0;
            break;
        case SDLK_a:
            wishdir[1] = 0;
            break;
        case SDLK_d:
            wishdir[1] = 0;
            break;
        case SDLK_SPACE:
            movement &= ~MOVEMENT_JUMP;
            break;
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_RIGHT) {
            movement |= MOVEMENT_JUMP;
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_RIGHT) {
            movement &= ~MOVEMENT_JUMP;
        }
        break;
    }
}

int main(int argc, char* argv[])
{
    unsigned prev_ticks;

    SDL_Init(SDL_INIT_VIDEO);
    init(argc, argv);

    prev_ticks = SDL_GetTicks();

    while (running)
    {
        SDL_Event e;
        unsigned ticks;

        /* cap tick rate to sdl's maximum timer resolution */
        for (; prev_ticks == (ticks = SDL_GetTicks()); SDL_Delay(0));

        delta_time = (ticks - prev_ticks) * 0.001f;
        prev_ticks = ticks;

        SDL_GetRelativeMouseState(&wishlook[0], &wishlook[1]);
        for (; SDL_PollEvent(&e); handle(&e));

        tick();
    }

    return 0;
}
