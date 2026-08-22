/* packinstall-lfs.c - minimal tar+zstd package manager for LFS/MLFS. */
#define _GNU_SOURCE
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REL "1"
#define DEFAULT_ROOT "/"
#define DB_REL "/var/lib/packinstall-lfs"
#define SUFFIX ".lfspkg.tar.zst"

static const char *root_dir = DEFAULT_ROOT;
static FILE *manifest;
static int walk_failed;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "packinstall-lfs: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static void usage(void) {
    puts("packinstall-lfs " REL);
    puts("LFS/MLFS tar+zstd package manager");
    puts("Usage:");
    puts("  packinstall-lfs create  STAGE NAME VERSION [OUTPUT]");
    puts("  packinstall-lfs install PACKAGE [--root DIR]");
    puts("  packinstall-lfs remove  NAME VERSION [--root DIR]");
    puts("  packinstall-lfs list    [--root DIR]");
    puts("  packinstall-lfs info    PACKAGE");
}

static int has_newline(const char *s) { return strchr(s, '\n') || strchr(s, '\r'); }

static void shell_quote(const char *s, char *out, size_t n) {
    size_t p = 0;
    if (n < 3) die("internal quoting buffer too small");
    out[p++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') {
            if (p + 4 >= n) die("path is too long");
            memcpy(out + p, "'\\''", 4); p += 4;
        } else {
            if (p + 2 >= n) die("path is too long");
            out[p++] = *s;
        }
    }
    out[p++] = '\''; out[p] = '\0';
}

static int run_command(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc)) return -1;
    return WEXITSTATUS(rc);
}

static void ensure_dir(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (!len || len >= sizeof(buf)) die("path too long: %s", path);
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) if (*p == '/') {
        *p = '\0';
        if (mkdir(buf, 0755) && errno != EEXIST) die("mkdir %s: %s", buf, strerror(errno));
        *p = '/';
    }
    if (mkdir(buf, 0755) && errno != EEXIST) die("mkdir %s: %s", buf, strerror(errno));
}

static int manifest_walk(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    if (ftwbuf->level == 0) return 0;
    if (typeflag != FTW_F && typeflag != FTW_D && typeflag != FTW_SL && typeflag != FTW_DP) return 0;
    const char *rel = fpath;
    if (rel[0] == '.') rel++;
    if (rel[0] == '/') rel++;
    if (has_newline(rel)) {
        fprintf(stderr, "packinstall-lfs: refusing filename containing newline: %s\n", fpath);
        walk_failed = 1;
        return 1;
    }
    fprintf(manifest, "%s\n", rel);
    return 0;
}

static void create_package(const char *stage, const char *name, const char *version, const char *output) {
    struct stat st;
    char qstage[PATH_MAX * 2], qout[PATH_MAX * 2];
    char info[PATH_MAX], files[PATH_MAX], cmd[PATH_MAX * 5];
    if (stat(stage, &st) != 0 || !S_ISDIR(st.st_mode)) die("stage directory does not exist: %s", stage);
    if (!name[0] || !version[0] || has_newline(name) || has_newline(version)) die("invalid package name/version");
    if (!output) {
        static char generated[PATH_MAX];
        snprintf(generated, sizeof(generated), "%s-%s-%s", name, version, SUFFIX);
        output = generated;
    }
    snprintf(info, sizeof(info), "%s/.LFSINFO", stage);
    snprintf(files, sizeof(files), "%s/.FILES", stage);
    FILE *fp = fopen(info, "w");
    if (!fp) die("cannot create %s: %s", info, strerror(errno));
    fprintf(fp, "format=packinstall-lfs/%s\nname=%s\nversion=%s\narch=native\n", REL, name, version);
    fclose(fp);
    manifest = fopen(files, "w");
    if (!manifest) die("cannot create %s: %s", files, strerror(errno));
    walk_failed = 0;
    if (nftw(stage, manifest_walk, 32, FTW_PHYS) != 0 || walk_failed) {
        fclose(manifest); unlink(info); unlink(files); die("cannot generate package manifest");
    }
    fclose(manifest);
    shell_quote(stage, qstage, sizeof(qstage));
    shell_quote(output, qout, sizeof(qout));
    snprintf(cmd, sizeof(cmd), "tar --zstd -C %s -cf %s .", qstage, qout);
    if (run_command(cmd) != 0) { unlink(info); unlink(files); die("tar failed while creating %s", output); }
    unlink(info); unlink(files);
    printf("created %s\n", output);
}

static int safe_member(const char *p) {
    if (!p || !*p || p[0] == '/') return 0;
    if (!strcmp(p, ".") || !strcmp(p, "./")) return 1;
    if (!strcmp(p, "..") || !strncmp(p, "../", 3) || strstr(p, "/../")) return 0;
    return 1;
}

static void validate_archive(const char *package) {
    char qp[PATH_MAX * 2], cmd[PATH_MAX * 3];
    shell_quote(package, qp, sizeof(qp));
    snprintf(cmd, sizeof(cmd), "tar --zstd -tf %s", qp);
    FILE *fp = popen(cmd, "r");
    if (!fp) die("cannot inspect %s", package);
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!safe_member(line)) { pclose(fp); die("unsafe archive member: %s", line); }
    }
    if (pclose(fp) != 0) die("cannot inspect archive: %s", package);
}

