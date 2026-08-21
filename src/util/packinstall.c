/*
 * packinstall.c - build, stage, package and optionally install a source tree.
 *
 * This utility is intended to replace the shell packInstall helper used by
 * bootstrap/LFS-style builds. It deliberately uses the native build tools
 * instead of requiring a PKGBUILD for every upstream build system.
 *
 * Supported build systems:
 *   Autotools, Make, Ninja, Meson, CMake, Cargo, Python,
 *   Perl, Go, Ruby/RubyGems and Bundler.
 *
 * Copyright (C) 2026 Kuznix
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 or (at your option) any
 * later version.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/utsname.h>

#define REL "1"

static int exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static int is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

static int run_command(const char *command)
{
	int status;

	fprintf(stderr, "packinstall: %s\n", command);

	status = system(command);

	if(status == -1) {
		fprintf(stderr,
			"packinstall: cannot execute command: %s\n",
			strerror(errno));
		return -1;
	}

	if(WIFEXITED(status))
		return WEXITSTATUS(status);

	if(WIFSIGNALED(status)) {
		fprintf(stderr,
			"packinstall: command terminated by signal %d\n",
			WTERMSIG(status));
		return 128;
	}

	return 128;
}

/* Quote one string for the POSIX shell. */
static char *shell_quote(const char *s)
{
	size_t n = 2;
	char *out;
	char *p;
	const char *q;

	for(q = s; *q; q++)
		n += (*q == '\'') ? 4 : 1;

	out = malloc(n + 1);
	if(!out)
		return NULL;

	p = out;
	*p++ = '\'';

	for(q = s; *q; q++) {
		if(*q == '\'') {
			memcpy(p, "'\\''", 4);
			p += 4;
		} else {
			*p++ = *q;
		}
	}

	*p++ = '\'';
	*p = '\0';

	return out;
}

static int clean_stage(const char *stage)
{
	char *q;
	char *cmd;
	int ret;

	q = shell_quote(stage);
	if(!q)
		return -1;

	if(strcmp(stage, "/") == 0 ||
	   strcmp(stage, ".") == 0 ||
	   strcmp(stage, "..") == 0) {
		fprintf(stderr,
			"packinstall: refusing to remove unsafe staging directory '%s'\n",
			stage);
		free(q);
		return -1;
	}

	if(asprintf(&cmd,
		    "rm -rf -- %s && mkdir -p -- %s",
		    q, q) < 0) {
		free(q);
		return -1;
	}

	ret = run_command(cmd);

	free(q);
	free(cmd);

	return ret;
}

static const char *detect_arch(void)
{
	static struct utsname u;

	if(uname(&u) != 0)
		return "any";

	if(strcmp(u.machine, "x86_64") == 0)
		return "x86_64";

	if(strcmp(u.machine, "i686") == 0 ||
	   strcmp(u.machine, "i386") == 0 ||
	   strcmp(u.machine, "i486") == 0 ||
	   strcmp(u.machine, "i586") == 0)
		return "i686";

	return u.machine;
}

/*
 * Extract a package version from the source tree directory name.
 *
 * Examples:
 *   bash-5.3.0       -> 5.3.0
 *   foo_1.2.3        -> 1.2.3
 *   expect5.45       -> 5.45
 *   tcl8.6.16        -> 8.6.16
 *   unzip610         -> 6.10
 *   docbook-xml      -> 4.5
 */
