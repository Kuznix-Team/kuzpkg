/*
 * packinstall-lfs.c - LFS/MLFS source packager using makepkg.
 *
 * Modern/default workflow:
 *
 *   packinstall-lfs create
 *   packinstall-lfs create .
 *   packinstall-lfs create /sources/kuzpkg
 *
 * Metadata:
 *
 *   PKGINFO
 *
 * Package format:
 *
 *   .kuzpkg.tar.zst
 *
 * Legacy raw-stage packaging is explicitly available through:
 *
 *   packinstall-lfs create-legacy STAGE NAME VERSION [OUTPUT]
 *
 * Supported build systems:
 *
 *   make
 *   autotools
 *   meson
 *   ninja
 *   cmake
 *   python
 *   python-setup
 *   python-pyproject
 *   python-cfg
 *   ruby
 *   perl
 *   go
 *   rust
 *   rustc
 *   cargo
 *   waf
 *   scons
 *
 * ABI profiles:
 *
 *   native
 *   lib32
 *   libx32
 *   libo32
 *   libm32
 *   ppc32
 *   ppc64
 *   ppc64le
 *
 * Root builds:
 *
 *   - automatically create/use "builder"
 *   - use useradd when available
 *   - fall back to /etc/passwd and /etc/group
 *   - never run makepkg as UID 0
 *
 * Copyright (C) 2026 Kuznix
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION "0.7"
#define SUFFIX ".kuzpkg.tar.zst"
#define PKGINFO_NAME "PKGINFO"
#define DB_REL "/var/lib/packinstall-lfs"

#define BUILDER_USER "builder"
#define BUILDER_GROUP "builder"
#define BUILDER_HOME "/home/builder"
#define BUILDER_SHELL "/bin/bash"

static const char *root_dir = "/";

/* ------------------------------------------------------------------------- */
/* ABI                                                                       */
/* ------------------------------------------------------------------------- */

enum package_abi {
	ABI_NATIVE = 0,
	ABI_LIB32,
	ABI_LIBX32,
	ABI_LIBO32,
	ABI_LIBM32,
	ABI_PPC32,
	ABI_PPC64,
	ABI_PPC64LE
};

struct abi_profile {
	enum package_abi abi;
	const char *name;
	const char *libdir;
	const char *flag;
	const char *machine;
};

static const struct abi_profile abi_profiles[] = {
	{
		ABI_NATIVE,
		"native",
		"/usr/lib",
		"",
		"native"
	},
	{
		ABI_LIB32,
		"lib32",
		"/usr/lib32",
		"-m32",
		"32-bit"
	},
	{
		ABI_LIBX32,
		"libx32",
		"/usr/libx32",
		"-mx32",
		"x32"
	},
	{
		ABI_LIBO32,
		"libo32",
		"/usr/libo32",
		"-mabi=32",
		"mips-o32"
	},
	{
		ABI_LIBM32,
		"libm32",
		"/usr/libm32",
		"-mabi=n32",
		"mips-n32"
	},
	{
		ABI_PPC32,
		"ppc32",
		"/usr/lib32",
		"-m32",
		"powerpc32"
	},
	{
		ABI_PPC64,
		"ppc64",
		"/usr/lib64",
		"-m64",
		"powerpc64"
	},
	{
		ABI_PPC64LE,
		"ppc64le",
		"/usr/lib64",
		"-m64",
		"powerpc64le"
	}
};

#define ABI_PROFILE_COUNT \
	(sizeof(abi_profiles) / sizeof(abi_profiles[0]))

/* ------------------------------------------------------------------------- */
/* Error handling                                                            */
/* ------------------------------------------------------------------------- */

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	fprintf(stderr, "packinstall-lfs: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);

	va_end(ap);

	exit(EXIT_FAILURE);
}

static void warnx(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	fprintf(stderr, "packinstall-lfs: warning: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);

	va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Usage                                                                     */
/* ------------------------------------------------------------------------- */

static void usage(void)
{
	puts("packinstall-lfs " VERSION " - LFS/MLFS makepkg packager");
	puts("");
	puts("Modern commands:");
	puts("  packinstall-lfs create [DIR] [options]");
	puts("  packinstall-lfs package [DIR] [options]");
	puts("");
	puts("Package management:");
	puts("  packinstall-lfs install PACKAGE [--root DIR]");
	puts("  packinstall-lfs remove NAME VERSION [--abi ABI] [--root DIR]");
	puts("  packinstall-lfs list [--root DIR]");
	puts("  packinstall-lfs info PACKAGE");
	puts("");
	puts("Legacy:");
	puts("  packinstall-lfs create-legacy STAGE NAME VERSION [OUTPUT]");
	puts("");
	puts("create/package options:");
	puts("  --dir DIR");
	puts("  --buildscript FILE");
	puts("  --build-system auto|make|autotools|meson|ninja|cmake|");
	puts("                 python|python-setup|python-pyproject|python-cfg|");
	puts("                 ruby|perl|go|rust|rustc|cargo|waf|scons");
	puts("  --abi native|lib32|libx32|libo32|libm32|ppc32|ppc64|ppc64le");
	puts("  --output DIR");
	puts("  --no-build");
	puts("  --skip-checks");
	puts("");
	puts("create with no arguments uses the current directory.");
	puts("");
	puts("Examples:");
	puts("  packinstall-lfs create");
	puts("  packinstall-lfs create .");
	puts("  packinstall-lfs create /sources/kuzpkg");
	puts("  packinstall-lfs create /sources/kuzpkg --no-build");
	puts("  packinstall-lfs create /sources/foo --abi lib32");
	puts("  packinstall-lfs create /sources/foo --build-system meson");
	puts("  packinstall-lfs create-legacy /tmp/pkgroot kuzpkg 0.1.0");
}

/* ------------------------------------------------------------------------- */
/* Generic helpers                                                           */
/* ------------------------------------------------------------------------- */

static void shell_quote(const char *s, char *out, size_t n)
{
	size_t p = 0;

	if (n < 3)
		die("internal quoting buffer too small");

	out[p++] = '\'';

	while (*s) {
		if (*s == '\'') {
			if (p + 4 >= n)
				die("quoted string is too long");

			memcpy(out + p, "'\\''", 4);
			p += 4;
		} else {
			if (p + 2 >= n)
				die("quoted string is too long");

			out[p++] = *s;
		}

		++s;
	}

	out[p++] = '\'';
	out[p] = '\0';
}

static int run_command(const char *cmd)
{
	int rc;

	rc = system(cmd);

	if (rc < 0)
		return -1;

	if (!WIFEXITED(rc))
		return -1;

	return WEXITSTATUS(rc);
}

static void ensure_dir(const char *path)
{
	char buffer[PATH_MAX];
	size_t len;
	char *p;

	len = strlen(path);

	if (!len || len >= sizeof(buffer))
		die("path too long: %s", path);

	memcpy(buffer, path, len + 1);

	for (p = buffer + 1; *p; ++p) {
		if (*p != '/')
			continue;

		*p = '\0';

		if (mkdir(buffer, 0755) != 0 &&
		    errno != EEXIST) {
			die("mkdir %s: %s",
			    buffer,
			    strerror(errno));
		}

		*p = '/';
	}

	if (mkdir(buffer, 0755) != 0 &&
	    errno != EEXIST) {
		die("mkdir %s: %s",
		    buffer,
		    strerror(errno));
	}
}

static const char *arg_value(int argc,
			     char **argv,
			     const char *opt)
{
	int i;

	for (i = 1; i + 1 < argc; ++i) {
		if (!strcmp(argv[i], opt))
			return argv[i + 1];
	}

	return NULL;
}

static int has_opt(int argc,
		   char **argv,
		   const char *opt)
{
	int i;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], opt))
			return 1;
	}

	return 0;
}

static int is_file(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;

	return S_ISREG(st.st_mode);
}

static int is_directory(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;

	return S_ISDIR(st.st_mode);
}

static int command_exists(const char *path)
{
	return access(path, X_OK) == 0;
}