static void package_info(const char *package) {
    char qp[PATH_MAX * 2], cmd[PATH_MAX * 3];
    shell_quote(package, qp, sizeof(qp));
    snprintf(cmd, sizeof(cmd), "tar --zstd -xOf %s ./.LFSINFO 2>/dev/null", qp);
    if (run_command(cmd) != 0) die("%s has no valid .LFSINFO", package);
}

static void install_package(const char *package) {
    char qpkg[PATH_MAX * 2], qroot[PATH_MAX * 2], qinfo[PATH_MAX * 2], cmd[PATH_MAX * 4];
    char info[PATH_MAX], db[PATH_MAX], files[PATH_MAX];
    char name[256] = {0}, version[256] = {0};
    validate_archive(package);
    snprintf(info, sizeof(info), "/tmp/packinstall-lfs-info-%ld", (long)getpid());
    shell_quote(package, qpkg, sizeof(qpkg)); shell_quote(info, qinfo, sizeof(qinfo));
    snprintf(cmd, sizeof(cmd), "tar --zstd -xOf %s ./.LFSINFO > %s", qpkg, qinfo);
    if (run_command(cmd) != 0) { unlink(info); die("cannot read package metadata"); }
    FILE *fp = fopen(info, "r");
    if (!fp) { unlink(info); die("cannot read package metadata"); }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "name=%255s", name) == 1) continue;
        if (sscanf(line, "version=%255s", version) == 1) continue;
    }
    fclose(fp); unlink(info);
    if (!name[0] || !version[0]) die("package metadata is incomplete");
    snprintf(db, sizeof(db), "%s%s/%s/%s", root_dir, DB_REL, name, version);
    ensure_dir(db); snprintf(files, sizeof(files), "%s/files", db);
    shell_quote(root_dir, qroot, sizeof(qroot));
    snprintf(cmd, sizeof(cmd), "tar --zstd -xpf %s -C %s --no-same-owner --no-same-permissions", qpkg, qroot);
    if (run_command(cmd) != 0) die("failed to install %s", package);
    snprintf(cmd, sizeof(cmd), "tar --zstd -xOf %s ./.FILES > %s", qpkg, qinfo);
    if (run_command(cmd) != 0) die("package has no manifest");
    if (rename(info, files) != 0) die("cannot save package manifest: %s", strerror(errno));
    printf("installed %s-%s\n", name, version);
}

static void remove_package(const char *name, const char *version) {
    char db[PATH_MAX], files[PATH_MAX], path[PATH_MAX];
    snprintf(db, sizeof(db), "%s%s/%s/%s", root_dir, DB_REL, name, version);
    snprintf(files, sizeof(files), "%s/files", db);
    FILE *fp = fopen(files, "r");
    if (!fp) die("package %s-%s is not installed", name, version);
    while (fgets(path, sizeof(path), fp)) {
        path[strcspn(path, "\r\n")] = '\0';
        if (!safe_member(path)) continue;
        char full[PATH_MAX]; snprintf(full, sizeof(full), "%s/%s", root_dir, path);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir(full);
        else if (unlink(full) != 0 && errno != ENOENT)
            fprintf(stderr, "packinstall-lfs: cannot remove %s: %s\n", full, strerror(errno));
    }
    fclose(fp); unlink(files); rmdir(db);
    printf("removed %s-%s\n", name, version);
}

static void list_packages(void) {
    char base[PATH_MAX], cmd[PATH_MAX * 2], qbase[PATH_MAX * 2];
    snprintf(base, sizeof(base), "%s%s", root_dir, DB_REL);
    shell_quote(base, qbase, sizeof(qbase));
    snprintf(cmd, sizeof(cmd), "find %s -mindepth 2 -maxdepth 2 -type d -printf '%%P\\n' 2>/dev/null", qbase);
    run_command(cmd);
}

static const char *arg_value(int argc, char **argv, const char *opt) {
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], opt)) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return EXIT_FAILURE; }
    const char *root = arg_value(argc, argv, "--root");
    if (root) root_dir = root;
    if (!strcmp(argv[1], "create")) {
        if (argc < 5) { usage(); return EXIT_FAILURE; }
        create_package(argv[2], argv[3], argv[4], argc >= 6 ? argv[5] : NULL); return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "install")) {
        if (argc < 3) { usage(); return EXIT_FAILURE; }
        install_package(argv[2]); return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "remove")) {
        if (argc < 4) { usage(); return EXIT_FAILURE; }
        remove_package(argv[2], argv[3]); return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "list")) { list_packages(); return EXIT_SUCCESS; }
    if (!strcmp(argv[1], "info")) {
        if (argc < 3) { usage(); return EXIT_FAILURE; }
        package_info(argv[2]); return EXIT_SUCCESS;
    }
    usage(); return EXIT_FAILURE;
}
