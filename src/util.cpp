#include "util.h"
#include "common.h"
#include "dobby.h"
#include <algorithm>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <mutex>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool safe_read(const void* addr, void* out, size_t len) {
    static std::mutex m;
    static int p[2] = {-1, -1};
    if (len == 0 || len > 4096 || !addr) return false;
    std::lock_guard<std::mutex> g(m);
    if (p[0] < 0) {
        if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) { p[0] = p[1] = -1; return false; }
    }
    ssize_t w = write(p[1], addr, len);
    if (w <= 0) return false;
    ssize_t r = read(p[0], out, (size_t)w);   // всегда вычищаем pipe
    return w == (ssize_t)len && r == w;
}

// ---------------------------------------------------------------------------------------
static bool find_module(const char* base, std::string* path, uintptr_t* start) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return false;
    char line[1280];
    bool ok = false;
    size_t blen = strlen(base);
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off;
        char perms[8];
        int n = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %n", &s, &e, perms, &off, &n) < 4 || n <= 0) continue;
        std::string p = trim(line + n);
        if (p.size() <= blen || p[0] != '/') continue;
        if (p.compare(p.size() - blen, blen, base) != 0) continue;
        if (p[p.size() - blen - 1] != '/') continue;
        if (off != 0) continue;                 // первый (низший) сегмент образа
        *path = p; *start = s; ok = true; break;
    }
    fclose(f);
    return ok;
}

static void* elf_lookup(const std::string& path, uintptr_t start, const char* name, bool prefix, size_t* out_size) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) { close(fd); return nullptr; }
    size_t sz = (size_t)st.st_size;
    void* m = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return nullptr;
    const uint8_t* b = (const uint8_t*)m;
    void* result = nullptr;
    auto eh = (const Elf64_Ehdr*)b;
    do {
        if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64) break;
        if (eh->e_phoff == 0 || eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > sz) break;
        uint64_t minv = UINT64_MAX;
        for (int i = 0; i < eh->e_phnum; i++) {
            auto ph = (const Elf64_Phdr*)(b + eh->e_phoff + (size_t)i * eh->e_phentsize);
            if (ph->p_type == PT_LOAD) minv = std::min<uint64_t>(minv, ph->p_vaddr & ~0xfffULL);
        }
        if (minv == UINT64_MAX) break;
        uintptr_t bias = start - (uintptr_t)minv;
        if (eh->e_shoff == 0 || eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > sz) break;
        size_t nlen = strlen(name);
        for (int i = 0; i < eh->e_shnum && !result; i++) {
            auto sh = (const Elf64_Shdr*)(b + eh->e_shoff + (size_t)i * eh->e_shentsize);
            if (sh->sh_type != SHT_DYNSYM && sh->sh_type != SHT_SYMTAB) continue;
            if (sh->sh_link >= eh->e_shnum) continue;
            auto str = (const Elf64_Shdr*)(b + eh->e_shoff + (size_t)sh->sh_link * eh->e_shentsize);
            if (sh->sh_offset + sh->sh_size > sz || str->sh_offset + str->sh_size > sz) continue;
            const char* strtab = (const char*)(b + str->sh_offset);
            auto syms = (const Elf64_Sym*)(b + sh->sh_offset);
            size_t cnt = sh->sh_size / sizeof(Elf64_Sym);
            for (size_t k = 0; k < cnt; k++) {
                const Elf64_Sym& s = syms[k];
                if (s.st_shndx == SHN_UNDEF || s.st_value == 0 || s.st_name >= str->sh_size) continue;
                if (ELF64_ST_TYPE(s.st_info) != STT_FUNC) continue;
                const char* nm = strtab + s.st_name;
                bool hit = prefix ? strncmp(nm, name, nlen) == 0 : strcmp(nm, name) == 0;
                if (hit) { result = (void*)(bias + s.st_value); if (out_size) *out_size = (size_t)s.st_size; break; }
            }
        }
    } while (0);
    munmap(m, sz);
    return result;
}

void* resolve_symbol_ex(const char* lib, const char* name, bool prefix, size_t* size) {
    if (size) *size = 0;
    std::string path;
    uintptr_t start = 0;
    bool have_elf = find_module(lib, &path, &start);
    if (!prefix) {
        void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            void* p = dlsym(h, name);
            dlclose(h);
            if (p) {
                // адрес берём у линкера, а размер — из ELF (если тот же символ)
                if (size && have_elf) {
                    size_t sz = 0;
                    void* q = elf_lookup(path, start, name, false, &sz);
                    if (q == p) *size = sz;
                }
                return p;
            }
        }
    }
    if (!have_elf) return nullptr;
    return elf_lookup(path, start, name, prefix, size);
}

void* resolve_symbol(const char* lib, const char* name, bool prefix) {
    return resolve_symbol_ex(lib, name, prefix, nullptr);
}

// ---------------------------------------------------------------------------------------
// Dobby (e9fe7fb) на arm64 пишет в начало функции 12 байт (adrp+add+br x17) или 16 (ldr+br+адрес),
// не проверяет размер функции, не останавливает другие потоки и не проверяет результат mprotect.
// Поэтому: (1) не трогаем функции короче 16 байт, (2) не пускаем перекрывающиеся патчи,
// (3) пишем в лог, что именно патчим.
static constexpr size_t kPatchBytes = 16;
static std::mutex g_hook_m;
static std::vector<std::pair<uintptr_t, uintptr_t>> g_patched;

bool inline_hook(const char* what, void* target, size_t target_size, bool require_size,
                 void* replace, void** orig) {
    if (!target) return false;
    if (target_size == 0 && require_size) {
        LOGW("hook %s skipped: function size unknown (%p)", what, target);
        return false;
    }
    if (target_size != 0 && target_size < kPatchBytes) {
        LOGW("hook %s skipped: function is only %zu bytes at %p, patch needs %zu and would overwrite neighbours",
             what, target_size, target, kPatchBytes);
        return false;
    }
    const uintptr_t a = (uintptr_t)target, e = a + kPatchBytes;
    {
        std::lock_guard<std::mutex> g(g_hook_m);
        for (auto& r : g_patched) {
            if (a < r.second && r.first < e) {
                if (a == r.first) { LOGW("hook %s: %p is already hooked", what, target); return true; }
                LOGW("hook %s skipped: patch overlaps an earlier hook at %p", what, (void*)r.first);
                return false;
            }
        }
    }
    uint32_t w[4] = {};
    if (g_cfg.debug) safe_read(target, w, sizeof w);
    LOGD("hook %s: target=%p size=%zu bytes=%08x %08x %08x %08x", what, target, target_size, w[0], w[1], w[2], w[3]);
    int r = DobbyHook(target, (dobby_dummy_func_t)replace, (dobby_dummy_func_t*)orig);
    if (r != 0 || (orig && !*orig)) {
        LOGE("hook %s failed (r=%d)", what, r);
        return false;
    }
    {
        std::lock_guard<std::mutex> g(g_hook_m);
        g_patched.emplace_back(a, e);
    }
    return true;
}
