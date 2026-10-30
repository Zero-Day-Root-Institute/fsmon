/* Test installation-prefix resolution without needing an iOS runtime. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <assert.h>
#ifndef FSMOD_DEFAULT_CONFIG
#define FSMOD_DEFAULT_CONFIG "/test-override/etc/fsmod.conf"
#endif
#define main fsmod_test_main
#include "../main.c"
#undef main

int main(void) {
    static const struct { const char *executable, *config; } cases[] = {
        {"/usr/sbin/fsmod", "/etc/fsmod.conf"},
        {"/sbin/fsmod", "/etc/fsmod.conf"},
        {"/var/jb/usr/sbin/fsmod", "/var/jb/etc/fsmod.conf"},
        {"/private/preboot/A1B2/jb-ABC/procursus/usr/sbin/fsmod",
         "/private/preboot/A1B2/jb-ABC/procursus/etc/fsmod.conf"},
        {"/usr/local/sbin/fsmod", "/etc/fsmod.conf"},
    };
    static const char *invalid[] = {
        "", "fsmod", "usr/sbin/fsmod", "/tmp/fsmod",
        "/usr/sbin/fsmod-old", "/usr/sbin/fsmod/", "/usr/sbin/fsmod/config",
        "/usr/bin/fsmod", "/bin/fsmod", "/var/jb/usr/bin/fsmod",
        "/private/preboot/A1B2/jb-ABC/procursus/usr/bin/fsmod", "/usr/local/bin/fsmod"
    };
    char path[PATH_MAX];
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(default_config_from_executable(cases[i].executable, path, sizeof(path)));
        assert(!strcmp(path, cases[i].config));
        /* The terminator must fit; no partial or truncated path may be used. */
        assert(default_config_from_executable(cases[i].executable, path, strlen(cases[i].config) + 1));
        assert(!strcmp(path, cases[i].config));
        assert(!default_config_from_executable(cases[i].executable, path, strlen(cases[i].config)));
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!default_config_from_executable(invalid[i], path, sizeof(path)));
    assert(!default_config_from_executable(NULL, path, sizeof(path)));
    assert(!default_config_from_executable("/usr/sbin/fsmod", NULL, sizeof(path)));
    assert(!default_config_from_executable("/usr/sbin/fsmod", path, 0));
    assert(default_config_path(path, sizeof(path)));
    assert(!strcmp(path, FSMOD_DEFAULT_CONFIG));
    assert(!default_config_path(path, strlen(FSMOD_DEFAULT_CONFIG)));
    puts("default configuration path tests passed");
    return 0;
}
