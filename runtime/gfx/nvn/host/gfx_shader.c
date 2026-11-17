// Shaders of MKW: SPIR-V prepared before the game runs (tools/shaders/translate.py), one <key>.spv and <key>.res.json
// per program under <exe dir>/spirv, key = nvn2_program_key(control, code). A program is loaded once and kept for the
// life of the process in a memory cache by that key, which is also the shader's id in a pipeline key record
// (vk_pipeline.c). Vertex shaders that read integer vertex formats get a variant with integer inputs
// (int_input_variant), fragment shaders that write integer targets one with integer outputs (int_output_variant).
//
// Reflection: every shader that enters the cache goes through gfx_shader_reflect (gfx_shader_refl.c); the gfx.h
// accessors are at the end of the file.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "nvn_api.h"
#include "gfx_shader_internal.h"
#include "host.h"
#include "platform/platform.h"

#define BUCKETS 4096
#define PATH_CAP 1024                                  // bytes of a UTF-8 path


static PlatMutex cache_lock = PLAT_MUTEX_INIT;                  // memory cache
static GfxShader *buckets[BUCKETS];
static volatile int64_t n_keys;                            // memory cache entries

#define read_file plat_file_read

// ---- cache

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s->spec), free(s);
}


// Inserts s, or frees it and returns the entry another thread inserted first.
static GfxShader *mem_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

static void key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; gfx_shader_reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4) ||
                  !strncmp(st, "\"task\"", 6) || !strncmp(st, "\"mesh\"", 6));   // task/mesh: gfx_shader_reflect() mesh_kind
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

// ---- MKW: SPIR-V prepared before the game runs (tools/shaders/translate.py)
// <game dir>/spirv/<key>.spv and .res.json, key = nvn2_program_key(control, code): every program the game ships was
// translated offline, so play never translates. MKW_SPIRV names another directory.
static char prepared_dir[PATH_CAP];
_Static_assert(CFG_GAME_DIR_MAX + 256 <= PATH_CAP, "game directory must leave room for derived shader paths");

static void prepared_init(void *param) {
    (void)param;
    const char *e = cfg_get("MKW_SPIRV");
    if (e && *e) {
        snprintf(prepared_dir, sizeof prepared_dir, "%s", e);
    } else {
        snprintf(prepared_dir, sizeof prepared_dir, "%s/spirv", cfg_game_dir());
    }
    rt_log("gfx_shader: prepared SPIR-V from %s", prepared_dir);
}

static void prepared_paths(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, prepared_init, NULL);
}

static GfxShader *prepared_load(const uint8_t key[32], const uint8_t *control) {
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    uint64_t spv_size = 0, res_size = 0;
    snprintf(p, sizeof p, "%s/%s.res.json", prepared_dir, hex);
    uint8_t *res = read_file(p, &res_size);
    if (!res || !reflection_valid((const char *)res, res_size)) {
        free(res);
        return NULL;
    }
    snprintf(p, sizeof p, "%s/%s.spv", prepared_dir, hex);
    uint8_t *spv = read_file(p, &spv_size);
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    memcpy(s->iid, key, 32);   // pipeline key records name a prepared shader by its program key (gfx_shader_load_input)
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    if (control) {
        uint16_t st;
        memcpy(&st, control + 0x56, 2);   // NVNshaderStage (nvn2_program_stage): 5 compute
        if (st == 5) {
            s->cs_table[0] = control[0xb1], s->cs_table[1] = control[0xb2];
            s->cs_table[2] = control[0xb3], s->cs_table[3] = control[0xb6];
            s->cs_table[4] = control[0xa9];
            s->cs_known = 1;
        }
    }
    gfx_shader_reflect(s);
    return mem_insert(s);
}

// ---- integer vertex attributes and colour outputs (gfx_spirv_io.c)