static void absolute_path(const char *input,
			  char *output,
			  size_t size)
{
	char *resolved;

	resolved = realpath(input, NULL);

	if (!resolved)
		die("cannot resolve %s: %s",
		    input,
		    strerror(errno));

	if (strlen(resolved) >= size) {
		free(resolved);
		die("path too long: %s",
		    input);
	}

	strcpy(output, resolved);

	free(resolved);
}

/* ------------------------------------------------------------------------- */
/* Root/builder handling                                                     */
/* ------------------------------------------------------------------------- */

static struct passwd *lookup_builder(void)
{
	return getpwnam(BUILDER_USER);
}

static int group_exists(const char *name)
{
	return getgrnam(name) != NULL;
}

static int passwd_uid_in_use(uid_t uid)
{
	struct passwd *pw;

	setpwent();

	while ((pw = getpwent()) != NULL) {
		if (pw->pw_uid == uid) {
			endpwent();
			return 1;
		}
	}

	endpwent();

	return 0;
}

static int group_gid_in_use(gid_t gid)
{
	struct group *gr;

	setgrent();

	while ((gr = getgrent()) != NULL) {
		if (gr->gr_gid == gid) {
			endgrent();
			return 1;
		}
	}

	endgrent();

	return 0;
}

static uid_t choose_builder_uid(void)
{
	uid_t uid;

	for (uid = 900; uid < 65000; ++uid) {
		if (!passwd_uid_in_use(uid))
			return uid;
	}

	die("cannot find an unused UID for builder");

	return 0;
}

static gid_t choose_builder_gid(void)
{
	gid_t gid;

	for (gid = 900; gid < 65000; ++gid) {
		if (!group_gid_in_use(gid))
			return gid;
	}

	die("cannot find an unused GID for builder");

	return 0;
}

static void append_line_file(const char *path,
			     const char *line,
			     mode_t mode)
{
	int fd;
	size_t remaining;
	const char *p;

	fd = open(path,
		  O_WRONLY | O_APPEND | O_CREAT,
		  mode);

	if (fd < 0)
		die("cannot open %s: %s",
		    path,
		    strerror(errno));

	p = line;
	remaining = strlen(line);

	while (remaining > 0) {
		ssize_t n;

		n = write(fd,
			  p,
			  remaining);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			close(fd);

			die("cannot write %s: %s",
			    path,
			    strerror(errno));
		}

		p += n;
		remaining -= (size_t)n;
	}

	close(fd);
}

/*
 * Fallback equivalent to appending to:
 *
 *   /etc/group
 *   /etc/passwd
 *
 * using cat >>, but implemented directly in C so it works on very small
 * LFS systems where useradd/cat may not both be available.
 */
static void create_builder_from_files(void)
{
	uid_t uid;
	gid_t gid;

	char group_line[256];
	char passwd_line[512];

	if (geteuid() != 0)
		die("builder account creation requires root");

	if (lookup_builder())
		return;

	uid = choose_builder_uid();

	if (group_exists(BUILDER_GROUP)) {
		struct group *gr = getgrnam(BUILDER_GROUP);

		if (!gr)
			die("builder group lookup failed");

		gid = gr->gr_gid;
	} else {
		gid = choose_builder_gid();
	}

	if (gid == 0)
		die("refusing builder group with GID 0");

	if (!group_exists(BUILDER_GROUP)) {
		snprintf(group_line,
			 sizeof(group_line),
			 "%s:x:%lu:\n",
			 BUILDER_GROUP,
			 (unsigned long)gid);

		append_line_file("/etc/group",
				 group_line,
				 0644);
	}

	{
		const char *shell =
			is_file(BUILDER_SHELL) ?
			BUILDER_SHELL :
			"/bin/sh";

		snprintf(passwd_line,
			 sizeof(passwd_line),
			 "%s:x:%lu:%lu:LFS package builder:%s:%s\n",
			 BUILDER_USER,
			 (unsigned long)uid,
			 (unsigned long)gid,
			 BUILDER_HOME,
			 shell);

		append_line_file("/etc/passwd",
				 passwd_line,
				 0644);
	}

	endpwent();
	endgrent();

	if (!lookup_builder())
		die("failed to create builder in /etc/passwd");

	ensure_dir(BUILDER_HOME);

	if (chown(BUILDER_HOME,
		  uid,
		  gid) != 0) {
		die("cannot chown %s: %s",
		    BUILDER_HOME,
		    strerror(errno));
	}

	if (chmod(BUILDER_HOME,
		  0700) != 0) {
		die("cannot chmod %s: %s",
		    BUILDER_HOME,
		    strerror(errno));
	}

	printf("created builder account using /etc/passwd and /etc/group "
	       "(uid=%lu gid=%lu)\n",
	       (unsigned long)uid,
	       (unsigned long)gid);
}

static void create_builder_user(void)
{
	struct passwd *pw;
	char cmd[PATH_MAX * 3];

	pw = lookup_builder();

	if (pw) {
		if (pw->pw_uid == 0)
			die("builder user exists with UID 0");

		return;
	}

	if (command_exists("/usr/sbin/useradd")) {
		snprintf(cmd,
			 sizeof(cmd),
			 "/usr/sbin/useradd "
			 "--system "
			 "--create-home "
			 "--user-group "
			 "--home-dir %s "
			 "--shell %s "
			 "%s",
			 BUILDER_HOME,
			 BUILDER_SHELL,
			 BUILDER_USER);

		if (run_command(cmd) == 0) {
			endpwent();
			endgrent();

			pw = lookup_builder();

			if (pw && pw->pw_uid != 0) {
				ensure_dir(BUILDER_HOME);

				printf("created builder using useradd "
				       "(uid=%lu gid=%lu)\n",
				       (unsigned long)pw->pw_uid,
				       (unsigned long)pw->pw_gid);

				return;
			}
		}
	}

	if (command_exists("/usr/bin/useradd")) {
		snprintf(cmd,
			 sizeof(cmd),
			 "/usr/bin/useradd "
			 "--system "
			 "--create-home "
			 "--user-group "
			 "--home-dir %s "
			 "--shell %s "
			 "%s",
			 BUILDER_HOME,
			 BUILDER_SHELL,
			 BUILDER_USER);

		if (run_command(cmd) == 0) {
			endpwent();
			endgrent();

			pw = lookup_builder();

			if (pw && pw->pw_uid != 0) {
				ensure_dir(BUILDER_HOME);

				printf("created builder using useradd "
				       "(uid=%lu gid=%lu)\n",
				       (unsigned long)pw->pw_uid,
				       (unsigned long)pw->pw_gid);

				return;
			}
		}
	}

	/*
	 * useradd unavailable or failed: direct /etc/passwd/group fallback.
	 */
	create_builder_from_files();
}

static void ensure_builder(void)
{
	struct passwd *pw;

	if (geteuid() != 0)
		return;

	pw = lookup_builder();

	if (pw && pw->pw_uid != 0)
		return;

	create_builder_user();
}

static uid_t builder_uid(void)
{
	struct passwd *pw = lookup_builder();

	if (!pw)
		die("builder user does not exist");

	if (pw->pw_uid == 0)
		die("builder user has UID 0");

	return pw->pw_uid;
}

static gid_t builder_gid(void)
{
	struct passwd *pw = lookup_builder();

	if (!pw)
		die("builder user does not exist");

	if (pw->pw_gid == 0)
		die("builder group has GID 0");

	return pw->pw_gid;
}

/* ------------------------------------------------------------------------- */
/* ABI helpers                                                               */
/* ------------------------------------------------------------------------- */

static const struct abi_profile *find_abi(const char *name)
{
	size_t i;

	if (!name || !strcmp(name, "native"))
		return &abi_profiles[ABI_NATIVE];

	for (i = 0; i < ABI_PROFILE_COUNT; ++i) {
		if (!strcmp(name, abi_profiles[i].name))
			return &abi_profiles[i];
	}

	return NULL;
}

static const struct abi_profile *detect_default_abi(void)
{
	struct utsname uts;

	if (uname(&uts) != 0)
		return &abi_profiles[ABI_NATIVE];

	if (!strcmp(uts.machine, "ppc64"))
		return &abi_profiles[ABI_PPC64];

	if (!strcmp(uts.machine, "ppc64le"))
		return &abi_profiles[ABI_PPC64LE];

	return &abi_profiles[ABI_NATIVE];
}

