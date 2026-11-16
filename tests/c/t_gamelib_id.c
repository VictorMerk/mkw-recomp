// Exercise the actual library guard with synthetic tables and sidecars; no game code or dynamic library is loaded.
#include <assert.h>
#include "../../runtime/core/boot/gamelib.c"

static const char *sidecar;
const char *cfg_game_dir(void) { return "/synthetic"; }
const RtGame *rt_game_builtin(void) { return NULL; }
int plat_file_exists(const char *path) { (void)path; return 1; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    (void)path;
    *size = strlen(sidecar);
    uint8_t *copy = malloc(*size);
    memcpy(copy, sidecar, *size);
    return copy;
}
void *plat_lib_open(const char *path) { (void)path; return NULL; }
void *plat_lib_sym(void *lib, const char *name) { (void)lib; (void)name; return NULL; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

int main(void) {
    RtGame g = {.abi = RT_GAME_ABI, .id = RT_GAME_ID, .nmodules = 1};
    assert(!check(&g));
    g.id = RT_GAME_ID_LEGACY_CRLF;
    assert(!check(&g));
    g.abi++;
    assert(check(&g));
    g.abi = RT_GAME_ABI;
    g.id = NULL;
    assert(check(&g));
    g.id = "unrelated";
    assert(check(&g));
    g.id = RT_GAME_ID "extra";
    assert(check(&g));
    g.id = RT_GAME_ID_LEGACY_CRLF;
    g.nmodules = 0;
    assert(check(&g));
    g.nmodules = RT_MAX_MODULES + 1;
    assert(check(&g));
    g.nmodules = 1;
    g.npatch_sets = RT_MAX_PATCH_SETS + 1;
    assert(check(&g));
    const char *valid[] = {RT_GAME_ID, RT_GAME_ID "\n", RT_GAME_ID_LEGACY_CRLF,
                           RT_GAME_ID_LEGACY_CRLF "\r\n"};
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++) {
        sidecar = valid[i];
        assert(rt_game_present());
    }
    const char *invalid[] = {"", "unrelated", RT_GAME_ID "extra", RT_GAME_ID_LEGACY_CRLF "extra"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        sidecar = invalid[i];
        assert(!rt_game_present());
    }
    return 0;
}
