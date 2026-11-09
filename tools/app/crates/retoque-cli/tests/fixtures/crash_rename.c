// Test-child-only fault injection: stop immediately before/after the actual rename syscall.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int selected(const char *dest) {
    const char *suffix = getenv("RETOQUE_CRASH_SUFFIX");
    if (!suffix) return 0;
    size_t n = strlen(dest), s = strlen(suffix);
    if (n < s || strcmp(dest + n - s, suffix)) return 0;
    static int calls = 0;
    const char *nth = getenv("RETOQUE_CRASH_NTH");
    return ++calls == (nth ? atoi(nth) : 1);
}
static void stop(int match, int after) {
    const char *phase = getenv("RETOQUE_CRASH_PHASE");
    if (match && phase && ((!strcmp(phase,"after")) == after)) _exit(86);
}
int rename(const char *old, const char *dest) {
    int (*real)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
    int match = selected(dest); stop(match, 0);
    int result = real(old, dest);
    if (!result) stop(match, 1);
    return result;
}
int renameat(int oldfd, const char *old, int newfd, const char *dest) {
    int (*real)(int, const char *, int, const char *) = dlsym(RTLD_NEXT, "renameat");
    int match = selected(dest); stop(match, 0);
    int result = real(oldfd, old, newfd, dest);
    if (!result) stop(match, 1);
    return result;
}
int renameat2(int oldfd, const char *old, int newfd, const char *dest, unsigned int flags) {
    int (*real)(int, const char *, int, const char *, unsigned int) = dlsym(RTLD_NEXT, "renameat2");
    int match = selected(dest); stop(match, 0);
    int result = real(oldfd, old, newfd, dest, flags);
    if (!result) stop(match, 1);
    return result;
}