/* ------------------------------------------------------------------------- */
/* makepkg metadata                                                          */
/* ------------------------------------------------------------------------- */

static void read_srcinfo(const char *dir,
			 char *name,
			 size_t name_size,
			 char *version,
			 size_t version_size,
			 char *release,
			 size_t release_size)
{
	char qdir[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];
	FILE *fp;
	char line[4096];
	int rc;

	name[0] = '\0';
	version[0] = '\0';
	release[0] = '\0';

	shell_quote(dir,
		    qdir,
		    sizeof(qdir));

	snprintf(cmd,
		 sizeof(cmd),
		 "cd %s && makepkg --printsrcinfo 2>/dev/null",
		 qdir);

	fp = popen(cmd, "r");

	if (!fp)
		die("cannot execute makepkg --printsrcinfo");

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		char *eq;
		char *value;

		eq = strchr(line, '=');

		if (!eq)
			continue;

		*eq++ = '\0';

		value = eq;

		while (*value == ' ' ||
		       *value == '\t')
			++value;

		value[strcspn(value,
			      "\r\n")] = '\0';

		if (!strcmp(line, "pkgname")) {
			snprintf(name,
				 name_size,
				 "%s",
				 value);
		} else if (!strcmp(line, "pkgver")) {
			snprintf(version,
				 version_size,
				 "%s",
				 value);
		} else if (!strcmp(line, "pkgrel")) {
			snprintf(release,
				 release_size,
				 "%s",
				 value);
		}
	}

	rc = pclose(fp);

	if (rc != 0 ||
	    !name[0] ||
	    !version[0]) {
		die("makepkg could not read pkgname/pkgver from PKGBUILD");
	}

	if (!release[0])
		snprintf(release,
			 release_size,
			 "1");
}

/* ------------------------------------------------------------------------- */
/* Build system detection                                                    */
/* ------------------------------------------------------------------------- */

static int has_named_file(const char *dir,
			  const char *name)
{
	char path[PATH_MAX];

	if (snprintf(path,
		     sizeof(path),
		     "%s/%s",
		     dir,
		     name) >= (int)sizeof(path)) {
		die("source path is too long");
	}

	return is_file(path);
}

static const char *detect_build_system(const char *dir)
{
	if (has_named_file(dir, "x.py"))
		return "x.py";

	if (has_named_file(dir, "meson.build"))
		return "meson.build";

	if (has_named_file(dir, "CMakeLists.txt"))
		return "CMakeLists.txt";

	if (has_named_file(dir, "configure.ac"))
		return "configure.ac";

	if (has_named_file(dir, "configure.in"))
		return "configure.in";

	if (has_named_file(dir, "autogen.sh"))
		return "autogen.sh";

	if (has_named_file(dir, "bootstrap"))
		return "bootstrap";

	if (has_named_file(dir, "Makefile.am"))
		return "Makefile.am";

	if (has_named_file(dir, "waf"))
		return "waf";

	if (has_named_file(dir, "wscript"))
		return "wscript";

	if (has_named_file(dir, "SConstruct"))
		return "SConstruct";

	if (has_named_file(dir, "sconstruct"))
		return "sconstruct";

	if (has_named_file(dir, "pyproject.toml"))
		return "pyproject.toml";

	if (has_named_file(dir, "setup.py"))
		return "setup.py";

	if (has_named_file(dir, "setup.cfg"))
		return "setup.cfg";

	if (has_named_file(dir, "extconf.rb"))
		return "extconf.rb";

	if (has_named_file(dir, "Makefile.PL"))
		return "Makefile.PL";

	if (has_named_file(dir, "go.mod"))
		return "go.mod";

	if (has_named_file(dir, "Cargo.toml"))
		return "Cargo.toml";

	if (has_named_file(dir, "Makefile"))
		return "Makefile";

	return NULL;
}

static const char *normalize_build_system(const char *system,
					  const char *dir)
{
	char path[PATH_MAX];

	if (!system ||
	    !strcmp(system, "auto"))
		return detect_build_system(dir);

	if (!strcmp(system, "make"))
		return "Makefile";

	if (!strcmp(system, "autotools"))
		return "configure.ac";

	if (!strcmp(system, "meson") ||
	    !strcmp(system, "ninja"))
		return "meson.build";

	if (!strcmp(system, "cmake"))
		return "CMakeLists.txt";

	if (!strcmp(system, "python")) {
		if (has_named_file(dir,
				   "pyproject.toml"))
			return "pyproject.toml";

		if (has_named_file(dir,
				   "setup.py"))
			return "setup.py";

		if (has_named_file(dir,
				   "setup.cfg"))
			return "setup.cfg";

		return "pyproject.toml";
	}

	if (!strcmp(system, "python-setup"))
		return "setup.py";

	if (!strcmp(system, "python-pyproject"))
		return "pyproject.toml";

	if (!strcmp(system, "python-cfg"))
		return "setup.cfg";

	if (!strcmp(system, "ruby"))
		return "extconf.rb";

	if (!strcmp(system, "perl"))
		return "Makefile.PL";

	if (!strcmp(system, "go"))
		return "go.mod";

	if (!strcmp(system, "rust") ||
	    !strcmp(system, "rustc")) {
		snprintf(path,
			 sizeof(path),
			 "%s/x.py",
			 dir);

		if (is_file(path))
			return "x.py";

		return "Cargo.toml";
	}

	if (!strcmp(system, "cargo"))
		return "Cargo.toml";

	if (!strcmp(system, "waf"))
		return "waf";

	if (!strcmp(system, "scons"))
		return "SConstruct";

	return system;
}

/* ------------------------------------------------------------------------- */
/* ABI environment                                                           */
/* ------------------------------------------------------------------------- */

static void append_abi_environment(FILE *fp,
				   const struct abi_profile *abi)
{
	fprintf(fp,
		"_packinstall_abi='%s'\n",
		abi->name);

	fprintf(fp,
		"_packinstall_libdir='%s'\n",
		abi->libdir);

	fprintf(fp,
		"export LIBDIR='%s'\n",
		abi->libdir);

	fprintf(fp,
		"export libdir='%s'\n",
		abi->libdir);

	if (abi->flag[0]) {
		fprintf(fp,
			"export CFLAGS=\"${CFLAGS:-} %s\"\n",
			abi->flag);

		fprintf(fp,
			"export CXXFLAGS=\"${CXXFLAGS:-} %s\"\n",
			abi->flag);

		fprintf(fp,
			"export CPPFLAGS=\"${CPPFLAGS:-} %s\"\n",
			abi->flag);

		fprintf(fp,
			"export LDFLAGS=\"${LDFLAGS:-} -L%s\"\n",
			abi->libdir);
	}

	if (abi->abi == ABI_LIBO32) {
		fprintf(fp,
			"export MIPS_ABI=o32\n");
	}

	if (abi->abi == ABI_LIBM32) {
		fprintf(fp,
			"export MIPS_ABI=n32\n");
	}
}

/* ------------------------------------------------------------------------- */
/* Generated build()                                                         */
/* ------------------------------------------------------------------------- */