// The variant of s with the Input (out 0) or Output (out 1) Locations in um / sm as uint / int vectors, or s.
static const GfxShader *int_io_variant(GfxShader *s, uint32_t um, uint32_t sm, int out) {
    if (!um && !sm) return s;
    uint8_t key[32];
    memcpy(key, s->key, 32);
    // variants: the program key with the masks folded into its last bytes, other marks for outputs
    key[30] ^= out ? 0x69 : 0x5a, key[31] ^= out ? 0x96 : 0xa5;
    for (int i = 0; i < 4; i++) key[22 + i] ^= (uint8_t)(um >> 8 * i), key[26 + i] ^= (uint8_t)(sm >> 8 * i);
    GfxShader *v = mem_find(key);
    if (v) return v;
    uint64_t nw = 0;
    uint32_t *code = spirv_int_io(s->spirv, s->words, um, sm, out ? 3 : 1, &nw);
    if (!code) {
        static volatile int32_t n_fail;
        if (plat_inc32(&n_fail) <= 8)
            rt_log("gfx_shader: integer %s %#x/%#x not patched in", out ? "colour outputs" : "vertex inputs", um, sm);
        return s;
    }
    if (!(v = calloc(1, sizeof *v)) || !(v->reflection = malloc(s->reflection_size + 1))) rt_fatal("gfx_shader: out of memory");
    memcpy(v->key, key, 32), memcpy(v->iid, s->iid, 32);
    v->spirv = code, v->words = nw;
    memcpy(v->reflection, s->reflection, s->reflection_size), v->reflection[s->reflection_size] = 0;
    v->reflection_size = s->reflection_size;
    gfx_shader_reflect(v);
    for (int l = 0; l < 32; l++)
        if ((um | sm) >> l & 1) (out ? v->out : v->in)[l].type = um >> l & 1 ? GFX_TYPE_UINT : GFX_TYPE_SINT;
    const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");   // the variants for spirv-val
    if (dd && *dd) {
        char path[PATH_CAP + 96], hex[65];
        key_hex(key, hex);
        plat_file_mkdir_p(dd);
        snprintf(path, sizeof path, "%s/%s.spv", dd, hex);
        plat_file_write(path, code, nw * 4);
    }
    static volatile int32_t n_var[2];   // apart: the vertex ones would use up the fragment ones' lines
    if (plat_inc32(&n_var[out]) <= 8)
        rt_log("gfx_shader: %s uint %#x sint %#x", out ? "fragment shader variant with integer outputs"
                                                       : "vertex shader variant with integer inputs", um, sm);
    return mem_insert(v);
}

// The variant of a prepared vertex shader for the integer attribute formats in a (GfxShaderAnswers.attr_type), or s.
static const GfxShader *int_input_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_VS || s->mesh_kind != GFX_MESH_NONE) return s;
    uint32_t um = 0, sm = 0;
    for (int l = 0; l < 16; l++)
        if (s->in_mask >> l & 1) {
            if (a->attr_type[l] == GFX_TYPE_UINT) um |= 1u << l;
            else if (a->attr_type[l] == GFX_TYPE_SINT) sm |= 1u << l;
        }
    return int_io_variant(s, um, sm, 0);
}

// The variant of a prepared fragment shader for the integer colour targets in a (GfxShaderAnswers.out_type), or s:
// only the outputs the shader writes (the others are masked off, vk_pipeline.c target_wmask).
static const GfxShader *int_output_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_FS) return s;
    uint32_t um = 0, sm = 0;
    for (int l = 0; l < 8; l++)
        if (s->out_mask >> l & 1) {
            if (a->out_type[l] == GFX_TYPE_UINT) um |= 1u << l;
            else if (a->out_type[l] == GFX_TYPE_SINT) sm |= 1u << l;
        }
    return int_io_variant(s, um, sm, 1);
}

static const GfxShader *int_image_variant(GfxShader *s, const GfxShaderAnswers *a);

// The integer variant a draw or dispatch's answers ask of s (none for the other stages).
static const GfxShader *int_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s) return s;
    return s->stage == GFX_STAGE_CS ? int_image_variant(s, a)
         : s->stage == GFX_STAGE_FS ? int_output_variant(s, a)
                                    : int_input_variant(s, a);
}

