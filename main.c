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
 * I might or might not add more features in the future, for now it just
 * renders the map's meshes with vertex lighting. I definitely want to
 * try and implement quake-like physics and collisions
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
 * controls are WASD, mouse, numpad +/-
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
 */

#include <SDL2/SDL.h>

#define degrees(rad) ((rad) * (180.0f / M_PI))
#define radians(deg) ((deg) * (M_PI / 180.0f))
#define dot3(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
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

    aspect = (float)gl_width / gl_height;
    gluPerspective(horizontal_fov / aspect, aspect, near, far);
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
    int front;
    int back;
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
            index = node->front;
        } else {
            index = node->back;
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

int tessellation_level;
float camera_pos[3];
float camera_angle[2]; /* yaw, pitch */
float wishdir[3];
int wishlook[2];

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

void update()
{
    float velocity[3];
    float pitch_sin, pitch_cos, yaw_sin, yaw_cos;
    float pitch_x;

    update_fps();

    SDL_GetRelativeMouseState(&wishlook[0], &wishlook[1]);
    camera_angle[0] += 0.002f * wishlook[0];
    camera_angle[1] += 0.002f * wishlook[1];
    clamp_angles(camera_angle, 2);

    pitch_sin = SDL_sinf(2*M_PI - camera_angle[1]);
    pitch_cos = SDL_cosf(2*M_PI - camera_angle[1]);
    yaw_sin = SDL_sinf(2*M_PI - camera_angle[0]);
    yaw_cos = SDL_cosf(2*M_PI - camera_angle[0]);

    /* this applies 2 rotations, pitch first then yaw */
    pitch_x = wishdir[0] * pitch_cos + wishdir[2] * (-pitch_sin);
    velocity[0] = pitch_x * yaw_cos + wishdir[1] * (-yaw_sin);
    velocity[1] = pitch_x * yaw_sin + wishdir[1] * yaw_cos;
    velocity[2] = wishdir[0] * pitch_sin + wishdir[2] * pitch_cos;

    nrm3(velocity);
    mul3_scalar(velocity, 500 * delta_time);
    add3(camera_pos, velocity);
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
    glTranslatef(-camera_pos[0], -camera_pos[1], -camera_pos[2]);

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
        switch (e->key.keysym.sym)
        {
        case SDLK_ESCAPE:
            running = 0;
            break;
        case SDLK_w:
            wishdir[0] = 1;
            break;
        case SDLK_s:
            wishdir[0] = -1;
            break;
        case SDLK_a:
            wishdir[1] = 1;
            break;
        case SDLK_d:
            wishdir[1] = -1;
            break;
        case SDLK_KP_PLUS:
            wishdir[2] = 1;
            break;
        case SDLK_KP_MINUS:
            wishdir[2] = -1;
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
        case SDLK_KP_PLUS:
            wishdir[2] = 0;
            break;
        case SDLK_KP_MINUS:
            wishdir[2] = 0;
            break;
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

        for (; SDL_PollEvent(&e); handle(&e));
        tick();
    }

    return 0;
}