static void append_generated_build(FILE *fp,
				   const char *system,
				   int no_build)
{
	if (no_build) {
		fprintf(fp,
			"build() {\n"
			"  :\n"
			"}\n");

		return;
	}

	fprintf(fp,
		"build() {\n"
		"  cd \"$_packinstall_source\"\n");

	if (!strcmp(system, "meson.build")) {
		fprintf(fp,
			"  command -v meson >/dev/null 2>&1 || return 127\n"
			"  command -v ninja >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-build\n"
			"  meson setup .packinstall-build "
			"--prefix=/usr "
			"--libdir=\"${LIBDIR#/usr/}\" "
			"--buildtype=release\n"
			"  ninja -C .packinstall-build\n");

	} else if (!strcmp(system, "CMakeLists.txt")) {
		fprintf(fp,
			"  command -v cmake >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-build\n"
			"  cmake -S . "
			"-B .packinstall-build "
			"-DCMAKE_BUILD_TYPE=Release "
			"-DCMAKE_INSTALL_PREFIX=/usr "
			"-DCMAKE_INSTALL_LIBDIR=\"${LIBDIR#/usr/}\"\n"
			"  cmake --build .packinstall-build\n");

	} else if (!strcmp(system, "configure.ac") ||
		   !strcmp(system, "configure.in") ||
		   !strcmp(system, "autogen.sh") ||
		   !strcmp(system, "bootstrap") ||
		   !strcmp(system, "Makefile.am")) {
		fprintf(fp,
			"  if [ ! -x ./configure ]; then\n"
			"    if [ -x ./autogen.sh ]; then\n"
			"      ./autogen.sh\n"
			"    elif [ -x ./bootstrap ]; then\n"
			"      ./bootstrap\n"
			"    else\n"
			"      command -v autoreconf >/dev/null 2>&1 || return 127\n"
			"      autoreconf -fi\n"
			"    fi\n"
			"  fi\n"
			"  ./configure "
			"--prefix=/usr "
			"--libdir=\"$LIBDIR\"\n"
			"  make\n");

	} else if (!strcmp(system, "Makefile")) {
		fprintf(fp,
			"  command -v make >/dev/null 2>&1 || return 127\n"
			"  make "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\"\n");

	} else if (!strcmp(system, "waf")) {
		fprintf(fp,
			"  test -x ./waf || chmod +x ./waf\n"
			"  ./waf configure --prefix=/usr\n"
			"  ./waf build\n");

	} else if (!strcmp(system, "wscript")) {
		fprintf(fp,
			"  command -v waf >/dev/null 2>&1 || return 127\n"
			"  waf configure --prefix=/usr\n"
			"  waf build\n");

	} else if (!strcmp(system, "SConstruct") ||
		   !strcmp(system, "sconstruct")) {
		fprintf(fp,
			"  command -v scons >/dev/null 2>&1 || return 127\n"
			"  scons\n");

	} else if (!strcmp(system, "setup.py")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n"
			"  python3 setup.py build\n");

	} else if (!strcmp(system, "setup.cfg")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n"
			"  python3 -m build --wheel --no-isolation\n");

	} else if (!strcmp(system, "pyproject.toml")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-wheels\n"
			"  mkdir -p .packinstall-wheels\n"
			"  python3 -m pip wheel "
			"--no-deps "
			"--no-build-isolation "
			"--wheel-dir .packinstall-wheels .\n");

	} else if (!strcmp(system, "extconf.rb")) {
		fprintf(fp,
			"  command -v ruby >/dev/null 2>&1 || return 127\n"
			"  ruby extconf.rb --prefix=/usr\n"
			"  make\n");

	} else if (!strcmp(system, "Makefile.PL")) {
		fprintf(fp,
			"  command -v perl >/dev/null 2>&1 || return 127\n"
			"  perl Makefile.PL PREFIX=/usr\n"
			"  make\n");

	} else if (!strcmp(system, "go.mod")) {
		fprintf(fp,
			"  command -v go >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-go\n"
			"  mkdir -p .packinstall-go/bin\n"
			"  go build "
			"-buildvcs=false "
			"-o .packinstall-go/bin/ ./...\n");

	} else if (!strcmp(system, "Cargo.toml")) {
		fprintf(fp,
			"  command -v cargo >/dev/null 2>&1 || return 127\n"
			"  cargo build --release\n");

	} else if (!strcmp(system, "x.py")) {
		fprintf(fp,
			"  test -x ./x.py || chmod +x ./x.py\n"
			"  ./x.py build\n");

	} else {
		fprintf(fp,
			"  make "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\"\n");
	}

	fprintf(fp,
		"}\n");
}

/* ------------------------------------------------------------------------- */
/* Generated package()                                                       */
/* ------------------------------------------------------------------------- */

static void append_generated_package(FILE *fp,
				     const char *system,
				     const struct abi_profile *abi)
{
	fprintf(fp,
		"package() {\n"
		"  cd \"$_packinstall_source\"\n"
		"  export DESTDIR=\"$pkgdir\"\n"
		"  export PREFIX=/usr\n"
		"  export prefix=/usr\n"
		"  export LIBDIR='%s'\n"
		"  export libdir='%s'\n",
		abi->libdir,
		abi->libdir);

	if (!strcmp(system, "meson.build")) {
		fprintf(fp,
			"  test -d .packinstall-build || return 1\n"
			"  DESTDIR=\"$pkgdir\" "
			"ninja -C .packinstall-build install\n");

	} else if (!strcmp(system, "CMakeLists.txt")) {
		fprintf(fp,
			"  test -d .packinstall-build || return 1\n"
			"  DESTDIR=\"$pkgdir\" "
			"cmake --install .packinstall-build\n");

	} else if (!strcmp(system, "configure.ac") ||
		   !strcmp(system, "configure.in") ||
		   !strcmp(system, "autogen.sh") ||
		   !strcmp(system, "bootstrap") ||
		   !strcmp(system, "Makefile.am")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n");

	} else if (!strcmp(system, "Makefile")) {
		fprintf(fp,
			"  if make -n install >/dev/null 2>&1; then\n"
			"    DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n"
			"  else\n"
			"    echo 'Makefile has no install target' >&2\n"
			"    return 1\n"
			"  fi\n");

	} else if (!strcmp(system, "waf")) {
		fprintf(fp,
			"  test -x ./waf || chmod +x ./waf\n"
			"  DESTDIR=\"$pkgdir\" ./waf install\n");

	} else if (!strcmp(system, "wscript")) {
		fprintf(fp,
			"  command -v waf >/dev/null 2>&1 || return 127\n"
			"  DESTDIR=\"$pkgdir\" waf install\n");

	} else if (!strcmp(system, "SConstruct") ||
		   !strcmp(system, "sconstruct")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"scons install "
			"prefix=/usr "
			"libdir=\"$LIBDIR\"\n");

	} else if (!strcmp(system, "setup.py")) {
		fprintf(fp,
			"  python3 setup.py install "
			"--root=\"$pkgdir\" "
			"--prefix=/usr "
			"--single-version-externally-managed "
			"--record=\"$srcdir/install-record.txt\"\n");

	} else if (!strcmp(system, "setup.cfg")) {
		fprintf(fp,
			"  for whl in dist/*.whl; do\n"
			"    test -f \"$whl\" || continue\n"
			"    python3 -m pip install "
			"--no-deps "
			"--no-index "
			"--root \"$pkgdir\" "
			"--prefix /usr "
			"\"$whl\"\n"
			"  done\n");

	} else if (!strcmp(system, "pyproject.toml")) {
		fprintf(fp,
			"  for whl in .packinstall-wheels/*.whl; do\n"
			"    test -f \"$whl\" || continue\n"
			"    python3 -m pip install "
			"--no-deps "
			"--no-index "
			"--root \"$pkgdir\" "
			"--prefix /usr "
			"\"$whl\"\n"
			"  done\n");

	} else if (!strcmp(system, "extconf.rb")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"make install\n");

	} else if (!strcmp(system, "Makefile.PL")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"make install\n");

	} else if (!strcmp(system, "go.mod")) {
		fprintf(fp,
			"  mkdir -p \"$pkgdir/usr/bin\"\n"
			"  for bin in .packinstall-go/bin/*; do\n"
			"    test -f \"$bin\" || continue\n"
			"    install -m755 \"$bin\" \"$pkgdir/usr/bin/\"\n"
			"  done\n");

	} else if (!strcmp(system, "Cargo.toml")) {
		fprintf(fp,
			"  mkdir -p \"$pkgdir/usr/bin\"\n"
			"  found=0\n"
			"  for bin in target/release/*; do\n"
			"    test -f \"$bin\" || continue\n"
			"    test -x \"$bin\" || continue\n"
			"    case \"$bin\" in\n"
			"      *.so|*.a|*.rlib|*.rmeta|*.d) continue ;;\n"
			"    esac\n"
			"    install -m755 \"$bin\" \"$pkgdir/usr/bin/\"\n"
			"    found=1\n"
			"  done\n"
			"  test \"$found\" = 1 || return 1\n");

	} else if (!strcmp(system, "x.py")) {
		fprintf(fp,
			"  test -d build || return 1\n"
			"  DESTDIR=\"$pkgdir\" ./x.py install\n");

	} else {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n");
	}

	fprintf(fp,
		"}\n");
}