// The prepared shader of a program, or NULL (logged once per program) when the offline translator could not do it.
static const GfxShader *prepared_get(const uint8_t *control, const uint8_t *code) {
    prepared_paths();
    uint8_t key[32];
    nvn2_program_key(control, code, key);
    GfxShader *s = mem_find(key);
    if (s) return s;
    s = prepared_load(key, control);
    if (!s) {
        char hex[65];
        key_hex(key, hex);
        static volatile int32_t n_missing;
        int32_t n = plat_inc32(&n_missing);
        if (n <= 20 || !(n & (n - 1))) rt_log("gfx_shader: no prepared SPIR-V %s (%d lookups so far)", hex, n);
        // MKW_SPIRV_DUMP=<dir>: the missing program's control section and code, to find where it comes from.
        const char *dump = cfg_get("MKW_SPIRV_DUMP");
        if (dump && *dump) {   // every missing program once (the file exists after the first time)
            char p[PATH_CAP + 80];
            uint32_t ctl_size = 0xd8, size;
            memcpy(&size, control + 0x44, 4);
            plat_file_mkdir_p(dump);
            snprintf(p, sizeof p, "%s/%s.ctl", dump, hex);
            if (plat_file_exists(p)) return s;
            plat_file_write(p, control, ctl_size);
            snprintf(p, sizeof p, "%s/%s.code", dump, hex);
            plat_file_write(p, code, size < (16u << 20) ? size : 0);
        }
    }
    return s;
}

// ---- public

// The prepared shader of a program, as its integer-input variant when a vertex shader's attributes in a have integer
// formats; NULL when the program has no prepared SPIR-V (logged).
// The variant of a prepared compute shader for the integer-format views in a (GfxShaderAnswers.img_uint/img_sint), or
// s (gfx_spirv_img.c).
static const GfxShader *int_image_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_CS || !(a->img_uint | a->img_sint)) return s;
    uint8_t key[32];
    memcpy(key, s->key, 32);
    key[30] ^= 0x3c, key[31] ^= 0xc3;   // as int_input_variant's, with other marks
    for (int i = 0; i < 8; i++) key[i + 14] ^= (uint8_t)(a->img_uint >> 8 * i), key[i + 6] ^= (uint8_t)(a->img_sint >> 8 * i);
    GfxShader *v = mem_find(key);
    if (v) return v;
    uint64_t nw = 0;
    const char *why = NULL;
    uint32_t *code = spirv_int_images(s->spirv, s->words, a->img_uint, a->img_sint, &nw, &why);
    if (!code) {
        static volatile int32_t n_fail;
        if (plat_inc32(&n_fail) <= 8) {
            char hex[65];
            key_hex(s->key, hex);
            rt_log("gfx_shader: integer images %#llx/%#llx not patched in program %s: %s; using original shader",
                   (unsigned long long)a->img_uint, (unsigned long long)a->img_sint, hex, why ? why : "unknown reason");
            // Keep the failing input too: successful variants alone cannot reproduce a failed conversion offline.
            const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");
            if (dd && *dd) {
                char path[PATH_CAP + 128];
                plat_file_mkdir_p(dd);
                snprintf(path, sizeof path, "%s/%s-u%llx-s%llx.failed.spv", dd, hex,
                         (unsigned long long)a->img_uint, (unsigned long long)a->img_sint);
                plat_file_write(path, s->spirv, s->words * 4);
            }
        }
        return s;
    }
    if (!(v = calloc(1, sizeof *v)) || !(v->reflection = malloc(s->reflection_size + 1))) rt_fatal("gfx_shader: out of memory");
    memcpy(v->key, key, 32), memcpy(v->iid, s->iid, 32);
    v->spirv = code, v->words = nw;
    memcpy(v->reflection, s->reflection, s->reflection_size), v->reflection[s->reflection_size] = 0;
    v->reflection_size = s->reflection_size;
    memcpy(v->cs_table, s->cs_table, sizeof v->cs_table), v->cs_known = s->cs_known;   // reflect places the resources by it
    gfx_shader_reflect(v);
    const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");   // the variants for spirv-val
    if (dd && *dd) {
        char path[PATH_CAP + 96], hex[65];
        key_hex(key, hex);
        plat_file_mkdir_p(dd);
        snprintf(path, sizeof path, "%s/%s.spv", dd, hex);
        plat_file_write(path, code, nw * 4);
    }
    static volatile int32_t n_var;
    if (plat_inc32(&n_var) <= 8)
        rt_log("gfx_shader: compute shader variant with integer images uint %#llx sint %#llx", (unsigned long long)a->img_uint,
               (unsigned long long)a->img_sint);
    return mem_insert(v);
}

const GfxShader *gfx_shader_get(const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a) {
    GfxShader *s = (GfxShader *)prepared_get(control, code);
    return int_variant(s, a);
}

