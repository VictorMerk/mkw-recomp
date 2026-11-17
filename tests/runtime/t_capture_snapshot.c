// Exercise the production capture snapshot/delta path with two synthetic guest pages. No game or GPU.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include "cap_mem.c"

struct CapOut {
    unsigned count;
    struct { uint32_t kind; uint64_t address, length; unsigned char bytes[4096]; } records[16];
};
static unsigned char *fake_pages;
static RtGuestLayout fake_layout;
static RtGameModule fake_module;
static RtGame fake_game;
static const uint64_t granule = 2ull << 20;
static int fake_trap;

const RtGuestLayout *mem_layout(void) { return &fake_layout; }
const RtGame *rt_game(void) { return &fake_game; }
void mem_hold_layout(void) {}
void mem_release_layout(void) {}
int mem_trapped(uint64_t address) { return fake_trap && address >= (uintptr_t)fake_pages && address < (uintptr_t)fake_pages + 4096; }
void mem_untrap(uint64_t address, uint64_t length) { (void)address; (void)length; }
int plat_mem_commit(uint64_t address, size_t length) { (void)address; (void)length; return 1; }
int plat_mem_protect(uint64_t address, uint64_t length, int prot) { (void)address; (void)length; (void)prot; return 1; }
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    (void)name; fn(arg); return (PlatThread *)(uintptr_t)1;
}
void plat_thread_join(PlatThread *thread) { (void)thread; }
int plat_mem_state(uint64_t address, uint64_t limit, uint64_t *end) {
    for (unsigned i = 0; i < 2; i++) {
        uint64_t page = (uintptr_t)fake_pages + i * granule;
        if (address >= page && address < page + 4096) { *end = page + 4096 < limit ? page + 4096 : limit; return i == 0 && fake_trap ? PLAT_PROT_NONE : PLAT_PROT_RW; }
        if (address < page) { *end = page < limit ? page : limit; return PLAT_MEM_RESERVED; }
    }
    *end = limit; return PLAT_MEM_RESERVED;
}
uint64_t plat_mem_populated(uint64_t address, uint64_t size, uint64_t (*runs)[2], uint64_t max) {
    uint64_t count = 0;
    for (unsigned i = 0; i < 2 && count < max; i++) {
        uint64_t page = (uintptr_t)fake_pages + i * granule;
        uint64_t lo = address > page ? address : page, hi = address + size < page + 4096 ? address + size : page + 4096;
        if (lo < hi) { runs[count][0] = lo; runs[count++][1] = hi; }
    }
    return count;
}
void cap_out(CapOut *out, uint32_t type, const void *a, size_t na, const void *b, size_t nb) {
    if ((type != CAP_MEM && type != CAP_ZERO) || na != 16 || out->count == 16) abort();
    const uint64_t *range = a;
    if (range[1] > 4096 || (type == CAP_MEM && nb != range[1])) abort();
    unsigned index = out->count++;
    out->records[index].kind = type;
    out->records[index].address = range[0]; out->records[index].length = range[1];
    memset(out->records[index].bytes, 0, 4096);
    if (nb) memcpy(out->records[index].bytes, b, nb);
}
static int restores_both_pages(const CapOut *out, unsigned char heap_value) {
    unsigned char values[2] = {0, 0};
    for (unsigned k = 0; k < out->count; k++) {
        for (unsigned i = 0; i < 2; i++) {
            uint64_t page = (uintptr_t)fake_pages + i * granule;
            if (out->records[k].address == page) values[i] = out->records[k].bytes[0];
        }
    }
    return values[0] == 'A' && values[1] == heap_value;
}
int main(void) {
#ifdef _WIN32
    fake_pages = _aligned_malloc((size_t)(2 * granule), (size_t)granule);
#else
    fake_pages = aligned_alloc((size_t)granule, (size_t)(2 * granule));
#endif
    if (!fake_pages) return 2;
    memset(fake_pages, 0, (size_t)(2 * granule));
    fake_pages[0] = 'A'; fake_pages[granule] = 'B';
    fake_module = (RtGameModule){.base = (uintptr_t)fake_pages, .size = 4096};
    fake_game = (RtGame){.modules = &fake_module, .nmodules = 1};
    fake_layout.heap = (RtRange){(uintptr_t)fake_pages + granule, granule};
    CapOut first = {0}, unchanged = {0}, second = {0}, delta_zero = {0}, zeroed = {0};
    if (!cap_mem_snapshot(&first) || !restores_both_pages(&first, 'B')) return 3;
    if (cap_mem_delta(&unchanged) || unchanged.count) return 4;
    fake_trap = 1;   // a fresh capture must resample trapped pages as its first snapshot did
    fake_pages[granule] = 'C';
    if (!cap_mem_snapshot(&second) || !restores_both_pages(&second, 'C')) {
        fprintf(stderr, "second independent snapshot lost an unchanged module page (%u records)\n", second.count); return 1;
    }
    memset(fake_pages + granule, 0, 4096);
    if (cap_mem_delta(&delta_zero) != 1 || delta_zero.count != 1 ||
        delta_zero.records[0].kind != CAP_ZERO || delta_zero.records[0].address != (uintptr_t)fake_pages + granule) return 6;
    if (!cap_mem_snapshot(&zeroed) || !restores_both_pages(&zeroed, 0)) return 5;
    puts("independent snapshots preserve unchanged, changed and zero pages; unchanged deltas stay empty");
#ifdef _WIN32
    _aligned_free(fake_pages);
#else
    free(fake_pages);
#endif
    return 0;
}