/* ------------------------------------------------------------------------- */
/* PKGINFO creation                                                          */
/* ------------------------------------------------------------------------- */

static void append_pkginfo_generation(FILE *fp,
				      const char *name,
				      const char *version,
				      const char *release,
				      const struct abi_profile *abi)
{
	fprintf(fp,
		"packinstall_lfs_pkginfo() {\n"
		"  mkdir -p \"$pkgdir/etc/packinstall-lfs\"\n"
		"  {\n"
		"    printf 'format=packinstall-lfs/%s\\n'\n"
		"    printf 'pkgname=%s\\n'\n"
		"    printf 'pkgver=%s\\n'\n"
		"    printf 'pkgrel=%s\\n'\n"
		"    printf 'abi=%s\\n'\n"
		"    printf 'libdir=%s\\n'\n"
		"    printf 'machine=%s\\n'\n"
		"  } > \"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"}\n",
		VERSION,
		name,
		version,
		release,
		abi->name,
		abi->libdir,
		abi->machine);
}

/* ------------------------------------------------------------------------- */
/* Existing PKGBUILD functions                                               */
/* ------------------------------------------------------------------------- */

static void save_original_function(FILE *fp,
				   const char *function)
{
	fprintf(fp,
		"if declare -F %s >/dev/null 2>&1; then\n"
		"  eval \"$(declare -f %s | "
		"sed '1s/^%s /_packinstall_original_%s /')\"\n"
		"fi\n",
		function,
		function,
		function,
		function);
}

static void append_prepare_function(FILE *fp)
{
	fprintf(fp,
		"prepare() {\n"
		"  if declare -F "
		"_packinstall_original_prepare "
		">/dev/null 2>&1; then\n"
		"    _packinstall_original_prepare\n"
		"  fi\n"
		"}\n");
}

static void append_check_function(FILE *fp,
				  int skip_checks,
				  int no_build)
{
	fprintf(fp,
		"check() {\n");

	if (!skip_checks && !no_build) {
		fprintf(fp,
			"  if declare -F "
			"_packinstall_original_check "
			">/dev/null 2>&1; then\n"
			"    _packinstall_original_check\n"
			"  fi\n");
	} else {
		fprintf(fp,
			"  :\n");
	}

	fprintf(fp,
		"}\n");
}

/* ------------------------------------------------------------------------- */
/* Create generated PKGBUILD                                                 */
/* ------------------------------------------------------------------------- */

static void create_wrapper_pkgbuid(
	const char *work_source,
	const char *original_pkgbuild,
	const char *temp,
	const char *system_override,
	const struct abi_profile *abi,
	const char *name,
	const char *version,
	const char *release,
	int skip_checks,
	int no_build)
{
	char wrapper[PATH_MAX];
	char qoriginal[PATH_MAX * 2];

	const char *system;
	int forced_system;

	FILE *fp;

	if (snprintf(wrapper,
		     sizeof(wrapper),
		     "%s/PKGBUILD",
		     temp) >= (int)sizeof(wrapper)) {
		die("generated PKGBUILD path is too long");
	}

	system = normalize_build_system(system_override,
					work_source);

	if (!system)
		die("unable to detect a supported build system in %s",
		    work_source);

	forced_system =
		system_override &&
		strcmp(system_override, "auto") != 0;

	shell_quote(original_pkgbuild,
		    qoriginal,
		    sizeof(qoriginal));

	fp = fopen(wrapper,
		   "w");

	if (!fp)
		die("cannot create generated PKGBUILD: %s",
		    strerror(errno));

	fprintf(fp,
		"# generated by packinstall-lfs %s\n",
		VERSION);

	/*
	 * Load the user's PKGBUILD definitions.
	 */
	fprintf(fp,
		"source %s\n",
		qoriginal);

	fprintf(fp,
		"_packinstall_source='%s'\n",
		work_source);

	fprintf(fp,
		"_packinstall_pkgname='%s'\n",
		name);

	fprintf(fp,
		"_packinstall_pkgver='%s'\n",
		version);

	fprintf(fp,
		"_packinstall_pkgrel='%s'\n",
		release);

	fprintf(fp,
		"_packinstall_basename=$(basename "
		"\"$_packinstall_source\")\n");

	/*
	 * Do not make makepkg extract a second source tree.
	 */
	fprintf(fp,
		"source=()\n"
		"sha256sums=()\n"
		"sha512sums=()\n"
		"b2sums=()\n"
		"md5sums=()\n");

	/*
	 * ABI-specific pkgname.
	 */
	if (abi->abi == ABI_NATIVE) {
		fprintf(fp,
			"pkgname='%s'\n",
			name);
	} else {
		fprintf(fp,
			"pkgname='%s-%s'\n",
			name,
			abi->name);
	}

	fprintf(fp,
		"pkgver='%s'\n"
		"pkgrel='%s'\n",
		version,
		release);

	if (abi->abi == ABI_NATIVE) {
		fprintf(fp,
			"arch=('auto')\n");
	} else {
		fprintf(fp,
			"arch=('%s')\n",
			abi->name);
	}

	append_pkginfo_generation(fp,
				      name,
				      version,
				      release,
				      abi);

	save_original_function(fp,
			       "prepare");

	save_original_function(fp,
			       "build");

	save_original_function(fp,
			       "check");

	save_original_function(fp,
			       "package");

	append_abi_environment(fp,
			       abi);

	append_prepare_function(fp);

	/*
	 * Build.
	 */
	if (no_build) {
		fprintf(fp,
			"build() {\n"
			"  :\n"
			"}\n");
	} else if (forced_system) {
		append_generated_build(fp,
				       system,
				       0);
	} else {
		fprintf(fp,
			"if declare -F "
			"_packinstall_original_build "
			">/dev/null 2>&1; then\n"
			"  build() {\n"
			"    _packinstall_original_build\n"
			"  }\n"
			"else\n");

		append_generated_build(fp,
				       system,
				       0);

		fprintf(fp,
			"fi\n");
	}

	append_check_function(fp,
			      skip_checks,
			      no_build);

	/*
	 * package() selection.
	 */
	if (forced_system) {
		append_generated_package(fp,
					 system,
					 abi);
	} else {
		fprintf(fp,
			"if declare -F "
			"_packinstall_original_package "
			">/dev/null 2>&1; then\n"
			"  package() {\n"
			"    _packinstall_original_package\n"
			"  }\n"
			"else\n");

		append_generated_package(fp,
					 system,
					 abi);

		fprintf(fp,
			"fi\n");
	}

	/*
	 * PKGINFO must be placed in $pkgdir after package() has staged files.
	 */
	fprintf(fp,
		"_packinstall_finish() {\n"
		"  packinstall_lfs_pkginfo\n"
		"}\n");

	/*
	 * Replace package() one final time so PKGINFO is guaranteed to be
	 * generated after the actual package() implementation.
	 */
	fprintf(fp,
		"_packinstall_package_impl() {\n"
		"  package\n"
		"}\n");

	fprintf(fp,
		"package() {\n"
		"  _packinstall_package_impl\n"
		"  packinstall_lfs_pkginfo\n"
		"}\n");

	/*
	 * The replacement above would recurse. Instead, redefine the actual
	 * implementation safely by capturing the generated package function.
	 */
	fprintf(fp,
		"eval \"$(declare -f package | "
		"sed '1s/^package /_packinstall_final_package /')\"\n");

	fprintf(fp,
		"package() {\n"
		"  _packinstall_final_package\n"
		"  packinstall_lfs_pkginfo\n"
		"}\n");

	fclose(fp);
}

/* ------------------------------------------------------------------------- */
/* Copy source tree for root builds                                          */
/* ------------------------------------------------------------------------- */