uint64_t gfx_shader_count(void) { return (uint64_t)n_keys; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

// By program key (a pipeline key record's shader id): memory, then the prepared directory. NULL when neither has it.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32]) {
    prepared_paths();
    GfxShader *s = mem_find(iid);
    return s ? s : prepared_load(iid, NULL);
}

// The shaders named by pipeline key records, loaded (out[i] NULL when one is missing). Off from run49 to 06/11/2026: the
// records' integer-input vertex shaders were built from the base program (vk_pipe_keys.c), and the key file was never
// read after the TOTK magic rename (05/11/2026).
// read on every core: one by one, 729 shaders took 1 056 ms the guest waited for (pw2, 06/11/2026)
static struct { const uint8_t (*iids)[32]; const GfxShader **out; size_t n; volatile int64_t next, miss; } PW;

static void prewarm_worker(void *arg) {
    plat_thread_background();   // off the frame's fast cores (MKW_FAST_CORES)
    (void)arg;
    for (int64_t i; (i = plat_inc64(&PW.next) - 1) < (int64_t)PW.n;)
        if (!(PW.out[i] = gfx_shader_load_input(PW.iids[i]))) plat_inc64(&PW.miss);
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out) {
    PW.iids = iids, PW.out = out, PW.n = n, PW.next = 0, PW.miss = 0;
    PlatThread *t[64];
    int nt = 0, cpus = (int)plat_cpu_count();
    for (int i = 0; i < cpus && i < 64 && n > 16; i++)
        if ((t[nt] = plat_thread_new(prewarm_worker, NULL, "shader prewarm"))) nt++;
    prewarm_worker(NULL);
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(t[i], 1000000000u)) {}
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys, %lld not found, on %d threads", n,
           (long long)PW.miss, nt + 1);
}

const GfxShader *gfx_shader_int_variant(const GfxShader *s, const GfxShaderAnswers *a) {
    return int_variant((GfxShader *)s, a);
}

const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words) {
    if (words) *words = s->words;
    return s->spirv;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }
int gfx_shader_first_store(const GfxShader *s) { return s->first_store; }

uint32_t gfx_shader_int_inputs(const GfxShader *s) { return s->int_inputs; }

uint32_t gfx_shader_raw_outputs(const GfxShader *s) { return s->raw_outputs; }

// A copy of s whose specialization constants default to val[k] for each word k with fix[k] (SCU<k> true, SCV<k> =
// val[k]), the same as spirv-opt --set-spec-const-default-value: the pipeline code sees an ordinary shader, so the
// pipeline cache, the GPL libraries and the background builds treat a variant like any other. The copy lives for
// the process and shares s's reflection; its key and input id are sha256(s's key, the fixed words). NULL when the
// SPIR-V has none of the constants.
const GfxShader *gfx_shader_variant(const GfxShader *s, const uint32_t *val, const uint8_t *fix) {
    enum { OP_SPEC_TRUE = 48, OP_SPEC_FALSE = 49, OP_SPEC = 50, OP_DECORATE = 71, DEC_SPEC_ID = 1 };
    if (s->words < 5) return NULL;
    uint32_t bound = s->spirv[3], *w = malloc(sizeof *w * s->words);
    int32_t *sid = malloc(sizeof *sid * (bound ? bound : 1));
    if (!w || !sid) rt_fatal("gfx_shader: out of memory");
    memcpy(w, s->spirv, sizeof *w * s->words);
    memset(sid, 0xff, sizeof *sid * bound);
    for (uint64_t i = 5; i < s->words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > s->words) break;
        if (op == OP_DECORATE && len >= 4 && w[i + 2] == DEC_SPEC_ID && w[i + 1] < bound) sid[w[i + 1]] = (int32_t)w[i + 3];
        i += len;
    }
    int patched = 0;
    for (uint64_t i = 5; i < s->words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > s->words) break;
        if ((op == OP_SPEC_TRUE || op == OP_SPEC_FALSE || op == OP_SPEC) && len >= 3 && w[i + 2] < bound) {
            int32_t id = sid[w[i + 2]];
            int k = id >> 1;
            if (id >= 0 && k < s->nspec && fix[k]) {
                if (!(id & 1) && op != OP_SPEC) w[i] = (w[i] & 0xffff0000u) | OP_SPEC_TRUE, patched++;
                else if ((id & 1) && op == OP_SPEC && len == 4) w[i + 3] = val[k], patched++;
            }
        }
        i += len;
    }
    free(sid);
    if (!patched) {
        free(w);
        return NULL;
    }
    GfxShader *v = malloc(sizeof *v);
    if (!v) rt_fatal("gfx_shader: out of memory");
    *v = *s;
    v->next = NULL, v->spirv = w, v->variant_of = s;
    size_t n = 32 + (size_t)s->nspec * 5;
    uint8_t *buf = malloc(n);
    if (!buf) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, s->key, 32);
    for (int k = 0; k < s->nspec; k++) buf[32 + 5 * k] = fix[k], memcpy(buf + 33 + 5 * k, fix[k] ? &val[k] : &(uint32_t){0}, 4);
    rt_sha256(buf, n, v->key);
    memcpy(v->iid, v->key, 32);
    free(buf);
    return v;
}

