// SPDX-License-Identifier: GPL-2.0-or-later
// iwasm supplies WASI. These are fastfetch's PISCO imports for its one-file image.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wasm_export.h"

static int elf;
static off_t elf_size;

int init_native_lib(void) {
    const char *path = getenv("PISCO_TEST_ELF");
    struct stat st;
    if (!path || (elf = open(path, O_RDONLY)) < 0) return -1;
    if (fstat(elf, &st) != 0) { close(elf); return -1; }
    elf_size = st.st_size;
    return 0;
}

void deinit_native_lib(void) { close(elf); }

static int named(const char *p, uint32_t len, const char *name) {
    return strlen(name) == len && memcmp(p, name, len) == 0;
}

static int is_elf(const char *p, uint32_t len) {
    return named(p, len, "fastfetch") || named(p, len, "c:/usr/bin/fastfetch");
}

static const char *directory(const char *p, uint32_t len) {
    if (named(p, len, "c:/")) return "usr/\n";
    if (named(p, len, "c:/usr")) return "bin/\n";
    if (named(p, len, "c:/usr/bin")) return "fastfetch\n";
    return NULL;
}

static int64_t file_stat(wasm_exec_env_t env, const char *p, uint32_t len) {
    if (is_elf(p, len)) return elf_size;
    return directory(p, len) ? -2 : -1;
}

static uint32_t file_list(wasm_exec_env_t env, const char *p, uint32_t len, char *dst, uint32_t cap) {
    const char *entries = directory(p, len);
    if (!entries) return 0;
    uint32_t n = strlen(entries);
    if (cap >= n) memcpy(dst, entries, n);
    return n;
}

static uint32_t read_elf(wasm_exec_env_t env, const char *p, uint32_t len,
                         uint64_t off, uint64_t n, uint64_t dst) {
    if (!is_elf(p, len) || off > (uint64_t)elf_size || n > UINT32_MAX) return 0;
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_app_addr(instance, dst, n)) return 0;
    ssize_t got = pread(elf, wasm_runtime_addr_app_to_native(instance, dst), n, (off_t)off);
    return got < 0 ? 0 : (uint32_t)got;
}

// a Linux drive's links and executable bits (rt/linux-x64/fs.c): this drive has no links, and its one program runs
static int32_t file_link(wasm_exec_env_t env, const char *p, uint32_t len, char *dst, uint32_t cap) { return -1; }
static int32_t file_exec(wasm_exec_env_t env, const char *p, uint32_t len) { return is_elf(p, len) ? 1 : -1; }

static void log_msg(wasm_exec_env_t env, const char *msg, uint32_t len) {
    fwrite(msg, 1, len, stderr);
    fputc('\n', stderr);
}

static void fault(wasm_exec_env_t env, uint32_t kind, uint64_t addr) {
    fprintf(stderr, "guest fault %u at 0x%llx\n", kind, (unsigned long long)addr);
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "PISCO guest fault");
}

static void title_exit(wasm_exec_env_t env, uint32_t status) { exit(status); }

static NativeSymbol imports[] = {
    {"stat", file_stat, "(*~)I", NULL}, {"list", file_list, "(*~*~)i", NULL},
    {"link", file_link, "(*~*~)i", NULL}, {"exec", file_exec, "(*~)i", NULL},
    {"read_file", read_elf, "(*~III)i", NULL}, {"log", log_msg, "(*~)", NULL},
    {"fault", fault, "(iI)", NULL}, {"exit", title_exit, "(i)", NULL},
};

uint32_t get_native_lib(char **module, NativeSymbol **symbols) {
    *module = "host";
    *symbols = imports;
    return sizeof(imports) / sizeof(imports[0]);
}