static void copy_source_tree(const char *source,
			     char *destination,
			     size_t destination_size)
{
	char templ[PATH_MAX];
	char qsource[PATH_MAX * 2];
	char qdestination[PATH_MAX * 2];
	char cmd[PATH_MAX * 8];
	const char *base;

	snprintf(templ,
		 sizeof(templ),
		 "/tmp/packinstall-lfs-builder-XXXXXX");

	if (!mkdtemp(templ))
		die("cannot create builder temporary directory: %s",
		    strerror(errno));

	base = strrchr(source,
		       '/');

	if (base)
		++base;
	else
		base = source;

	if (snprintf(destination,
		     destination_size,
		     "%s/%s",
		     templ,
		     base) >= (int)destination_size) {
		die("builder source path is too long");
	}

	shell_quote(source,
		    qsource,
		    sizeof(qsource));

	shell_quote(destination,
		    qdestination,
		    sizeof(qdestination));

	snprintf(cmd,
		 sizeof(cmd),
		 "cp -a %s %s",
		 qsource,
		 qdestination);

	if (run_command(cmd) != 0)
		die("cannot copy source tree");

	snprintf(cmd,
		 sizeof(cmd),
		 "chown -R %lu:%lu %s",
		 (unsigned long)builder_uid(),
		 (unsigned long)builder_gid(),
		 qdestination);

	if (run_command(cmd) != 0)
		die("cannot chown source tree to builder");

	snprintf(cmd,
		 sizeof(cmd),
		 "chmod 700 %s",
		 qdestination);

	if (run_command(cmd) != 0)
		die("cannot chmod builder source tree");
}