static char *package_version(const char *tree)
{
	const char *b = base_name(tree);
	const char *p;
	char *v;
	char *dash;

	if(strncmp(b, "expect", 6) == 0 ||
	   strncmp(b, "tcl", 3) == 0) {
		p = b;

		while(*p && (*p < '0' || *p > '9'))
			p++;

		return strdup(*p ? p : "0");
	}

	if(strncmp(b, "unzip", 5) == 0) {
		p = b + 5;

		if(p[0] &&
		   p[1] &&
		   p[0] >= '0' &&
		   p[0] <= '9' &&
		   p[1] >= '0' &&
		   p[1] <= '9') {
			if(asprintf(&v,
				    "%c.%c%s",
				    p[0],
				    p[1],
				    p + 2) >= 0)
				return v;
		}
	}

	if(strcmp(b, "docbook-xml") == 0)
		return strdup("4.5");

	p = strrchr(b, '-');
	dash = strrchr(b, '_');

	if(dash && (!p || dash > p))
		p = dash;

	if(p &&
	   p[1] &&
	   ((p[1] >= '0' && p[1] <= '9') || p[1] == 'v')) {
		p++;
	} else {
		p = b;

		while(*p && (*p < '0' || *p > '9'))
			p++;
	}

	v = strdup(*p ? p : "0");
	if(!v)
		return NULL;

	for(char *q = v; *q; q++) {
		if(*q == '_')
			*q = '.';
	}

	return v;
}

static char *package_name(const char *tree)
{
	const char *b = base_name(tree);
	const char *p = b;
	char *name;

	while(*p >= '0' && *p <= '9')
		p++;

	if(p - b >= 2 &&
	   p - b <= 4 &&
	   *p == '-') {
		b = p + 1;
	}

	name = strdup(b);
	if(!name)
		return NULL;

	for(size_t i = 1; name[i]; i++) {
		if(name[i] == '-' || name[i] == '_') {
			const char *q = name + i + 1;

			if((*q >= '0' && *q <= '9') || *q == 'v') {
				name[i] = '\0';
				break;
			}
		}
	}

	return name;
}

/*
 * Locate the first gemspec in the source tree.
 *
 * The returned string must be freed by the caller.
 */
static char *find_gemspec(void)
{
	FILE *p;
	char line[PATH_MAX];

	p = popen(
		"find . -maxdepth 1 -type f -name '*.gemspec' "
		"-print -quit",
		"r");

	if(!p)
		return NULL;

	if(!fgets(line, sizeof(line), p)) {
		pclose(p);
		return NULL;
	}

	pclose(p);

	line[strcspn(line, "\n")] = '\0';

	if(line[0] == '\0')
		return NULL;

	if(line[0] == '.' &&
	   line[1] == '/') {
		return strdup(line + 2);
	}

	return strdup(line);
}

/*
 * Locate the first .gem package.
 *
 * The returned string must be freed by the caller.
 */
static char *find_gem_package(void)
{
	FILE *p;
	char line[PATH_MAX];

	p = popen(
		"find . -maxdepth 1 -type f -name '*.gem' "
		"-print -quit",
		"r");

	if(!p)
		return NULL;

	if(!fgets(line, sizeof(line), p)) {
		pclose(p);
		return NULL;
	}

	pclose(p);

	line[strcspn(line, "\n")] = '\0';

	if(line[0] == '\0')
		return NULL;

	if(line[0] == '.' &&
	   line[1] == '/') {
		return strdup(line + 2);
	}

	return strdup(line);
}

/*
 * Build and stage a Ruby gem.
 *
 * RubyGems itself performs the actual build from the gemspec and then
 * installs the resulting local gem into the package staging root.
 */