const GfxShader *gfx_shader_variant_of(const GfxShader *s) { return s->variant_of; }

int gfx_shader_spec(const GfxShader *s, const uint32_t **words) {
    *words = s->spec;
    return s->nspec;
}

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

int gfx_shader_mesh_kind(const GfxShader *s) { return s ? s->mesh_kind : GFX_MESH_NONE; }

int gfx_shader_cs_of_mesh(const GfxShader *s) { return s && s->cs_of_mesh; }

// The vertex shader of task/mesh as compute: a prepared shader under a fixed key that no program has
// (tools/shaders/translate.py write_mesh_cs_vs, sm86 head.py MESH_CS_VS_KEY), so pipeline key records name it too.
static const uint8_t mesh_cs_vs_key[32] = "mkw-mesh-cs-vs-1mkw-mesh-cs-vs-1";

const GfxShader *gfx_shader_mesh_cs_vs(void) {
    static const GfxShader *vs;
    static int tried;
    if (!tried) {
        vs = gfx_shader_load_input(mesh_cs_vs_key), tried = 1;
        if (!vs) rt_log("gfx_shader: no vertex shader for task/mesh as compute (spirv/%s.spv)", "6d6b772d...");
    }
    return vs;
}

int gfx_shader_is_mesh_cs_vs(const GfxShader *s) { return s && !memcmp(s->key, mesh_cs_vs_key, 32); }

// The compute form of a task or mesh program, <key>.cs.spv beside its .spv, with the
// same reflection: stage CS, the task/mesh kind kept for its binding tags. Loaded once and kept with the program (not
// in the cache by key, where the program itself is). NULL for any other program or when the file is missing.
const GfxShader *gfx_shader_cs_variant(const GfxShader *s) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    if (!s || s->mesh_kind == GFX_MESH_NONE || s->cs_of_mesh) return NULL;
    GfxShader *m = (GfxShader *)s;
    plat_mutex_lock(&lock);
    if (!m->cs_variant) {
        char hex[65], p[PATH_CAP + 80];
        key_hex(s->key, hex);
        snprintf(p, sizeof p, "%s/%s.cs.spv", prepared_dir, hex);
        uint64_t n = 0;
        uint8_t *spv = read_file(p, &n);
        uint32_t magic = 0;
        if (spv && n >= 20) memcpy(&magic, spv, 4);
        char *res = malloc(s->reflection_size + 1);
        if (spv && n % 4 == 0 && magic == 0x07230203 && spirv_complete((const uint32_t *)spv, n / 4) && res) {
            GfxShader *v = calloc(1, sizeof *v);
            memcpy(v->key, s->key, 32), memcpy(v->iid, s->iid, 32);
            v->spirv = (uint32_t *)spv, v->words = n / 4;
            memcpy(res, s->reflection, s->reflection_size), res[s->reflection_size] = 0;
            v->reflection = res, v->reflection_size = s->reflection_size;
            gfx_shader_reflect(v);
            v->stage = GFX_STAGE_CS, v->cs_of_mesh = 1;
            m->cs_variant = v;
        } else {
            free(spv), free(res);
            rt_log("gfx_shader: no compute form of %s program %s (%s)", s->mesh_kind == GFX_MESH_TASK ? "task" : "mesh", hex, p);
        }
    }
    plat_mutex_unlock(&lock);
    return m->cs_variant;
}
const uint8_t *gfx_shader_cs_table(const GfxShader *s) { return s && s->cs_known ? s->cs_table : NULL; }