static void remove_tree(const char *path)
{
	char qpath[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	shell_quote(path,
		    qpath,
		    sizeof(qpath));

	snprintf(cmd,
		 sizeof(cmd),
		 "rm -rf -- %s",
		 qpath);

	(void)run_command(cmd);
}

/* ------------------------------------------------------------------------- */
/* Execute makepkg                                                           */
/* ------------------------------------------------------------------------- */

static void execute_makepkg(const char *workdir,
			    const char *wrapper,
			    const char *output,
			    int skip_checks,
			    int no_build)
{
	char qworkdir[PATH_MAX * 2];
	char qwrapper[PATH_MAX * 2];
	char qoutput[PATH_MAX * 2];

	char command[PATH_MAX * 8];

	shell_quote(workdir,
		    qworkdir,
		    sizeof(qworkdir));

	shell_quote(wrapper,
		    qwrapper,
		    sizeof(qwrapper));

	shell_quote(output,
		    qoutput,
		    sizeof(qoutput));

	snprintf(command,
		 sizeof(command),
		 "cd %s && "
		 "PKGEXT='%s' "
		 "PKGDEST=%s "
		 "makepkg "
		 "-f "
		 "--nodeps "
		 "--noextract "
		 "-p %s "
		 "%s",
		 qworkdir,
		 SUFFIX,
		 qoutput,
		 qwrapper,
		 (skip_checks || no_build) ?
		 "--nocheck" :
		 "");

	if (geteuid() != 0) {
		if (run_command(command) != 0)
			die("makepkg failed");

		return;
	}

	/*
	 * Preferred path: runuser.
	 */
	{
		struct passwd *pw;

		pw = lookup_builder();

		if (!pw)
			die("builder user disappeared");

		if (command_exists("/usr/sbin/runuser")) {
			char qcommand[PATH_MAX * 9];
			char qhome[PATH_MAX * 2];
			char run_cmd[PATH_MAX * 12];

			shell_quote(command,
				    qcommand,
				    sizeof(qcommand));

			shell_quote(pw->pw_dir,
				    qhome,
				    sizeof(qhome));

			snprintf(run_cmd,
				 sizeof(run_cmd),
				 "runuser -u %s -- "
				 "env "
				 "HOME=%s "
				 "USER=%s "
				 "LOGNAME=%s "
				 "PATH=/usr/bin:/usr/sbin:/bin:/sbin "
				 "bash -c %s",
				 BUILDER_USER,
				 qhome,
				 BUILDER_USER,
				 BUILDER_USER,
				 qcommand);

			if (run_command(run_cmd) != 0)
				die("makepkg failed as builder");

			return;
		}

		if (command_exists("/bin/su")) {
			char qcommand[PATH_MAX * 9];
			char run_cmd[PATH_MAX * 10];

			shell_quote(command,
				    qcommand,
				    sizeof(qcommand));

			snprintf(run_cmd,
				 sizeof(run_cmd),
				 "su -s /bin/bash %s -c %s",
				 BUILDER_USER,
				 qcommand);

			if (run_command(run_cmd) != 0)
				die("makepkg failed as builder");

			return;
		}

		if (command_exists("/usr/bin/su")) {
			char qcommand[PATH_MAX * 9];
			char run_cmd[PATH_MAX * 10];

			shell_quote(command,
				    qcommand,
				    sizeof(qcommand));

			snprintf(run_cmd,
				 sizeof(run_cmd),
				 "/usr/bin/su -s /bin/bash %s -c %s",
				 BUILDER_USER,
				 qcommand);

			if (run_command(run_cmd) != 0)
				die("makepkg failed as builder");

			return;
		}

		/*
		 * Minimal-LFS fallback: fork + drop privileges directly.
		 */
		{
			pid_t pid;
			int status;

			pid = fork();

			if (pid < 0)
				die("fork failed: %s",
				    strerror(errno));

			if (pid == 0) {
				struct passwd *child_pw;

				child_pw = lookup_builder();

				if (!child_pw)
					_exit(126);

				if (initgroups(BUILDER_USER,
					       child_pw->pw_gid) != 0)
					_exit(126);

				if (setgid(child_pw->pw_gid) != 0)
					_exit(126);

				if (setuid(child_pw->pw_uid) != 0)
					_exit(126);

				execl("/bin/bash",
				      "bash",
				      "-c",
				      command,
				      (char *)NULL);

				_exit(127);
			}

			if (waitpid(pid,
				     &status,
				     0) < 0) {
				die("waitpid failed: %s",
				    strerror(errno));
			}

			if (!WIFEXITED(status) ||
			    WEXITSTATUS(status) != 0) {
				die("makepkg failed as builder");
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Modern create/package                                                     */
/* ------------------------------------------------------------------------- */

static void modern_package(const char *source_dir,
			   const char *pkgbuild,
			   const char *system_override,
			   const struct abi_profile *abi,
			   const char *output,
			   int skip_checks,
			   int no_build)
{
	char name[256];
	char version[256];
	char release[256];

	char workdir[PATH_MAX];
	char temp[PATH_MAX];
	char wrapper[PATH_MAX];
	char output_dir[PATH_MAX];

	int root_build;

	read_srcinfo(source_dir,
		     name,
		     sizeof(name),
		     version,
		     sizeof(version),
		     release,
		     sizeof(release));

	root_build = geteuid() == 0;

	if (root_build) {
		copy_source_tree(source_dir,
				 workdir,
				 sizeof(workdir));
	} else {
		snprintf(workdir,
			 sizeof(workdir),
			 "%s",
			 source_dir);
	}

	snprintf(temp,
		 sizeof(temp),
		 "%s/.packinstall-lfs.%ld",
		 workdir,
		 (long)getpid());

	if (mkdir(temp,
		  0700) != 0) {
		die("cannot create package temporary directory: %s",
		    strerror(errno));
	}

	if (root_build) {
		if (chown(temp,
			  builder_uid(),
			  builder_gid()) != 0) {
			die("cannot chown package temporary directory");
		}

		if (chmod(temp,
			  0700) != 0) {
			die("cannot chmod package temporary directory");
		}
	}

	snprintf(wrapper,
		 sizeof(wrapper),
		 "%s/PKGBUILD",
		 temp);

	/*
	 * If no output was supplied, put the resulting archive beside the
	 * source tree for non-root builds. Root gets a temporary output
	 * directory and the package is copied back afterwards.
	 */
	if (output) {
		absolute_path(output,
			      output_dir,
			      sizeof(output_dir));

		if (!is_directory(output_dir))
			ensure_dir(output_dir);
	} else if (root_build) {
		snprintf(output_dir,
			 sizeof(output_dir),
			 "%s/.packinstall-output-%ld",
			 workdir,
			 (long)getpid());

		if (mkdir(output_dir,
			  0755) != 0) {
			die("cannot create temporary output directory");
		}

		if (chown(output_dir,
			  builder_uid(),
			  builder_gid()) != 0) {
			die("cannot chown output directory");
		}
	} else {
		snprintf(output_dir,
			 sizeof(output_dir),
			 "%s",
			 source_dir);
	}

	if (root_build) {
		if (chown(output_dir,
			  builder_uid(),
			  builder_gid()) != 0) {
			die("cannot chown output directory");
		}
	}

	create_wrapper_pkgbuid(
		workdir,
		pkgbuild,
		temp,
		system_override,
		abi,
		name,
		version,
		release,
		skip_checks,
		no_build);

	execute_makepkg(
		temp,
		wrapper,
		output_dir,
		skip_checks,
		no_build);

	unlink(wrapper);
	rmdir(temp);

	if (root_build && !output) {
		char qout[PATH_MAX * 2];
		char qsource[PATH_MAX * 2];
		char cmd[PATH_MAX * 6];

		shell_quote(output_dir,
			    qout,
			    sizeof(qout));

		shell_quote(source_dir,
			    qsource,
			    sizeof(qsource));

		snprintf(cmd,
			 sizeof(cmd),
			 "cp -a %s/*.kuzpkg.tar.zst %s/ 2>/dev/null",
			 qout,
			 qsource);

		if (run_command(cmd) != 0) {
			warnx("could not copy generated package back to %s",
			      source_dir);
		}

		remove_tree(output_dir);
		remove_tree(workdir);
	}

	printf("packaged %s-%s-%s [%s]\n",
	       name,
	       version,
	       release,
	       abi->name);
}

/* ------------------------------------------------------------------------- */
/* Legacy create                                                             */
/* ------------------------------------------------------------------------- */

static void create_legacy(const char *stage,
			  const char *name,
			  const char *version,
			  const char *output)
{
	struct stat st;

	char qstage[PATH_MAX * 2];
	char qout[PATH_MAX * 2];
	char cmd[PATH_MAX * 4];
	char pkginfo[PATH_MAX];

	if (stat(stage,
		 &st) != 0 ||
	    !S_ISDIR(st.st_mode)) {
		die("stage directory does not exist: %s",
		    stage);
	}

	if (!output) {
		static char generated[PATH_MAX];

		snprintf(generated,
			 sizeof(generated),
			 "%s-%s-1%s",
			 name,
			 version,
			 SUFFIX);

		output = generated;
	}

	/*
	 * Legacy archives contain PKGINFO, never .LFSINFO.
	 */
	snprintf(pkginfo,
		 sizeof(pkginfo),
		 "%s/%s",
		 stage,
		 PKGINFO_NAME);

	{
		FILE *fp;

		fp = fopen(pkginfo,
			   "w");

		if (!fp)
			die("cannot create %s: %s",
			    pkginfo,
			    strerror(errno));

		fprintf(fp,
			"format=packinstall-lfs/%s\n"
			"pkgname=%s\n"
			"pkgver=%s\n"
			"pkgrel=1\n"
			"abi=native\n"
			"libdir=/usr/lib\n"
			"arch=native\n",
			VERSION,
			name,
			version);

		fclose(fp);
	}

	shell_quote(stage,
		    qstage,
		    sizeof(qstage));

	shell_quote(output,
		    qout,
		    sizeof(qout));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -C %s "
		 "-cf %s .",
		 qstage,
		 qout);

	if (run_command(cmd) != 0) {
		unlink(pkginfo);
		die("tar failed");
	}

	unlink(pkginfo);

	printf("created legacy package %s\n",
	       output);
}

/* ------------------------------------------------------------------------- */
/* Archive validation                                                        */
/* ------------------------------------------------------------------------- */

static int safe_member(const char *path)
{
	if (!path || !*path)
		return 0;

	if (path[0] == '/')
		return 0;

	if (!strcmp(path, ".") ||
	    !strcmp(path, "./"))
		return 1;

	if (!strcmp(path, ".."))
		return 0;

	if (!strncmp(path, "../", 3))
		return 0;

	if (strstr(path, "/../"))
		return 0;

	if (strstr(path, "/./"))
		return 0;

	return 1;
}

static void validate_archive(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	FILE *fp;
	char line[PATH_MAX];

	shell_quote(package,
		    qpackage,
		    sizeof(qpackage));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -tf %s",
		 qpackage);

	fp = popen(cmd,
		   "r");

	if (!fp)
		die("cannot inspect %s",
		    package);

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		line[strcspn(line,
			     "\r\n")] = '\0';

		if (!safe_member(line)) {
			pclose(fp);

			die("unsafe archive member: %s",
			    line);
		}
	}

	if (pclose(fp) != 0)
		die("cannot inspect archive: %s",
		    package);
}

/* ------------------------------------------------------------------------- */
/* Read PKGINFO                                                              */
/* ------------------------------------------------------------------------- */

static void package_info(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	shell_quote(package,
		    qpackage,
		    sizeof(qpackage));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xOf %s "
		 "./%s 2>/dev/null",
		 qpackage,
		 PKGINFO_NAME);

	if (run_command(cmd) != 0)
		die("%s has no PKGINFO",
		    package);
}

/* ------------------------------------------------------------------------- */
/* Install                                                                    */
/* ------------------------------------------------------------------------- */

static void install_package(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char qroot[PATH_MAX * 2];
	char qtmp[PATH_MAX * 2];

	char cmd[PATH_MAX * 5];
	char tmp[PATH_MAX];

	char name[256] = {0};
	char version[256] = {0};
	char release[256] = {0};
	char abi[128] = {0};

	char db[PATH_MAX];
	char files[PATH_MAX];

	FILE *fp;
	char line[1024];

	validate_archive(package);

	shell_quote(package,
		    qpackage,
		    sizeof(qpackage));

	shell_quote(root_dir,
		    qroot,
		    sizeof(qroot));

	snprintf(tmp,
		 sizeof(tmp),
		 "/tmp/packinstall-lfs-pkginfo-%ld",
		 (long)getpid());

	shell_quote(tmp,
		    qtmp,
		    sizeof(qtmp));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xOf %s "
		 "./%s > %s 2>/dev/null",
		 qpackage,
		 PKGINFO_NAME,
		 qtmp);

	if (run_command(cmd) != 0)
		die("package does not contain PKGINFO");

	fp = fopen(tmp,
		   "r");

	if (!fp)
		die("cannot read PKGINFO");

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		if (sscanf(line,
			   "pkgname=%255s",
			   name) == 1)
			continue;

		if (sscanf(line,
			   "pkgver=%255s",
			   version) == 1)
			continue;

		if (sscanf(line,
			   "pkgrel=%255s",
			   release) == 1)
			continue;

		if (sscanf(line,
			   "abi=%127s",
			   abi) == 1)
			continue;
	}

	fclose(fp);
	unlink(tmp);

	if (!name[0] ||
	    !version[0]) {
		die("PKGINFO is missing pkgname/pkgver");
	}

	if (!release[0])
		snprintf(release,
			 sizeof(release),
			 "1");

	if (!abi[0])
		snprintf(abi,
			 sizeof(abi),
			 "native");

	if (snprintf(db,
		     sizeof(db),
		     "%s%s/%s/%s/%s",
		     root_dir,
		     DB_REL,
		     name,
		     version,
		     abi) >= (int)sizeof(db)) {
		die("package database path is too long");
	}

	if (snprintf(files,
		     sizeof(files),
		     "%s/files",
		     db) >= (int)sizeof(files)) {
		die("package database path is too long");
	}

	ensure_dir(db);

	/*
	 * Don't install PKGINFO itself into the target filesystem root.
	 * Keep package metadata in the package database.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xpf %s "
		 "-C %s "
		 "--exclude=./%s "
		 "--exclude=./etc/packinstall-lfs "
		 "--no-same-owner "
		 "--no-same-permissions",
		 qpackage,
		 qroot,
		 PKGINFO_NAME);

	if (run_command(cmd) != 0)
		die("failed to install %s",
		    package);

	/*
	 * Generate a file manifest from the archive.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -tf %s "
		 "| sed '/^%s$/d' > %s",
		 qpackage,
		 PKGINFO_NAME,
		 qtmp);

	if (run_command(cmd) == 0) {
		if (rename(tmp,
			   files) != 0) {
			warnx("cannot save package manifest: %s",
			      strerror(errno));
		}
	} else {
		fp = fopen(files,
			   "w");

		if (fp) {
			fprintf(fp,
				"# package %s-%s-%s [%s]\n",
				name,
				version,
				release,
				abi);

			fclose(fp);
		}
	}

	printf("installed %s-%s-%s [%s]\n",
	       name,
	       version,
	       release,
	       abi);
}

/* ------------------------------------------------------------------------- */
/* Remove                                                                    */
/* ------------------------------------------------------------------------- */