static int build_ruby_gem(const char *stage)
{
	char *qstage;
	char *gemspec = NULL;
	char *qgemspec = NULL;
	char *gem = NULL;
	char *qgem = NULL;
	char *cmd = NULL;
	int ret = -1;

	qstage = shell_quote(stage);
	if(!qstage)
		return -1;

	gemspec = find_gemspec();

	if(gemspec) {
		qgemspec = shell_quote(gemspec);

		if(!qgemspec)
			goto out;

		if(asprintf(&cmd,
			    "gem build %s",
			    qgemspec) < 0)
			goto out;

		ret = run_command(cmd);

		free(cmd);
		cmd = NULL;

		free(qgemspec);
		qgemspec = NULL;

		if(ret != 0)
			goto out;

		gem = find_gem_package();
	} else {
		gem = find_gem_package();
	}

	/*
	 * If there is no gemspec and no existing .gem, this is not
	 * a RubyGems project that packinstall can package.
	 */
	if(!gem) {
		fprintf(stderr,
			"packinstall: no Ruby gemspec or .gem package found\n");
		ret = 1;
		goto out;
	}

	qgem = shell_quote(gem);
	if(!qgem)
		goto out;

	/*
	 * Install into the staging root rather than the host Ruby
	 * installation.
	 *
	 * --install-dir:
	 *   /usr/lib/ruby/gems inside the package.
	 *
	 * --bindir:
	 *   Ruby gem executables become /usr/bin programs.
	 *
	 * --ignore-dependencies:
	 *   Dependencies are handled by the package manager rather
	 *   than pulling packages into the temporary staging tree.
	 *
	 * --no-document:
	 *   Avoid installing generated RI/RDoc documentation.
	 */
	if(asprintf(&cmd,
		    "mkdir -p -- %s/usr/lib/ruby/gems "
		    "&& mkdir -p -- %s/usr/bin "
		    "&& gem install --local %s "
		    "--install-dir %s/usr/lib/ruby/gems "
		    "--bindir %s/usr/bin "
		    "--ignore-dependencies "
		    "--no-document "
		    "--force",
		    qstage,
		    qstage,
		    qgem,
		    qstage,
		    qstage) < 0)
		goto out;

	ret = run_command(cmd);

out:
	free(cmd);
	free(gem);
	free(qgem);
	free(gemspec);
	free(qgemspec);
	free(qstage);

	return ret;
}

/*
 * Build/stage all supported project types.
 */
static int build_project(const char *src, const char *stage)
{
	char *qstage = shell_quote(stage);
	char *cmd = NULL;
	int ret = -1;

	if(!qstage)
		return -1;

	/*
	 * Autotools
	 */
	if(exists("configure")) {
		if(asprintf(&cmd,
			    "./configure --prefix=/usr "
			    "&& make "
			    "&& DESTDIR=%s make install",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Meson
	 */
	if(exists("meson.build")) {
		if(asprintf(&cmd,
			    "meson setup build "
			    "--prefix=/usr "
			    "--buildtype=plain "
			    "&& meson compile -C build "
			    "&& DESTDIR=%s meson install -C build",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * CMake
	 */
	if(exists("CMakeLists.txt")) {
		if(asprintf(&cmd,
			    "cmake -S . -B build "
			    "-DCMAKE_BUILD_TYPE=Release "
			    "-DCMAKE_INSTALL_PREFIX=/usr "
			    "&& cmake --build build "
			    "&& DESTDIR=%s cmake --install build",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Ninja
	 */
	if(exists("build.ninja")) {
		if(asprintf(&cmd,
			    "ninja "
			    "&& DESTDIR=%s ninja install",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Rust / Cargo
	 */
	if(exists("Cargo.toml")) {
		if(asprintf(&cmd,
			    "mkdir -p %s/usr "
			    "&& cargo install --path . "
			    "--root %s/usr --locked",
			    qstage,
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Ruby / RubyGems
	 *
	 * Checked before generic Make because a gem project may contain
	 * a Rakefile or Makefile as well.
	 */
	if(exists("*.gemspec")) {
		ret = build_ruby_gem(stage);

		if(ret == 0)
			goto out;
	}

	{
		char *gemspec = find_gemspec();

		if(gemspec) {
			free(gemspec);

			ret = build_ruby_gem(stage);

			if(ret == 0)
				goto out;
		}
	}

	/*
	 * Bundler-only Ruby project.
	 *
	 * This is mostly useful for applications that provide a Gemfile
	 * but do not provide a gemspec.
	 */
	if(exists("Gemfile") && !exists("*.gemspec")) {
		if(asprintf(&cmd,
			    "mkdir -p %s/usr/lib/ruby/vendor_ruby "
			    "&& bundle config set --local path %s/usr/lib/ruby/vendor_ruby "
			    "&& bundle config set --local without 'development test' "
			    "&& bundle install --jobs 1 --retry 3",
			    qstage,
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Ruby Rake fallback.
	 */
	if(exists("Rakefile")) {
		if(asprintf(&cmd,
			    "rake "
			    "&& DESTDIR=%s rake install",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Python
	 */
	if(exists("pyproject.toml") ||
	   exists("setup.py") ||
	   exists("setup.cfg")) {
		if(asprintf(&cmd,
			    "python3 -m pip install . "
			    "--root %s "
			    "--prefix /usr "
			    "--no-deps "
			    "--no-build-isolation",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Perl Module::Build
	 */
	if(exists("Build.PL")) {
		if(asprintf(&cmd,
			    "perl Build.PL "
			    "--destdir %s "
			    "--install_base /usr "
			    "&& ./Build "
			    "&& ./Build install",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Perl MakeMaker
	 */
	if(exists("Makefile.PL")) {
		if(asprintf(&cmd,
			    "perl Makefile.PL PREFIX=/usr "
			    "&& make "
			    "&& DESTDIR=%s make install",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Go
	 */
	if(exists("go.mod")) {
		if(asprintf(&cmd,
			    "mkdir -p %s/usr/bin "
			    "&& GOBIN=%s/usr/bin go install ./...",
			    qstage,
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/*
	 * Generic Make
	 */
	if(exists("Makefile") ||
	   exists("makefile") ||
	   exists("GNUmakefile")) {
		if(asprintf(&cmd,
			    "make "
			    "&& DESTDIR=%s make install",
			    qstage) < 0)
			goto out;

		ret = run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	fprintf(stderr,
		"packinstall: no supported build system succeeded in %s\n",
		src);

	ret = 1;

out:
	free(cmd);
	free(qstage);

	return ret;
}

static int write_pkgbuild(const char *work,
			  const char *stage,
			  const char *name,
			  const char *version,
			  const char *arch)
{
	char *qstage = shell_quote(stage);
	char *cmd;
	FILE *f;
	int ret = -1;

	if(!qstage)
		goto out;

	if(asprintf(&cmd,
		    "%s/PKGBUILD",
		    work) < 0)
		goto out;

	f = fopen(cmd, "w");

	if(!f) {
		fprintf(stderr,
			"packinstall: cannot write %s: %s\n",
			cmd,
			strerror(errno));
		free(cmd);
		goto out;
	}

	fprintf(f,
		"pkgname='%s'\n"
		"pkgver='%s'\n"
		"pkgrel='%s'\n"
		"pkgdesc='%s'\n"
		"arch=('%s')\n"
		"source=()\n"
		"sha256sums=()\n"
		"\n"
		"package() {\n"
		"    cp -a -- %s/. \"$pkgdir\"/\n"
		"}\n",
		name,
		version,
		REL,
		name,
		arch,
		qstage);

	fclose(f);

	free(cmd);

	ret = 0;

out:
	free(qstage);

	return ret;
}

static int package_and_install(const char *stage,
			       const char *name,
			       const char *version,
			       const char *arch,
			       int do_install)
{
	char templ[] = "/tmp/packinstall.XXXXXX";
	char *work;
	char *qwork;
	char *cmd = NULL;
	int ret;

	work = mkdtemp(templ);

	if(!work) {
		fprintf(stderr,
			"packinstall: mkdtemp: %s\n",
			strerror(errno));
		return 1;
	}

	if(write_pkgbuild(work,
			  stage,
			  name,
			  version,
			  arch) != 0)
		return 1;

	qwork = shell_quote(work);

	if(!qwork)
		return 1;

	if(asprintf(&cmd,
		    "cd %s && "
		    "makepkg -f --skipinteg --nodeps",
		    qwork) < 0) {
		free(qwork);
		return 1;
	}

	ret = run_command(cmd);

	free(cmd);
	free(qwork);

	if(ret != 0)
		return ret;

	fprintf(stdout,
		"packinstall: package built in %s\n",
		work);

	if(do_install) {
		char *q;
		FILE *p;
		char pkg[PATH_MAX];

		if(asprintf(&q,
			    "find %s "
			    "-maxdepth 1 "
			    "-type f "
			    "-name '*.pkg.tar.*' "
			    "-print -quit",
			    work) < 0)
			return 1;

		p = popen(q, "r");
		free(q);

		if(!p)
			return 1;

		if(!fgets(pkg, sizeof(pkg), p)) {
			pclose(p);
			return 1;
		}

		pclose(p);

		pkg[strcspn(pkg, "\n")] = '\0';

		q = shell_quote(pkg);

		if(!q)
			return 1;

		if(asprintf(&cmd,
			    "kuzpkg -U --noconfirm %s",
			    q) < 0) {
			free(q);
			return 1;
		}

		ret = run_command(cmd);

		free(cmd);
		free(q);
	}

	return ret;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s <source-tree> <staging-dir> [--no-install]\n"
		"\n"
		"Build, stage and package a source tree.\n"
		"\n"
		"Detected systems:\n"
		"  Autotools\n"
		"  Make\n"
		"  Ninja\n"
		"  Meson\n"
		"  CMake\n"
		"  Cargo\n"
		"  Python\n"
		"  Perl\n"
		"  Go\n"
		"  Ruby/RubyGems\n"
		"  Bundler\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *src;
	const char *stage;
	const char *arch;

	char *name = NULL;
	char *version = NULL;

	int do_install = 1;

	char cwd[PATH_MAX];

	int ret;

	if(argc < 3 || argc > 4) {
		usage(argv[0]);
		return 2;
	}

	src = argv[1];
	stage = argv[2];

	if(argc == 4 &&
	   strcmp(argv[3], "--no-install") == 0) {
		do_install = 0;
	} else if(argc == 4) {
		usage(argv[0]);
		return 2;
	}

	if(!is_dir(src)) {
		fprintf(stderr,
			"packinstall: source tree '%s' "
			"does not exist or is not a directory\n",
			src);
		return 1;
	}

	if(!getcwd(cwd, sizeof(cwd))) {
		fprintf(stderr,
			"packinstall: getcwd: %s\n",
			strerror(errno));
		return 1;
	}

	if(chdir(src) != 0) {
		fprintf(stderr,
			"packinstall: cannot enter '%s': %s\n",
			src,
			strerror(errno));
		return 1;
	}

	name = package_name(src);
	version = package_version(src);
	arch = detect_arch();

	if(!name || !version) {
		fprintf(stderr,
			"packinstall: out of memory\n");

		free(name);
		free(version);

		chdir(cwd);

		return 1;
	}

	fprintf(stderr,
		"packinstall: package=%s "
		"version=%s "
		"arch=%s\n",
		name,
		version,
		arch);

	if(clean_stage(stage) != 0)
		goto fail;

	ret = build_project(src, stage);

	if(ret != 0)
		goto fail;

	/*
	 * Avoid replacing the system's generated Info directory entry.
	 */
	{
		char *q = shell_quote(stage);
		char *cmd;

		if(!q ||
		   asprintf(&cmd,
			    "rm -f -- %s/usr/share/info/dir",
			    q) < 0) {
			free(q);
			goto fail;
		}

		ret = run_command(cmd);

		free(q);
		free(cmd);

		if(ret != 0)
			goto fail;
	}

	ret = package_and_install(stage,
				  name,
				  version,
				  arch,
				  do_install);

	if(ret != 0)
		goto fail;

	free(name);
	free(version);

	chdir(cwd);

	return 0;

fail:
	fprintf(stderr,
		"packinstall: failed\n");

	free(name);
	free(version);

	chdir(cwd);

	return 1;
}