static void remove_package(const char *name,
			   const char *version,
			   const char *abi_name)
{
	char db[PATH_MAX];
	char files[PATH_MAX];
	char path[PATH_MAX];

	FILE *fp;

	if (!abi_name)
		abi_name = "native";

	if (snprintf(db,
		     sizeof(db),
		     "%s%s/%s/%s/%s",
		     root_dir,
		     DB_REL,
		     name,
		     version,
		     abi_name) >= (int)sizeof(db)) {
		die("package database path is too long");
	}

	if (snprintf(files,
		     sizeof(files),
		     "%s/files",
		     db) >= (int)sizeof(files)) {
		die("package database path is too long");
	}

	fp = fopen(files,
		   "r");

	if (!fp)
		die("package %s-%s [%s] is not installed",
		    name,
		    version,
		    abi_name);

	while (fgets(path,
		     sizeof(path),
		     fp)) {
		char full[PATH_MAX];
		struct stat st;

		path[strcspn(path,
			     "\r\n")] = '\0';

		if (path[0] == '#')
			continue;

		if (!safe_member(path))
			continue;

		if (strlen(root_dir) +
		    1 +
		    strlen(path) >= PATH_MAX)
			continue;

		strcpy(full,
		       root_dir);

		strcat(full,
		       "/");

		strcat(full,
		       path);

		if (lstat(full,
			  &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			(void)rmdir(full);
		} else if (unlink(full) != 0 &&
			   errno != ENOENT) {
			fprintf(stderr,
				"packinstall-lfs: cannot remove %s: %s\n",
				full,
				strerror(errno));
		}
	}

	fclose(fp);

	unlink(files);
	rmdir(db);

	printf("removed %s-%s [%s]\n",
	       name,
	       version,
	       abi_name);
}

/* ------------------------------------------------------------------------- */
/* List                                                                      */
/* ------------------------------------------------------------------------- */

static void list_packages(void)
{
	char base[PATH_MAX];
	char qbase[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	if (snprintf(base,
		     sizeof(base),
		     "%s%s",
		     root_dir,
		     DB_REL) >= (int)sizeof(base)) {
		die("package database path is too long");
	}

	shell_quote(base,
		    qbase,
		    sizeof(qbase));

	snprintf(cmd,
		 sizeof(cmd),
		 "find %s "
		 "-mindepth 3 "
		 "-maxdepth 3 "
		 "-type d "
		 "-printf '%%P\\n' "
		 "2>/dev/null",
		 qbase);

	(void)run_command(cmd);
}

/* ------------------------------------------------------------------------- */
/* Find PKGBUILD                                                             */
/* ------------------------------------------------------------------------- */

static void resolve_pkgbuild(const char *dir,
			     const char *requested,
			     char *output,
			     size_t output_size)
{
	const char *name;

	if (requested) {
		if (requested[0] == '/') {
			if (strlen(requested) >= output_size)
				die("PKGBUILD path is too long");

			strcpy(output, requested);
			return;
		}

		if (strlen(dir) +
		    1 +
		    strlen(requested) >= output_size) {
			die("PKGBUILD path is too long");
		}

		snprintf(output,
			 output_size,
			 "%s/%s",
			 dir,
			 requested);

		return;
	}

	name = "PKGBUILD";

	if (strlen(dir) +
	    1 +
	    strlen(name) >= output_size)
		die("PKGBUILD path is too long");

	snprintf(output,
		 output_size,
		 "%s/%s",
		 dir,
		 name);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *command;
	const char *root;

	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}

	command = argv[1];

	root = arg_value(argc,
			 argv,
			 "--root");

	if (root)
		root_dir = root;

	/*
	 * Modern create and package are the same packaging path.
	 */
	if (!strcmp(command, "create") ||
	    !strcmp(command, "package")) {
		const char *dir;
		const char *requested_pkgbuild;
		const char *system;
		const char *abi_name;
		const char *output;

		const struct abi_profile *abi;

		char absdir[PATH_MAX];
		char pkgbuild[PATH_MAX];

		dir = arg_value(argc,
				argv,
				"--dir");

		/*
		 * Also allow:
		 *
		 *   packinstall-lfs create /sources/kuzpkg
		 *
		 * rather than requiring --dir.
		 */
		if (!dir) {
			if (argc >= 3 &&
			    argv[2][0] != '-' &&
			    strcmp(argv[2], "--dir") &&
			    strcmp(argv[2], "--abi") &&
			    strcmp(argv[2], "--output")) {
				dir = argv[2];
			} else {
				dir = ".";
			}
		}

		if (!dir || !*dir)
			dir = ".";

		if (!is_directory(dir))
			die("source directory does not exist: %s",
			    dir);

		absolute_path(dir,
			      absdir,
			      sizeof(absdir));

		requested_pkgbuild =
			arg_value(argc,
				  argv,
				  "--buildscript");

		resolve_pkgbuild(absdir,
				 requested_pkgbuild,
				 pkgbuild,
				 sizeof(pkgbuild));

		if (!is_file(pkgbuild))
			die("build script not found: %s",
			    pkgbuild);

		abi_name =
			arg_value(argc,
				  argv,
				  "--abi");

		if (abi_name) {
			abi = find_abi(abi_name);

			if (!abi)
				die("unknown ABI profile: %s",
				    abi_name);
		} else {
			abi = detect_default_abi();
		}

		system =
			arg_value(argc,
				  argv,
				  "--build-system");

		output =
			arg_value(argc,
				  argv,
				  "--output");

		if (geteuid() == 0)
			ensure_builder();

		modern_package(
			absdir,
			pkgbuild,
			system,
			abi,
			output,
			has_opt(argc,
				argv,
				"--skip-checks"),
			has_opt(argc,
				argv,
				"--no-build"));

		return EXIT_SUCCESS;
	}

	/*
	 * Explicitly legacy. It is NOT the default create mode anymore.
	 */
	if (!strcmp(command,
		    "create-legacy")) {
		const char *stage;
		const char *name;
		const char *version;
		const char *output;

		if (argc < 5) {
			fprintf(stderr,
				"packinstall-lfs: create-legacy requires:\n"
				"  packinstall-lfs create-legacy "
				"STAGE NAME VERSION [OUTPUT]\n");

			return EXIT_FAILURE;
		}

		stage = argv[2];
		name = argv[3];
		version = argv[4];

		output =
			argc >= 6 ?
			argv[5] :
			NULL;

		create_legacy(stage,
			      name,
			      version,
			      output);

		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "install")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}

		install_package(argv[2]);

		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "remove")) {
		const char *abi;

		if (argc < 4) {
			usage();
			return EXIT_FAILURE;
		}

		abi =
			arg_value(argc,
				  argv,
				  "--abi");

		remove_package(
			argv[2],
			argv[3],
			abi);

		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "list")) {
		list_packages();
		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "info")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}

		package_info(argv[2]);

		return EXIT_SUCCESS;
	}

	fprintf(stderr,
		"packinstall-lfs: unknown command: %s\n\n",
		command);

	usage();

	return EXIT_FAILURE;
}
