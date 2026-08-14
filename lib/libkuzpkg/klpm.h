/*
 * klpm.h
 *
 *  Copyright (C) 2026 Kuznix
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *  Copyright (c) 2005 by Aurelien Foret <orelien@chez.com>
 *  Copyright (c) 2005 by Christian Hamar <krics@linuxforum.hu>
 *  Copyright (c) 2005, 2006 by Miklos Vajna <vmiklos@frugalware.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


/** @mainpage klpm
 *
 * libkuzpkg is a package management library, primarily used by kuzpkg.
 */

#ifndef KUZPKG_H
#define KUZPKG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>   /* bool */
#include <stdint.h>    /* int64_t */
#include <sys/types.h> /* off_t */
#include <stdarg.h>    /* va_list */

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

#include <klpm_list.h>

/** @addtogroup libkuzpkg The libkuzpkg Public API
 *
 *
 *
 * libkuzpkg is a package management library, primarily used by kuzpkg.
 * For ease of access, the libkuzpkg manual has been split up into several sections.
 *
 * @section see_also See Also
 * \b libkuzpkg_list(3),
 * \b libkuzpkg_cb(3),
 * \b libkuzpkg_databases(3),
 * \b libkuzpkg_depends(3),
 * \b libkuzpkg_errors(3),
 * \b libkuzpkg_files(3),
 * \b libkuzpkg_groups(3),
 * \b libkuzpkg_handle(3),
 * \b libkuzpkg_log(3),
 * \b libkuzpkg_misc(3),
 * \b libkuzpkg_options(3),
 * \b libkuzpkg_packages(3),
 * \b libkuzpkg_sig(3),
 * \b libkuzpkg_trans(3)
 * @{
 */

/*
 * Opaque Structures
 */

/** The libkuzpkg context handle.
 *
 * This struct represents an instance of libkuzpkg.
 * @ingroup libkuzpkg_handle
 */
typedef struct _klpm_handle_t klpm_handle_t;

/** A database.
 *
 * A database is a container that stores metadata about packages.
 *
 * A database can be located on the local filesystem or on a remote server.
 *
 * To use a database, it must first be registered via \link klpm_register_syncdb \endlink.
 * If the database is already present in dbpath then it will be usable. Otherwise,
 * the database needs to be downloaded using \link klpm_db_update \endlink. Even if the
 * source of the database is the local filesystem.
 *
 * After this, the database can be used to query packages and groups. Any packages or groups
 * from the database will continue to be owned by the database and do not need to be freed by
 * the user. They will be freed when the database is unregistered.
 *
 * Databases are automatically unregistered when the \link klpm_handle_t \endlink is released.
 * @ingroup libkuzpkg_databases
 */
typedef struct _klpm_db_t klpm_db_t;


/** A package.
 *
 * A package can be loaded from disk via \link klpm_pkg_load \endlink or retrieved from a database.
 * Packages from databases are automatically freed when the database is unregistered. Packages loaded
 * from a file must be freed manually.
 *
 * Packages can then be queried for metadata or added to a transaction
 * to be added or removed from the system.
 * @ingroup libkuzpkg_packages
 */
typedef struct _klpm_pkg_t klpm_pkg_t;

/** The extended data type used to store non-standard package data fields
 * @ingroup libkuzpkg_packages
 */
typedef struct _klpm_pkg_xdata_t {
	char *name;
	char *value;
} klpm_pkg_xdata_t;

/** The time type used by libkuzpkg. Represents a unix time stamp
 * @ingroup libkuzpkg_misc */
typedef int64_t klpm_time_t;

/** @addtogroup libkuzpkg_files Files
 * @brief Functions for package files
 * @{
 */

/** File in a package */
typedef struct _klpm_file_t {
       /** Name of the file */
       char *name;
       /** Size of the file */
       off_t size;
       /** The file's permissions */
       mode_t mode;
} klpm_file_t;

/** Package filelist container */
typedef struct _klpm_filelist_t {
       /** Amount of files in the array */
       size_t count;
       /** An array of files */
       klpm_file_t *files;
} klpm_filelist_t;

/** Local package or package file backup entry */
typedef struct _klpm_backup_t {
       /** Name of the file (without .pacsave extension) */
       char *name;
       /** Hash of the filename (used internally) */
       char *hash;
} klpm_backup_t;

/** Determines whether a package filelist contains a given path.
 * The provided path should be relative to the install root with no leading
 * slashes, e.g. "etc/localtime". When searching for directories, the path must
 * have a trailing slash.
 * @param filelist a pointer to a package filelist
 * @param path the path to search for in the package
 * @return a pointer to the matching file or NULL if not found
 */
klpm_file_t *klpm_filelist_contains(const klpm_filelist_t *filelist, const char *path);

/* End of libkuzpkg_files */
/** @} */


/** @addtogroup libkuzpkg_groups Groups
 * @brief Functions for package groups
 * @{
 */

/** Package group */
typedef struct _klpm_group_t {
	/** group name */
	char *name;
	/** list of klpm_pkg_t packages */
	klpm_list_t *packages;
} klpm_group_t;

/** Find group members across a list of databases.
 * If a member exists in several databases, only the first database is used.
 * IgnorePkg is also handled.
 * @param dbs the list of klpm_db_t *
 * @param name the name of the group
 * @return the list of klpm_pkg_t * (caller is responsible for klpm_list_free)
 */
klpm_list_t *klpm_find_group_pkgs(klpm_list_t *dbs, const char *name);

/* End of libkuzpkg_groups */
/** @} */


/** @addtogroup libkuzpkg_errors Error Codes
 * Error codes returned by libkuzpkg.
 * @{
 */

/** libkuzpkg's error type */
typedef enum _klpm_errno_t {
	/** No error */
	KUZPKG_ERR_OK = 0,
	/** Failed to allocate memory */
	KUZPKG_ERR_MEMORY,
	/** A system error occurred */
	KUZPKG_ERR_SYSTEM,
	/** Permmision denied */
	KUZPKG_ERR_BADPERMS,
	/** Should be a file */
	KUZPKG_ERR_NOT_A_FILE,
	/** Should be a directory */
	KUZPKG_ERR_NOT_A_DIR,
	/** Function was called with invalid arguments */
	KUZPKG_ERR_WRONG_ARGS,
	/** Insufficient disk space */
	KUZPKG_ERR_DISK_SPACE,
	/* Interface */
	/** Handle should be null */
	KUZPKG_ERR_HANDLE_NULL,
	/** Handle should not be null */
	KUZPKG_ERR_HANDLE_NOT_NULL,
	/** Failed to acquire lock */
	KUZPKG_ERR_HANDLE_LOCK,
	/* Databases */
	/** Failed to open database */
	KUZPKG_ERR_DB_OPEN,
	/** Failed to create database */
	KUZPKG_ERR_DB_CREATE,
	/** Database should not be null */
	KUZPKG_ERR_DB_NULL,
	/** Database should be null */
	KUZPKG_ERR_DB_NOT_NULL,
	/** The database could not be found */
	KUZPKG_ERR_DB_NOT_FOUND,
	/** Database is invalid */
	KUZPKG_ERR_DB_INVALID,
	/** Database has an invalid signature */
	KUZPKG_ERR_DB_INVALID_SIG,
	/** The localdb is in a newer/older format than libkuzpkg expects */
	KUZPKG_ERR_DB_VERSION,
	/** Failed to write to the database */
	KUZPKG_ERR_DB_WRITE,
	/** Failed to remove entry from database */
	KUZPKG_ERR_DB_REMOVE,
	/* Servers */
	/** Server URL is in an invalid format */
	KUZPKG_ERR_SERVER_BAD_URL,
	/** The database has no configured servers */
	KUZPKG_ERR_SERVER_NONE,
	/* Transactions */
	/** A transaction is already initialized */
	KUZPKG_ERR_TRANS_NOT_NULL,
	/** A transaction has not been initialized */
	KUZPKG_ERR_TRANS_NULL,
	/** Duplicate target in transaction */
	KUZPKG_ERR_TRANS_DUP_TARGET,
	/** Duplicate filename in transaction */
	KUZPKG_ERR_TRANS_DUP_FILENAME,
	/** A transaction has not been initialized */
	KUZPKG_ERR_TRANS_NOT_INITIALIZED,
	/** Transaction has not been prepared */
	KUZPKG_ERR_TRANS_NOT_PREPARED,
	/** Transaction was aborted */
	KUZPKG_ERR_TRANS_ABORT,
	/** Failed to interrupt transaction */
	KUZPKG_ERR_TRANS_TYPE,
	/** Tried to commit transaction without locking the database */
	KUZPKG_ERR_TRANS_NOT_LOCKED,
	/** A hook failed to run */
	KUZPKG_ERR_TRANS_HOOK_FAILED,
	/* Packages */
	/** Package not found */
	KUZPKG_ERR_PKG_NOT_FOUND,
	/** Package is in ignorepkg */
	KUZPKG_ERR_PKG_IGNORED,
	/** Package is invalid */
	KUZPKG_ERR_PKG_INVALID,
	/** Package has an invalid checksum */
	KUZPKG_ERR_PKG_INVALID_CHECKSUM,
	/** Package has an invalid signature */
	KUZPKG_ERR_PKG_INVALID_SIG,
	/** Package does not have a signature */
	KUZPKG_ERR_PKG_MISSING_SIG,
	/** Cannot open the package file */
	KUZPKG_ERR_PKG_OPEN,
	/** Failed to remove package files */
	KUZPKG_ERR_PKG_CANT_REMOVE,
	/** Package has an invalid name */
	KUZPKG_ERR_PKG_INVALID_NAME,
	/** Package has an invalid architecture */
	KUZPKG_ERR_PKG_INVALID_ARCH,
	/* Signatures */
	/** Signatures are missing */
	KUZPKG_ERR_SIG_MISSING,
	/** Signatures are invalid */
	KUZPKG_ERR_SIG_INVALID,
	/* Dependencies */
	/** Dependencies could not be satisfied */
	KUZPKG_ERR_UNSATISFIED_DEPS,
	/** Conflicting dependencies */
	KUZPKG_ERR_CONFLICTING_DEPS,
	/** Files conflict */
	KUZPKG_ERR_FILE_CONFLICTS,
	/* Misc */
	/** Download setup failed */
	KUZPKG_ERR_RETRIEVE_PREPARE,
	/** Download failed */
	KUZPKG_ERR_RETRIEVE,
	/** Invalid Regex */
	KUZPKG_ERR_INVALID_REGEX,
	/* External library errors */
	/** Error in libarchive */
	KUZPKG_ERR_LIBARCHIVE,
	/** Error in libcurl */
	KUZPKG_ERR_LIBCURL,
	/** Error in external download program */
	KUZPKG_ERR_EXTERNAL_DOWNLOAD,
	/** Error in gpgme */
	KUZPKG_ERR_GPGME,
	/** Missing compile-time features */
	KUZPKG_ERR_MISSING_CAPABILITY_SIGNATURES
} klpm_errno_t;

/** Returns the current error code from the handle.
 * @param handle the context handle
 * @return the current error code of the handle
 */
klpm_errno_t klpm_errno(klpm_handle_t *handle);

/** Returns the string corresponding to an error number.
 * @param err the error code to get the string for
 * @return the string relating to the given error code
 */
const char *klpm_strerror(klpm_errno_t err);

/* End of libkuzpkg_errors */
/** @} */


/** \addtogroup libkuzpkg_handle Handle
 * @brief Functions to initialize and release libkuzpkg
 * @{
 */

/** Initializes the library.
 * Creates handle, connects to database and creates lockfile.
 * This must be called before any other functions are called.
 * @param root the root path for all filesystem operations
 * @param dbpath the absolute path to the libkuzpkg database
 * @param err an optional variable to hold any error return codes
 * @return a context handle on success, NULL on error, err will be set if provided
 */
klpm_handle_t *klpm_initialize(const char *root, const char *dbpath,
		klpm_errno_t *err);

/** Release the library.
 * Disconnects from the database, removes handle and lockfile
 * This should be the last klpm call you make.
 * After this returns, handle should be considered invalid and cannot be reused
 * in any way.
 * @param handle the context handle
 * @return 0 on success, -1 on error
 */
int klpm_release(klpm_handle_t *handle);

/* End of libkuzpkg_handle */
/** @} */


/** @addtogroup libkuzpkg_sig Signature checking
 * @brief Functions to check signatures
 * @{
 */

/** PGP signature verification options */
typedef enum _klpm_siglevel_t {
	/** Packages require a signature */
	KUZPKG_SIG_PACKAGE = (1 << 0),
	/** Packages do not require a signature,
	 * but check packages that do have signatures */
	KUZPKG_SIG_PACKAGE_OPTIONAL = (1 << 1),
	/* Allow packages with signatures that are marginal trust */
	KUZPKG_SIG_PACKAGE_MARGINAL_OK = (1 << 2),
	/** Allow packages with signatures that are unknown trust */
	KUZPKG_SIG_PACKAGE_UNKNOWN_OK = (1 << 3),

	/** Databases require a signature */
	KUZPKG_SIG_DATABASE = (1 << 10),
	/** Databases do not require a signature,
	 * but check databases that do have signatures */
	KUZPKG_SIG_DATABASE_OPTIONAL = (1 << 11),
	/** Allow databases with signatures that are marginal trust */
	KUZPKG_SIG_DATABASE_MARGINAL_OK = (1 << 12),
	/** Allow databases with signatures that are unknown trust */
	KUZPKG_SIG_DATABASE_UNKNOWN_OK = (1 << 13),

	/** The Default siglevel */
	KUZPKG_SIG_USE_DEFAULT = (1 << 30)
} klpm_siglevel_t;

/** PGP signature verification status return codes */
typedef enum _klpm_sigstatus_t {
	/** Signature is valid */
	KUZPKG_SIGSTATUS_VALID,
	/** The key has expired */
	KUZPKG_SIGSTATUS_KEY_EXPIRED,
	/** The signature has expired */
	KUZPKG_SIGSTATUS_SIG_EXPIRED,
	/** The key is not in the keyring */
	KUZPKG_SIGSTATUS_KEY_UNKNOWN,
	/** The key has been disabled */
	KUZPKG_SIGSTATUS_KEY_DISABLED,
	/** The signature is invalid */
	KUZPKG_SIGSTATUS_INVALID
} klpm_sigstatus_t;


/** The trust level of a PGP key */
typedef enum _klpm_sigvalidity_t {
	/** The signature is fully trusted */
	KUZPKG_SIGVALIDITY_FULL,
	/** The signature is marginally trusted */
	KUZPKG_SIGVALIDITY_MARGINAL,
	/** The signature is never trusted */
	KUZPKG_SIGVALIDITY_NEVER,
	/** The signature has unknown trust */
	KUZPKG_SIGVALIDITY_UNKNOWN
} klpm_sigvalidity_t;

/** A PGP key */
typedef struct _klpm_pgpkey_t {
	/** The actual key data */
	void *data;
	/** The key's fingerprint */
	char *fingerprint;
	/** UID of the key */
	char *uid;
	/** Name of the key's owner */
	char *name;
	/** Email of the key's owner */
	char *email;
	/** When the key was created */
	klpm_time_t created;
	/** When the key expires */
	klpm_time_t expires;
	/** The length of the key */
	unsigned int length;
	/** has the key been revoked */
	unsigned int revoked;
} klpm_pgpkey_t;

/**
 * Signature result. Contains the key, status, and validity of a given
 * signature.
 */
typedef struct _klpm_sigresult_t {
	/** The key of the signature */
	klpm_pgpkey_t key;
	/** The status of the signature */
	klpm_sigstatus_t status;
	/** The validity of the signature */
	klpm_sigvalidity_t validity;
} klpm_sigresult_t;

/**
 * Signature list. Contains the number of signatures found and a pointer to an
 * array of results. The array is of size count.
 */
typedef struct _klpm_siglist_t {
	/** The amount of results in the array */
	size_t count;
	/** An array of sigresults */
	klpm_sigresult_t *results;
} klpm_siglist_t;

/**
 * Check the PGP signature for the given package file.
 * @param pkg the package to check
 * @param siglist a pointer to storage for signature results
 * @return 0 on success, -1 if an error occurred or signature is missing
 */
int klpm_pkg_check_pgp_signature(klpm_pkg_t *pkg, klpm_siglist_t *siglist);

/**
 * Check the PGP signature for the given database.
 * @param db the database to check
 * @param siglist a pointer to storage for signature results
 * @return 0 on success, -1 if an error occurred or signature is missing
 */
int klpm_db_check_pgp_signature(klpm_db_t *db, klpm_siglist_t *siglist);

/**
 * Clean up and free a signature result list.
 * Note that this does not free the siglist object itself in case that
 * was allocated on the stack; this is the responsibility of the caller.
 * @param siglist a pointer to storage for signature results
 * @return 0 on success, -1 on error
 */
int klpm_siglist_cleanup(klpm_siglist_t *siglist);

/**
 * Decode a loaded signature in base64 form.
 * @param base64_data the signature to attempt to decode
 * @param data the decoded data; must be freed by the caller
 * @param data_len the length of the returned data
 * @return 0 on success, -1 on failure to properly decode
 */
int klpm_decode_signature(const char *base64_data,
		unsigned char **data, size_t *data_len);

/**
 * Extract the Issuer Key ID from a signature
 * @param handle the context handle
 * @param identifier the identifier of the key.
 * This may be the name of the package or the path to the package.
 * @param sig PGP signature
 * @param len length of signature
 * @param keys a pointer to storage for key IDs
 * @return 0 on success, -1 on error
 */
int klpm_extract_keyid(klpm_handle_t *handle, const char *identifier,
		const unsigned char *sig, const size_t len, klpm_list_t **keys);

/* End of libkuzpkg_sig */
/** @} */


/** @addtogroup libkuzpkg_depends Dependency
 * @brief Functions dealing with libkuzpkg's dependency and conflict
 * information.
 * @{
 */

/** Types of version constraints in dependency specs. */
typedef enum _klpm_depmod_t {
        /** No version constraint */
        KUZPKG_DEP_MOD_ANY = 1,
        /** Test version equality (package=x.y.z) */
        KUZPKG_DEP_MOD_EQ,
        /** Test for at least a version (package>=x.y.z) */
        KUZPKG_DEP_MOD_GE,
        /** Test for at most a version (package<=x.y.z) */
        KUZPKG_DEP_MOD_LE,
        /** Test for greater than some version (package>x.y.z) */
        KUZPKG_DEP_MOD_GT,
        /** Test for less than some version (package<x.y.z) */
        KUZPKG_DEP_MOD_LT
} klpm_depmod_t;

/**
 * File conflict type.
 * Whether the conflict results from a file existing on the filesystem, or with
 * another target in the transaction.
 */
typedef enum _klpm_fileconflicttype_t {
	/** The conflict results with a another target in the transaction */
	KUZPKG_FILECONFLICT_TARGET = 1,
	/** The conflict results from a file existing on the filesystem */
	KUZPKG_FILECONFLICT_FILESYSTEM
} klpm_fileconflicttype_t;

/** The basic dependency type.
 *
 * This type is used throughout libkuzpkg, not just for dependencies
 * but also conflicts and providers. */
typedef struct _klpm_depend_t {
	/**  Name of the provider to satisfy this dependency */
	char *name;
	/**  Version of the provider to match against (optional) */
	char *version;
	/** A description of why this dependency is needed (optional) */
	char *desc;
	/** A hash of name (used internally to speed up conflict checks) */
	unsigned long name_hash;
	/** How the version should match against the provider */
	klpm_depmod_t mod;
} klpm_depend_t;

/** Missing dependency. */
typedef struct _klpm_depmissing_t {
	/** Name of the package that has the dependency */
	char *target;
	/** The dependency that was wanted */
	klpm_depend_t *depend;
	/** If the depmissing was caused by a conflict, the name of the package
	 * that would be installed, causing the satisfying package to be removed */
	char *causingpkg;
} klpm_depmissing_t;

/** A conflict that has occurred between two packages. */
typedef struct _klpm_conflict_t {
	/** The first package */
	klpm_pkg_t *package1;
	/** The second package */
	klpm_pkg_t *package2;
	/** The conflict */
	klpm_depend_t *reason;
} klpm_conflict_t;

/** File conflict.
 *
 * A conflict that has happened due to a two packages containing the same file,
 * or a package contains a file that is already on the filesystem and not owned
 * by that package. */
typedef struct _klpm_fileconflict_t {
	/** The name of the package that caused the conflict */
	char *target;
	/** The type of conflict */
	klpm_fileconflicttype_t type;
	/** The name of the file that the package conflicts with */
	char *file;
	/** The name of the package that also owns the file if there is one*/
	char *ctarget;
} klpm_fileconflict_t;

/** Checks dependencies and returns missing ones in a list.
 * Dependencies can include versions with depmod operators.
 * @param handle the context handle
 * @param pkglist the list of local packages
 * @param remove an klpm_list_t* of packages to be removed
 * @param upgrade an klpm_list_t* of packages to be upgraded (remove-then-upgrade)
 * @param reversedeps handles the backward dependencies
 * @return an klpm_list_t* of klpm_depmissing_t pointers.
 */
klpm_list_t *klpm_checkdeps(klpm_handle_t *handle, klpm_list_t *pkglist,
		klpm_list_t *remove, klpm_list_t *upgrade, int reversedeps);

/** Find a package satisfying a specified dependency.
 * The dependency can include versions with depmod operators.
 * @param pkgs an klpm_list_t* of klpm_pkg_t where the satisfyer will be searched
 * @param depstring package or provision name, versioned or not
 * @return a klpm_pkg_t* satisfying depstring
 */
klpm_pkg_t *klpm_find_satisfier(klpm_list_t *pkgs, const char *depstring);

/** Find a package satisfying a specified dependency.
 * First look for a literal, going through each db one by one. Then look for
 * providers. The first satisfyer that belongs to an installed package is
 * returned. If no providers belong to an installed package then an
 * klpm_question_select_provider_t is created to select the provider.
 * The dependency can include versions with depmod operators.
 *
 * @param handle the context handle
 * @param dbs an klpm_list_t* of klpm_db_t where the satisfyer will be searched
 * @param depstring package or provision name, versioned or not
 * @return a klpm_pkg_t* satisfying depstring
 */
klpm_pkg_t *klpm_find_dbs_satisfier(klpm_handle_t *handle,
		klpm_list_t *dbs, const char *depstring);

/** Check the package conflicts in a database
 *
 * @param handle the context handle
 * @param pkglist the list of packages to check
 *
 * @return an klpm_list_t of klpm_conflict_t
 */
klpm_list_t *klpm_checkconflicts(klpm_handle_t *handle, klpm_list_t *pkglist);

/** Returns a newly allocated string representing the dependency information.
 * @param dep a dependency info structure
 * @return a formatted string, e.g. "glibc>=2.12"
 */
char *klpm_dep_compute_string(const klpm_depend_t *dep);

/** Return a newly allocated dependency information parsed from a string
 *\link klpm_dep_free should be used to free the dependency \endlink
 * @param depstring a formatted string, e.g. "glibc=2.12"
 * @return a dependency info structure
 */
klpm_depend_t *klpm_dep_from_string(const char *depstring);

/** Free a dependency info structure
 * @param dep struct to free
 */
void klpm_dep_free(klpm_depend_t *dep);

/** Free a fileconflict and its members.
 * @param conflict the fileconflict to free
 */
void klpm_fileconflict_free(klpm_fileconflict_t *conflict);

/** Free a depmissing and its members
 * @param miss the depmissing to free
 * */
void klpm_depmissing_free(klpm_depmissing_t *miss);

/**
 * Free a conflict and its members.
 * @param conflict the conflict to free
 */
void klpm_conflict_free(klpm_conflict_t *conflict);


/* End of libkuzpkg_depends */
/** @} */


/** \addtogroup libkuzpkg_cb Callbacks
 * @brief Functions and structures for libkuzpkg's callbacks
 * @{
 */

/**
 * Type of events.
 */
typedef enum _klpm_event_type_t {
	/** Dependencies will be computed for a package. */
	KUZPKG_EVENT_CHECKDEPS_START = 1,
	/** Dependencies were computed for a package. */
	KUZPKG_EVENT_CHECKDEPS_DONE,
	/** File conflicts will be computed for a package. */
	KUZPKG_EVENT_FILECONFLICTS_START,
	/** File conflicts were computed for a package. */
	KUZPKG_EVENT_FILECONFLICTS_DONE,
	/** Dependencies will be resolved for target package. */
	KUZPKG_EVENT_RESOLVEDEPS_START,
	/** Dependencies were resolved for target package. */
	KUZPKG_EVENT_RESOLVEDEPS_DONE,
	/** Inter-conflicts will be checked for target package. */
	KUZPKG_EVENT_INTERCONFLICTS_START,
	/** Inter-conflicts were checked for target package. */
	KUZPKG_EVENT_INTERCONFLICTS_DONE,
	/** Processing the package transaction is starting. */
	KUZPKG_EVENT_TRANSACTION_START,
	/** Processing the package transaction is finished. */
	KUZPKG_EVENT_TRANSACTION_DONE,
	/** Package will be installed/upgraded/downgraded/re-installed/removed; See
	 * klpm_event_package_operation_t for arguments. */
	KUZPKG_EVENT_PACKAGE_OPERATION_START,
	/** Package was installed/upgraded/downgraded/re-installed/removed; See
	 * klpm_event_package_operation_t for arguments. */
	KUZPKG_EVENT_PACKAGE_OPERATION_DONE,
	/** Target package's integrity will be checked. */
	KUZPKG_EVENT_INTEGRITY_START,
	/** Target package's integrity was checked. */
	KUZPKG_EVENT_INTEGRITY_DONE,
	/** Target package will be loaded. */
	KUZPKG_EVENT_LOAD_START,
	/** Target package is finished loading. */
	KUZPKG_EVENT_LOAD_DONE,
	/** Scriptlet has printed information; See klpm_event_scriptlet_info_t for
	 * arguments. */
	KUZPKG_EVENT_SCRIPTLET_INFO,
	/** Database files will be downloaded from a repository. */
	KUZPKG_EVENT_DB_RETRIEVE_START,
	/** Database files were downloaded from a repository. */
	KUZPKG_EVENT_DB_RETRIEVE_DONE,
	/** Not all database files were successfully downloaded from a repository. */
	KUZPKG_EVENT_DB_RETRIEVE_FAILED,
	/** Package files will be downloaded from a repository. */
	KUZPKG_EVENT_PKG_RETRIEVE_START,
	/** Package files were downloaded from a repository. */
	KUZPKG_EVENT_PKG_RETRIEVE_DONE,
	/** Not all package files were successfully downloaded from a repository. */
	KUZPKG_EVENT_PKG_RETRIEVE_FAILED,
	/** Disk space usage will be computed for a package. */
	KUZPKG_EVENT_DISKSPACE_START,
	/** Disk space usage was computed for a package. */
	KUZPKG_EVENT_DISKSPACE_DONE,
	/** An optdepend for another package is being removed; See
	 * klpm_event_optdep_removal_t for arguments. */
	KUZPKG_EVENT_OPTDEP_REMOVAL,
	/** A configured repository database is missing; See
	 * klpm_event_database_missing_t for arguments. */
	KUZPKG_EVENT_DATABASE_MISSING,
	/** Checking keys used to create signatures are in keyring. */
	KUZPKG_EVENT_KEYRING_START,
	/** Keyring checking is finished. */
	KUZPKG_EVENT_KEYRING_DONE,
	/** Downloading missing keys into keyring. */
	KUZPKG_EVENT_KEY_DOWNLOAD_START,
	/** Key downloading is finished. */
	KUZPKG_EVENT_KEY_DOWNLOAD_DONE,
	/** A .pacnew file was created; See klpm_event_pacnew_created_t for arguments. */
	KUZPKG_EVENT_PACNEW_CREATED,
	/** A .pacsave file was created; See klpm_event_pacsave_created_t for
	 * arguments. */
	KUZPKG_EVENT_PACSAVE_CREATED,
	/** Processing hooks will be started. */
	KUZPKG_EVENT_HOOK_START,
	/** Processing hooks is finished. */
	KUZPKG_EVENT_HOOK_DONE,
	/** A hook is starting */
	KUZPKG_EVENT_HOOK_RUN_START,
	/** A hook has finished running. */
	KUZPKG_EVENT_HOOK_RUN_DONE
} klpm_event_type_t;

/** An event that may represent any event. */
typedef struct _klpm_event_any_t {
	/** Type of event */
	klpm_event_type_t type;
} klpm_event_any_t;

/** An enum over the kind of package operations. */
typedef enum _klpm_package_operation_t {
	/** Package (to be) installed. (No oldpkg) */
	KUZPKG_PACKAGE_INSTALL = 1,
	/** Package (to be) upgraded */
	KUZPKG_PACKAGE_UPGRADE,
	/** Package (to be) re-installed */
	KUZPKG_PACKAGE_REINSTALL,
	/** Package (to be) downgraded */
	KUZPKG_PACKAGE_DOWNGRADE,
	/** Package (to be) removed (No newpkg) */
	KUZPKG_PACKAGE_REMOVE
} klpm_package_operation_t;

/** A package operation event occurred. */
typedef struct _klpm_event_package_operation_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Type of operation */
	klpm_package_operation_t operation;
	/** Old package */
	klpm_pkg_t *oldpkg;
	/** New package */
	klpm_pkg_t *newpkg;
} klpm_event_package_operation_t;

/** An optional dependency was removed. */
typedef struct _klpm_event_optdep_removal_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Package with the optdep */
	klpm_pkg_t *pkg;
	/** Optdep being removed */
	klpm_depend_t *optdep;
} klpm_event_optdep_removal_t;

/** A scriptlet was ran. */
typedef struct _klpm_event_scriptlet_info_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Line of scriptlet output */
	const char *line;
} klpm_event_scriptlet_info_t;


/** A database is missing.
 *
 * The database is registered but has not been downloaded
 */
typedef struct _klpm_event_database_missing_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Name of the database */
	const char *dbname;
} klpm_event_database_missing_t;

/** A package was downloaded. */
typedef struct _klpm_event_pkgdownload_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Name of the file */
	const char *file;
} klpm_event_pkgdownload_t;

/** A pacnew file was created. */
typedef struct _klpm_event_pacnew_created_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Whether the creation was result of a NoUpgrade or not */
	int from_noupgrade;
	/** Old package */
	klpm_pkg_t *oldpkg;
	/** New Package */
	klpm_pkg_t *newpkg;
	/** Filename of the file without the .pacnew suffix */
	const char *file;
} klpm_event_pacnew_created_t;

/** A pacsave file was created. */
typedef struct _klpm_event_pacsave_created_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Old package */
	klpm_pkg_t *oldpkg;
	/** Filename of the file without the .pacsave suffix */
	const char *file;
} klpm_event_pacsave_created_t;

/** Kind of hook. */
typedef enum _klpm_hook_when_t {
	/* Pre transaction hook */
	KUZPKG_HOOK_PRE_TRANSACTION = 1,
	/* Post transaction hook */
	KUZPKG_HOOK_POST_TRANSACTION
} klpm_hook_when_t;

/** pre/post transaction hooks are to be ran. */
typedef struct _klpm_event_hook_t {
	/** Type of event*/
	klpm_event_type_t type;
	/** Type of hook */
	klpm_hook_when_t when;
} klpm_event_hook_t;

/** A pre/post transaction hook was ran. */
typedef struct _klpm_event_hook_run_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Name of hook */
	const char *name;
	/** Description of hook to be outputted */
	const char *desc;
	/** position of hook being run */
	size_t position;
	/** total hooks being run */
	size_t total;
} klpm_event_hook_run_t;

/** Packages downloading about to start. */
typedef struct _klpm_event_pkg_retrieve_t {
	/** Type of event */
	klpm_event_type_t type;
	/** Number of packages to download */
	size_t num;
	/** Total size of packages to download */
	off_t total_size;
} klpm_event_pkg_retrieve_t;

/** Events.
 * This is a union passed to the callback that allows the frontend to know
 * which type of event was triggered (via type). It is then possible to
 * typecast the pointer to the right structure, or use the union field, in order
 * to access event-specific data. */
typedef union _klpm_event_t {
	/** Type of event it's always safe to access this. */
	klpm_event_type_t type;
	/** The any event type. It's always safe to access this. */
	klpm_event_any_t any;
	/** Package operation */
	klpm_event_package_operation_t package_operation;
	/** An optdept was remove */
	klpm_event_optdep_removal_t optdep_removal;
	/** A scriptlet was ran */
	klpm_event_scriptlet_info_t scriptlet_info;
	/** A database is missing */
	klpm_event_database_missing_t database_missing;
	/** A package was downloaded */
	klpm_event_pkgdownload_t pkgdownload;
	/** A pacnew file was created */
	klpm_event_pacnew_created_t pacnew_created;
	/** A pacsave file was created */
	klpm_event_pacsave_created_t pacsave_created;
	/** Pre/post transaction hooks are being ran */
	klpm_event_hook_t hook;
	/** A hook was ran */
	klpm_event_hook_run_t hook_run;
	/** Download packages */
	klpm_event_pkg_retrieve_t pkg_retrieve;
} klpm_event_t;

/** Event callback.
 *
 * Called when an event occurs
 * @param ctx user-provided context
 * @param event the event that occurred */
typedef void (*klpm_cb_event)(void *ctx, klpm_event_t *event);

/**
 * Type of question.
 * Unlike the events or progress enumerations, this enum has bitmask values
 * so a frontend can use a bitmask map to supply preselected answers to the
 * different types of questions.
 */
typedef enum _klpm_question_type_t {
	/** Should target in ignorepkg be installed anyway? */
	KUZPKG_QUESTION_INSTALL_IGNOREPKG = (1 << 0),
	/** Should a package be replaced? */
	KUZPKG_QUESTION_REPLACE_PKG = (1 << 1),
	/** Should a conflicting package be removed? */
	KUZPKG_QUESTION_CONFLICT_PKG = (1 << 2),
	/** Should a corrupted package be deleted? */
	KUZPKG_QUESTION_CORRUPTED_PKG = (1 << 3),
	/** Should unresolvable targets be removed from the transaction? */
	KUZPKG_QUESTION_REMOVE_PKGS = (1 << 4),
	/** Provider selection */
	KUZPKG_QUESTION_SELECT_PROVIDER = (1 << 5),
	/** Should a key be imported? */
	KUZPKG_QUESTION_IMPORT_KEY = (1 << 6)
} klpm_question_type_t;

/** A question that can represent any other question. */
typedef struct _klpm_question_any_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer */
	int answer;
} klpm_question_any_t;

/** Should target in ignorepkg be installed anyway? */
typedef struct _klpm_question_install_ignorepkg_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: whether or not to install pkg anyway */
	int install;
	/** The ignored package that we are deciding whether to install */
	klpm_pkg_t *pkg;
} klpm_question_install_ignorepkg_t;

/** Should a package be replaced? */
typedef struct _klpm_question_replace_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: whether or not to replace oldpkg with newpkg */
	int replace;
	/** Package to be replaced */
	klpm_pkg_t *oldpkg;
	/** Package to replace with.*/
	klpm_pkg_t *newpkg;
	/** DB of newpkg */
	klpm_db_t *newdb;
} klpm_question_replace_t;

/** Should a conflicting package be removed? */
typedef struct _klpm_question_conflict_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: whether or not to remove conflict->package2 */
	int remove;
	/** Conflict info */
	klpm_conflict_t *conflict;
} klpm_question_conflict_t;

/** Should a corrupted package be deleted? */
typedef struct _klpm_question_corrupted_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: whether or not to remove filepath */
	int remove;
	/** File to remove */
	const char *filepath;
	/** Error code indicating the reason for package invalidity */
	klpm_errno_t reason;
} klpm_question_corrupted_t;

/** Should unresolvable targets be removed from the transaction? */
typedef struct _klpm_question_remove_pkgs_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: whether or not to skip packages */
	int skip;
	/** List of klpm_pkg_t* with unresolved dependencies */
	klpm_list_t *packages;
} klpm_question_remove_pkgs_t;

/** Provider selection */
typedef struct _klpm_question_select_provider_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: which provider to use (index from providers) */
	int use_index;
	/** List of klpm_pkg_t* as possible providers */
	klpm_list_t *providers;
	/** What providers provide for */
	klpm_depend_t *depend;
} klpm_question_select_provider_t;

/** Should a key be imported? */
typedef struct _klpm_question_import_key_t {
	/** Type of question */
	klpm_question_type_t type;
	/** Answer: whether or not to import key */
	int import;
	/** UID of the key to import */
	const char *uid;
	/** Fingerprint the key to import */
	const char *fingerprint;
} klpm_question_import_key_t;

/**
 * Questions.
 * This is an union passed to the callback that allows the frontend to know
 * which type of question was triggered (via type). It is then possible to
 * typecast the pointer to the right structure, or use the union field, in order
 * to access question-specific data. */
typedef union _klpm_question_t {
	/** The type of question. It's always safe to access this. */
	klpm_question_type_t type;
	/** A question that can represent any question.
	 * It's always safe to access this. */
	klpm_question_any_t any;
	/** Should target in ignorepkg be installed anyway? */
	klpm_question_install_ignorepkg_t install_ignorepkg;
	/** Should a package be replaced? */
	klpm_question_replace_t replace;
	/** Should a conflicting package be removed? */
	klpm_question_conflict_t conflict;
	/** Should a corrupted package be deleted? */
	klpm_question_corrupted_t corrupted;
	/** Should unresolvable targets be removed from the transaction? */
	klpm_question_remove_pkgs_t remove_pkgs;
	/** Provider selection */
	klpm_question_select_provider_t select_provider;
	/** Should a key be imported? */
	klpm_question_import_key_t import_key;
} klpm_question_t;

/** Question callback.
 *
 * This callback allows user to give input and decide what to do during certain events
 * @param ctx user-provided context
 * @param question the question being asked.
 */
typedef void (*klpm_cb_question)(void *ctx, klpm_question_t *question);

/** An enum over different kinds of progress alerts. */
typedef enum _klpm_progress_t {
	/** Package install */
	KUZPKG_PROGRESS_ADD_START,
	/** Package upgrade */
	KUZPKG_PROGRESS_UPGRADE_START,
	/** Package downgrade */
	KUZPKG_PROGRESS_DOWNGRADE_START,
	/** Package reinstall */
	KUZPKG_PROGRESS_REINSTALL_START,
	/** Package removal */
	KUZPKG_PROGRESS_REMOVE_START,
	/** Conflict checking */
	KUZPKG_PROGRESS_CONFLICTS_START,
	/** Diskspace checking */
	KUZPKG_PROGRESS_DISKSPACE_START,
	/** Package Integrity checking */
	KUZPKG_PROGRESS_INTEGRITY_START,
	/** Loading packages from disk */
	KUZPKG_PROGRESS_LOAD_START,
	/** Checking signatures of packages */
	KUZPKG_PROGRESS_KEYRING_START
} klpm_progress_t;

/** Progress callback
 *
 * Alert the front end about the progress of certain events.
 * Allows the implementation of loading bars for events that
 * make take a while to complete.
 * @param ctx user-provided context
 * @param progress the kind of event that is progressing
 * @param pkg for package operations, the name of the package being operated on
 * @param percent the percent completion of the action
 * @param howmany the total amount of items in the action
 * @param current the current amount of items completed
 */
/** Progress callback */
typedef void (*klpm_cb_progress)(void *ctx, klpm_progress_t progress, const char *pkg,
		int percent, size_t howmany, size_t current);

/*
 * Downloading
 */

/** File download events.
 * These events are reported by KUZPKG via download callback.
 */
typedef enum _klpm_download_event_type_t {
	/** A download was started */
	KUZPKG_DOWNLOAD_INIT,
	/** A download made progress */
	KUZPKG_DOWNLOAD_PROGRESS,
	/** Download will be retried */
	KUZPKG_DOWNLOAD_RETRY,
	/** A download completed */
	KUZPKG_DOWNLOAD_COMPLETED
} klpm_download_event_type_t;

/** Context struct for when a download starts. */
typedef struct _klpm_download_event_init_t {
	/** whether this file is optional and thus the errors could be ignored */
	int optional;
} klpm_download_event_init_t;

/** Context struct for when a download progresses. */
typedef struct _klpm_download_event_progress_t {
	/** Amount of data downloaded */
	off_t downloaded;
	/** Total amount need to be downloaded */
	off_t total;
} klpm_download_event_progress_t;

/** Context struct for when a download retries. */
typedef struct _klpm_download_event_retry_t {
	/** If the download will resume or start over */
	int resume;
} klpm_download_event_retry_t;

/** Context struct for when a download completes. */
typedef struct _klpm_download_event_completed_t {
	/** Total bytes in file */
	off_t total;
	/** download result code:
	 *    0 - download completed successfully
	 *    1 - the file is up-to-date
	 *   -1 - error
	 */
	int result;
} klpm_download_event_completed_t;

/** Type of download progress callbacks.
 * @param ctx user-provided context
 * @param filename the name of the file being downloaded
 * @param event the event type
 * @param data the event data of type klpm_download_event_*_t
 */
typedef void (*klpm_cb_download)(void *ctx, const char *filename,
		klpm_download_event_type_t event, void *data);


/** A callback for downloading files
 * @param ctx user-provided context
 * @param url the URL of the file to be downloaded
 * @param localpath the directory to which the file should be downloaded
 * @param force whether to force an update, even if the file is the same
 * @return 0 on success, 1 if the file exists and is identical, -1 on
 * error.
 */
typedef int (*klpm_cb_fetch)(void *ctx, const char *url, const char *localpath,
		int force);

/* End of libkuzpkg_cb */
/** @} */


/** @addtogroup libkuzpkg_databases Database
 * @brief Functions to query and manipulate the database of libkuzpkg.
 * @{
 */

/** Get the database of locally installed packages.
 * The returned pointer points to an internal structure
 * of libkuzpkg which should only be manipulated through
 * libkuzpkg functions.
 * @return a reference to the local database
 */
klpm_db_t *klpm_get_localdb(klpm_handle_t *handle);

/** Get the list of sync databases.
 * Returns a list of klpm_db_t structures, one for each registered
 * sync database.
 *
 * @param handle the context handle
 * @return a reference to an internal list of klpm_db_t structures
 */
klpm_list_t *klpm_get_syncdbs(klpm_handle_t *handle);

/** Register a sync database of packages.
 * Databases can not be registered when there is an active transaction.
 *
 * @param handle the context handle
 * @param treename the name of the sync repository
 * @param level what level of signature checking to perform on the
 * database; note that this must be a '.sig' file type verification
 * @return an klpm_db_t* on success (the value), NULL on error
 */
klpm_db_t *klpm_register_syncdb(klpm_handle_t *handle, const char *treename,
		int level);

/** Unregister all package databases.
 * Databases can not be unregistered while there is an active transaction.
 *
 * @param handle the context handle
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_unregister_all_syncdbs(klpm_handle_t *handle);

/** Unregister a package database.
 * Databases can not be unregistered when there is an active transaction.
 *
 * @param db pointer to the package database to unregister
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_db_unregister(klpm_db_t *db);

/** Get the handle of a package database.
 * @param db pointer to the package database
 * @return the klpm handle that the package database belongs to
 */
klpm_handle_t *klpm_db_get_handle(klpm_db_t *db);

/** Get the name of a package database.
 * @param db pointer to the package database
 * @return the name of the package database, NULL on error
 */
const char *klpm_db_get_name(const klpm_db_t *db);

/** Get the signature verification level for a database.
 * Will return the default verification level if this database is set up
 * with KUZPKG_SIG_USE_DEFAULT.
 * @param db pointer to the package database
 * @return the signature verification level
 */
int klpm_db_get_siglevel(klpm_db_t *db);

/** Check the validity of a database.
 * This is most useful for sync databases and verifying signature status.
 * If invalid, the handle error code will be set accordingly.
 * @param db pointer to the package database
 * @return 0 if valid, -1 if invalid (pm_errno is set accordingly)
 */
int klpm_db_get_valid(klpm_db_t *db);

/** @name Server accessors
 * @{
 */

/** Get the list of servers assigned to this db.
 * @param db pointer to the database to get the servers from
 * @return a char* list of servers
 */
klpm_list_t *klpm_db_get_servers(const klpm_db_t *db);

/** Sets the list of servers for the database to use.
 * @param db the database to set the servers. The list will be duped and
 * the original will still need to be freed by the caller.
 * @param servers a char* list of servers.
 */
int klpm_db_set_servers(klpm_db_t *db, klpm_list_t *servers);

/** Add a download server to a database.
 * @param db database pointer
 * @param url url of the server
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_db_add_server(klpm_db_t *db, const char *url);

/** Remove a download server from a database.
 * @param db database pointer
 * @param url url of the server
 * @return 0 on success, 1 on server not present,
 * -1 on error (pm_errno is set accordingly)
 */
int klpm_db_remove_server(klpm_db_t *db, const char *url);

/** Get the list of cache servers assigned to this db.
 * @param db pointer to the database to get the servers from
 * @return a char* list of servers
 */
klpm_list_t *klpm_db_get_cache_servers(const klpm_db_t *db);

/** Sets the list of cache servers for the database to use.
 * @param db the database to set the servers. The list will be duped and
 * the original will still need to be freed by the caller.
 * @param servers a char* list of servers.
 */
int klpm_db_set_cache_servers(klpm_db_t *db, klpm_list_t *servers);

/** Add a download cache server to a database.
 * @param db database pointer
 * @param url url of the server
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_db_add_cache_server(klpm_db_t *db, const char *url);

/** Remove a download cache server from a database.
 * @param db database pointer
 * @param url url of the server
 * @return 0 on success, 1 on server not present,
 * -1 on error (pm_errno is set accordingly)
 */
int klpm_db_remove_cache_server(klpm_db_t *db, const char *url);

/* End of server accessors */
/** @} */

/** Update package databases.
 *
 * An update of the package databases in the list \a dbs will be attempted.
 * Unless \a force is true, the update will only be performed if the remote
 * databases were modified since the last update.
 *
 * This operation requires a database lock, and will return an applicable error
 * if the lock could not be obtained.
 *
 * Example:
 * @code
 * klpm_list_t *dbs = klpm_get_syncdbs(config->handle);
 * ret = klpm_db_update(config->handle, dbs, force);
 * if(ret < 0) {
 *     pm_printf(KUZPKG_LOG_ERROR, _("failed to synchronize all databases (%s)\n"),
 *         klpm_strerror(klpm_errno(config->handle)));
 * }
 * @endcode
 *
 * @note After a successful update, the \link klpm_db_get_pkgcache()
 * package cache \endlink will be invalidated
 * @param handle the context handle
 * @param dbs list of package databases to update
 * @param force if true, then forces the update, otherwise update only in case
 * the databases aren't up to date
 * @return 0 on success, -1 on error (pm_errno is set accordingly),
 * 1 if all databases are up to to date
 */
int klpm_db_update(klpm_handle_t *handle, klpm_list_t *dbs, int force);

/** Get a package entry from a package database.
 * Looking up a package is O(1) and will be significantly faster than
 * iterating over the pkgcahe.
 * @param db pointer to the package database to get the package from
 * @param name of the package
 * @return the package entry on success, NULL on error
 */
klpm_pkg_t *klpm_db_get_pkg(klpm_db_t *db, const char *name);

/** Get the package cache of a package database.
 * This is a list of all packages the db contains.
 * @param db pointer to the package database to get the package from
 * @return the list of packages on success, NULL on error
 */
klpm_list_t *klpm_db_get_pkgcache(klpm_db_t *db);

/** Get a group entry from a package database.
 * Looking up a group is O(1) and will be significantly faster than
 * iterating over the groupcahe.
 * @param db pointer to the package database to get the group from
 * @param name of the group
 * @return the groups entry on success, NULL on error
 */
klpm_group_t *klpm_db_get_group(klpm_db_t *db, const char *name);

/** Get the group cache of a package database.
 * @param db pointer to the package database to get the group from
 * @return the list of groups on success, NULL on error
 */
klpm_list_t *klpm_db_get_groupcache(klpm_db_t *db);

/** Searches a database with regular expressions.
 * @param db pointer to the package database to search in
 * @param needles a list of regular expressions to search for
 * @param ret pointer to list for storing packages matching all
 * regular expressions - must point to an empty (NULL) klpm_list_t *.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_db_search(klpm_db_t *db, const klpm_list_t *needles,
		klpm_list_t **ret);

/** The usage level of a database. */
typedef enum _klpm_db_usage_t {
       /** Enable refreshes for this database */
       KUZPKG_DB_USAGE_SYNC = 1,
       /** Enable search for this database */
       KUZPKG_DB_USAGE_SEARCH = (1 << 1),
       /** Enable installing packages from this database */
       KUZPKG_DB_USAGE_INSTALL = (1 << 2),
       /** Enable sysupgrades with this database */
       KUZPKG_DB_USAGE_UPGRADE = (1 << 3),
       /** Enable all usage levels */
       KUZPKG_DB_USAGE_ALL = (1 << 4) - 1,
} klpm_db_usage_t;

/** @name Usage accessors
 * @{
 */

/** Sets the usage of a database.
 * @param db pointer to the package database to set the status for
 * @param usage a bitmask of klpm_db_usage_t values
 * @return 0 on success, or -1 on error
 */
int klpm_db_set_usage(klpm_db_t *db, int usage);

/** Gets the usage of a database.
 * @param db pointer to the package database to get the status of
 * @param usage pointer to an klpm_db_usage_t to store db's status
 * @return 0 on success, or -1 on error
 */
int klpm_db_get_usage(klpm_db_t *db, int *usage);

/* End of usage accessors */
/** @} */


/* End of libkuzpkg_databases */
/** @} */


/** \addtogroup libkuzpkg_log Logging Functions
 * @brief Functions to log using libkuzpkg
 * @{
 */

/** Logging Levels */
typedef enum _klpm_loglevel_t {
       /** Error */
       KUZPKG_LOG_ERROR    = 1,
       /** Warning */
       KUZPKG_LOG_WARNING  = (1 << 1),
       /** Debug */
       KUZPKG_LOG_DEBUG    = (1 << 2),
       /** Function */
       KUZPKG_LOG_FUNCTION = (1 << 3)
} klpm_loglevel_t;


/** The callback type for logging.
 *
 * libkuzpkg will call this function whenever something is to be logged.
 * many libkuzpkg will produce log output. Additionally any calls to \link klpm_logaction
 * \endlink will also call this callback.
 * @param ctx user-provided context
 * @param level the currently set loglevel
 * @param fmt the printf like format string
 * @param args printf like arguments
 */
typedef void (*klpm_cb_log)(void *ctx, klpm_loglevel_t level, const char *fmt, va_list args);

/** A printf-like function for logging.
 * @param handle the context handle
 * @param prefix caller-specific prefix for the log
 * @param fmt output format
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_logaction(klpm_handle_t *handle, const char *prefix,
		const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* End of libkuzpkg_log */
/** @} */


/** @addtogroup libkuzpkg_options Options
 * Libklpm option getters and setters
 * @{
 */

/** @name Accessors for callbacks
 * @{
 */

/** Returns the callback used for logging.
 * @param handle the context handle
 * @return the currently set log callback
 */
klpm_cb_log klpm_option_get_logcb(klpm_handle_t *handle);

/** Returns the callback used for logging.
 * @param handle the context handle
 * @return the currently set log callback context
 */
void *klpm_option_get_logcb_ctx(klpm_handle_t *handle);

/** Sets the callback used for logging.
 * @param handle the context handle
 * @param cb the cb to use
 * @param ctx user-provided context to pass to cb
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_logcb(klpm_handle_t *handle, klpm_cb_log cb, void *ctx);

/** Returns the callback used to report download progress.
 * @param handle the context handle
 * @return the currently set download callback
 */
klpm_cb_download klpm_option_get_dlcb(klpm_handle_t *handle);

/** Returns the callback used to report download progress.
 * @param handle the context handle
 * @return the currently set download callback context
 */
void *klpm_option_get_dlcb_ctx(klpm_handle_t *handle);

/** Sets the callback used to report download progress.
 * @param handle the context handle
 * @param cb the cb to use
 * @param ctx user-provided context to pass to cb
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_dlcb(klpm_handle_t *handle, klpm_cb_download cb, void *ctx);

/** Returns the downloading callback.
 * @param handle the context handle
 * @return the currently set fetch callback
 */
klpm_cb_fetch klpm_option_get_fetchcb(klpm_handle_t *handle);

/** Returns the downloading callback.
 * @param handle the context handle
 * @return the currently set fetch callback context
 */
void *klpm_option_get_fetchcb_ctx(klpm_handle_t *handle);

/** Sets the downloading callback.
 * @param handle the context handle
 * @param cb the cb to use
 * @param ctx user-provided context to pass to cb
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_fetchcb(klpm_handle_t *handle, klpm_cb_fetch cb, void *ctx);

/** Returns the callback used for events.
 * @param handle the context handle
 * @return the currently set event callback
 */
klpm_cb_event klpm_option_get_eventcb(klpm_handle_t *handle);

/** Returns the callback used for events.
 * @param handle the context handle
 * @return the currently set event callback context
 */
void *klpm_option_get_eventcb_ctx(klpm_handle_t *handle);

/** Sets the callback used for events.
 * @param handle the context handle
 * @param cb the cb to use
 * @param ctx user-provided context to pass to cb
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_eventcb(klpm_handle_t *handle, klpm_cb_event cb, void *ctx);

/** Returns the callback used for questions.
 * @param handle the context handle
 * @return the currently set question callback
 */
klpm_cb_question klpm_option_get_questioncb(klpm_handle_t *handle);

/** Returns the callback used for questions.
 * @param handle the context handle
 * @return the currently set question callback context
 */
void *klpm_option_get_questioncb_ctx(klpm_handle_t *handle);

/** Sets the callback used for questions.
 * @param handle the context handle
 * @param cb the cb to use
 * @param ctx user-provided context to pass to cb
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_questioncb(klpm_handle_t *handle, klpm_cb_question cb, void *ctx);

/**Returns the callback used for operation progress.
 * @param handle the context handle
 * @return the currently set progress callback
 */
klpm_cb_progress klpm_option_get_progresscb(klpm_handle_t *handle);

/**Returns the callback used for operation progress.
 * @param handle the context handle
 * @return the currently set progress callback context
 */
void *klpm_option_get_progresscb_ctx(klpm_handle_t *handle);

/** Sets the callback used for operation progress.
 * @param handle the context handle
 * @param cb the cb to use
 * @param ctx user-provided context to pass to cb
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_progresscb(klpm_handle_t *handle, klpm_cb_progress cb, void *ctx);
/* End of callback accessors */
/** @} */


/** @name Accessors to the root directory
 *
 * The root directory is the prefix to which libkuzpkg installs packages to.
 * Hooks and scriptlets will also be run in a chroot to ensure they behave correctly
 * in alternative roots.
 * @{
 */

/** Returns the root path. Read-only.
 * @param handle the context handle
 */
const char *klpm_option_get_root(klpm_handle_t *handle);
/* End of root accessors */
/** @} */


/** @name Accessors to the database path
 *
 * The dbpath is where libkuzpkg stores the local db and
 * downloads sync databases.
 * @{
 */

/** Returns the path to the database directory. Read-only.
 * @param handle the context handle
 */
const char *klpm_option_get_dbpath(klpm_handle_t *handle);
/* End of dbpath accessors */
/** @} */


/** @name Accessors to the lockfile
 *
 * The lockfile is used to ensure two instances of libkuzpkg can not write
 * to the database at the same time. The lock file is created when
 * committing a transaction and released when the transaction completes.
 * Or when calling \link klpm_unlock \endlink.
 * @{
 */

/** Get the name of the database lock file. Read-only.
 * This is the name that the lockfile would have. It does not
 * matter if the lockfile actually exists on disk.
 * @param handle the context handle
 */
const char *klpm_option_get_lockfile(klpm_handle_t *handle);
/* End of lockfile accessors */
/** @} */

/** @name Accessors to the list of package cache directories.
 *
 * This is where libkuzpkg will store downloaded packages.
 * @{
 */

/** Gets the currently configured cachedirs,
 * @param handle the context handle
 * @return a char* list of cache directories
 */
klpm_list_t *klpm_option_get_cachedirs(klpm_handle_t *handle);

/** Sets the cachedirs.
 * @param handle the context handle
 * @param cachedirs a char* list of cachdirs. The list will be duped and
 * the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_cachedirs(klpm_handle_t *handle, klpm_list_t *cachedirs);

/** Append a cachedir to the configured cachedirs.
 * @param handle the context handle
 * @param cachedir the cachedir to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_cachedir(klpm_handle_t *handle, const char *cachedir);

/** Remove a cachedir from the configured cachedirs.
 * @param handle the context handle
 * @param cachedir the cachedir to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_cachedir(klpm_handle_t *handle, const char *cachedir);
/* End of cachedir accessors */
/** @} */


/** @name Accessors to the list of package hook directories.
 *
 * libkuzpkg will search these directories for hooks to run. A hook in
 * a later directory will override previous hooks if they have the same name.
 * @{
 */

/** Gets the currently configured hookdirs,
 * @param handle the context handle
 * @return a char* list of hook directories
 */
klpm_list_t *klpm_option_get_hookdirs(klpm_handle_t *handle);

/** Sets the hookdirs.
 * @param handle the context handle
 * @param hookdirs a char* list of hookdirs. The list will be duped and
 * the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_hookdirs(klpm_handle_t *handle, klpm_list_t *hookdirs);

/** Append a hookdir to the configured hookdirs.
 * @param handle the context handle
 * @param hookdir the hookdir to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_hookdir(klpm_handle_t *handle, const char *hookdir);

/** Remove a hookdir from the configured hookdirs.
 * @param handle the context handle
 * @param hookdir the hookdir to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_hookdir(klpm_handle_t *handle, const char *hookdir);
/* End of hookdir accessors */
/** @} */


/** @name Accessors to the list of overwritable files.
 *
 * Normally libkuzpkg will refuse to install a package that owns files that
 * are already on disk and not owned by that package.
 *
 * If a conflicting file matches a glob in the overwrite_files list, then no
 * conflict will be raised and libkuzpkg will simply overwrite the file.
 * @{
 */

/** Gets the currently configured overwritable files,
 * @param handle the context handle
 * @return a char* list of overwritable file globs
 */
klpm_list_t *klpm_option_get_overwrite_files(klpm_handle_t *handle);

/** Sets the overwritable files.
 * @param handle the context handle
 * @param globs a char* list of overwritable file globs. The list will be duped and
 * the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_overwrite_files(klpm_handle_t *handle, klpm_list_t *globs);

/** Append an overwritable file to the configured overwritable files.
 * @param handle the context handle
 * @param glob the file glob to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_overwrite_file(klpm_handle_t *handle, const char *glob);

/** Remove a file glob from the configured overwritable files globs.
 * @note The overwritable file list contains a list of globs. The glob to
 * remove must exactly match the entry to remove. There is no glob expansion.
 * @param handle the context handle
 * @param glob the file glob to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_overwrite_file(klpm_handle_t *handle, const char *glob);
/* End of overwrite accessors */
/** @} */


/** @name Accessors to the log file
 *
 * This controls where libkuzpkg will save log output to.
 * @{
 */

/** Gets the filepath to the currently set logfile.
 * @param handle the context handle
 * @return the path to the logfile
 */
const char *klpm_option_get_logfile(klpm_handle_t *handle);

/** Sets the logfile path.
 * @param handle the context handle
 * @param logfile path to the new location of the logfile
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_logfile(klpm_handle_t *handle, const char *logfile);
/* End of logfile accessors */
/** @} */


/** @name Accessors to the GPG directory
 *
 * This controls where libkuzpkg will store GnuPG's files.
 * @{
 */

/** Returns the path to libkuzpkg's GnuPG home directory.
 * @param handle the context handle
 * @return the path to libkuzpkgs's GnuPG home directory
 */
const char *klpm_option_get_gpgdir(klpm_handle_t *handle);

/** Sets the path to libkuzpkg's GnuPG home directory.
 * @param handle the context handle
 * @param gpgdir the gpgdir to set
 */
int klpm_option_set_gpgdir(klpm_handle_t *handle, const char *gpgdir);
/* End of gpgdir accessors */
/** @} */


/** @name Accessors for use sandboxuser
 *
 *  This controls the user that libkuzpkg will use for sensitive operations like
 *  downloading files.
 * @{
 */

/** Returns the user to switch to for sensitive operations.
 * @return the user name
 */
const char *klpm_option_get_sandboxuser(klpm_handle_t *handle);

/** Sets the user to switch to for sensitive operations.
 * @param handle the context handle
 * @param sandboxuser the user to set
 */
int klpm_option_set_sandboxuser(klpm_handle_t *handle, const char *sandboxuser);

/* End of sandboxuser accessors */
/** @} */


/** @name Accessors for use syslog
 *
 * This controls whether libkuzpkg will also use the syslog. Even if this option
 * is enabled, libkuzpkg will still try to log to its log file.
 * @{
 */

/** Returns whether to use syslog (0 is FALSE, TRUE otherwise).
 * @param handle the context handle
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_get_usesyslog(klpm_handle_t *handle);

/** Sets whether to use syslog (0 is FALSE, TRUE otherwise).
 * @param handle the context handle
 * @param usesyslog whether to use the syslog (0 is FALSE, TRUE otherwise)
 */
int klpm_option_set_usesyslog(klpm_handle_t *handle, int usesyslog);
/* End of usesyslog accessors */
/** @} */


/** @name Accessors to the list of no-upgrade files.
 * These functions modify the list of files which should
 * not be updated by package installation.
 * @{
 */

/** Get the list of no-upgrade files
 * @param handle the context handle
 * @return the char* list of no-upgrade files
 */
klpm_list_t *klpm_option_get_noupgrades(klpm_handle_t *handle);

/** Add a file to the no-upgrade list
 * @param handle the context handle
 * @param path the path to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_noupgrade(klpm_handle_t *handle, const char *path);

/** Sets the list of no-upgrade files
 * @param handle the context handle
 * @param noupgrade a char* list of file to not upgrade.
 * The list will be duped and the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_noupgrades(klpm_handle_t *handle, klpm_list_t *noupgrade);

/** Remove an entry from the no-upgrade list
 * @param handle the context handle
 * @param path the path to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_noupgrade(klpm_handle_t *handle, const char *path);

/** Test if a path matches any of the globs in the no-upgrade list
 * @param handle the context handle
 * @param path the path to test
 * @return 0 is the path matches a glob, negative if there is no match and
 * positive is the  match was inverted
 */
int klpm_option_match_noupgrade(klpm_handle_t *handle, const char *path);
/* End of noupgrade accessors */
/** @} */


/** @name Accessors to the list of no-extract files.
 * These functions modify the list of filenames which should
 * be skipped packages which should
 * not be upgraded by a sysupgrade operation.
 * @{
 */

/** Get the list of no-extract files
 * @param handle the context handle
 * @return the char* list of no-extract files
 */
klpm_list_t *klpm_option_get_noextracts(klpm_handle_t *handle);

/** Add a file to the no-extract list
 * @param handle the context handle
 * @param path the path to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_noextract(klpm_handle_t *handle, const char *path);

/** Sets the list of no-extract files
 * @param handle the context handle
 * @param noextract a char* list of file to not extract.
 * The list will be duped and the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_noextracts(klpm_handle_t *handle, klpm_list_t *noextract);

/** Remove an entry from the no-extract list
 * @param handle the context handle
 * @param path the path to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_noextract(klpm_handle_t *handle, const char *path);

/** Test if a path matches any of the globs in the no-extract list
 * @param handle the context handle
 * @param path the path to test
 * @return 0 is the path matches a glob, negative if there is no match and
 * positive is the  match was inverted
 */
int klpm_option_match_noextract(klpm_handle_t *handle, const char *path);
/* End of noextract accessors */
/** @} */


/** @name Accessors to the list of ignored packages.
 * These functions modify the list of packages that
 * should be ignored by a sysupgrade.
 *
 * Entries in this list may be globs and only match the package's
 * name. Providers are not taken into account.
 * @{
 */

/** Get the list of ignored packages
 * @param handle the context handle
 * @return the char* list of ignored packages
 */
klpm_list_t *klpm_option_get_ignorepkgs(klpm_handle_t *handle);

/** Add a file to the ignored package list
 * @param handle the context handle
 * @param pkg the package to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_ignorepkg(klpm_handle_t *handle, const char *pkg);

/** Sets the list of packages to ignore
 * @param handle the context handle
 * @param ignorepkgs a char* list of packages to ignore
 * The list will be duped and the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_ignorepkgs(klpm_handle_t *handle, klpm_list_t *ignorepkgs);

/** Remove an entry from the ignorepkg list
 * @param handle the context handle
 * @param pkg the package to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_ignorepkg(klpm_handle_t *handle, const char *pkg);
/* End of ignorepkg accessors */
/** @} */


/** @name Accessors to the list of ignored groups.
 * These functions modify the list of groups whose packages
 * should be ignored by a sysupgrade.
 *
 * Entries in this list may be globs.
 * @{
 */

/** Get the list of ignored groups
 * @param handle the context handle
 * @return the char* list of ignored groups
 */
klpm_list_t *klpm_option_get_ignoregroups(klpm_handle_t *handle);

/** Add a file to the ignored group list
 * @param handle the context handle
 * @param grp the group to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_ignoregroup(klpm_handle_t *handle, const char *grp);

/** Sets the list of groups to ignore
 * @param handle the context handle
 * @param ignoregrps a char* list of groups to ignore
 * The list will be duped and the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_ignoregroups(klpm_handle_t *handle, klpm_list_t *ignoregrps);

/** Remove an entry from the ignoregroup list
 * @param handle the context handle
 * @param grp the group to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_ignoregroup(klpm_handle_t *handle, const char *grp);
/* End of ignoregroup accessors */
/** @} */


/** @name Accessors to the list of ignored dependencies.
 * These functions modify the list of dependencies that
 * should be ignored by a sysupgrade.
 *
 * This is effectively a list of virtual providers that
 * packages can use to satisfy their dependencies.
 * @{
 */

/** Gets the list of dependencies that are assumed to be met
 * @param handle the context handle
 * @return a list of klpm_depend_t*
 */
klpm_list_t *klpm_option_get_assumeinstalled(klpm_handle_t *handle);

/** Add a depend to the assumed installed list
 * @param handle the context handle
 * @param dep the dependency to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_add_assumeinstalled(klpm_handle_t *handle, const klpm_depend_t *dep);

/** Sets the list of dependencies that are assumed to be met
 * @param handle the context handle
 * @param deps a list of *klpm_depend_t
 * The list will be duped and the original will still need to be freed by the caller.
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_assumeinstalled(klpm_handle_t *handle, klpm_list_t *deps);

/** Remove an entry from the assume installed list
 * @param handle the context handle
 * @param dep the dep to remove
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_remove_assumeinstalled(klpm_handle_t *handle, const klpm_depend_t *dep);
/* End of assunmeinstalled accessors */
/** @} */


/** @name Accessors to the list of allowed architectures.
 * libkuzpkg will only install packages that match one of the configured
 * architectures. The architectures do not need to match the physical
   architecture. They can just be treated as a label.
 * @{
 */

/** Returns the allowed package architecture.
 * @param handle the context handle
 * @return the configured package architectures
 */
klpm_list_t *klpm_option_get_architectures(klpm_handle_t *handle);

/** Adds an allowed package architecture.
 * @param handle the context handle
 * @param arch the architecture to set
 */
int klpm_option_add_architecture(klpm_handle_t *handle, const char *arch);

/** Sets the allowed package architecture.
 * @param handle the context handle
 * @param arches the architecture to set
 */
int klpm_option_set_architectures(klpm_handle_t *handle, klpm_list_t *arches);

/** Removes an allowed package architecture.
 * @param handle the context handle
 * @param arch the architecture to remove
 */
int klpm_option_remove_architecture(klpm_handle_t *handle, const char *arch);

/* End of arch accessors */
/** @} */


/** @name Accessors for check space.
 *
 * This controls whether libkuzpkg will check if there is sufficient before
 * installing packages.
 * @{
 */

/** Get whether or not checking for free space before installing packages is enabled.
 * @param handle the context handle
 * @return 0 if disabled, 1 if enabled
 */
int klpm_option_get_checkspace(klpm_handle_t *handle);

/** Enable/disable checking free space before installing packages.
 * @param handle the context handle
 * @param checkspace 0 for disabled, 1 for enabled
 */
int klpm_option_set_checkspace(klpm_handle_t *handle, int checkspace);
/* End of checkspace accessors */
/** @} */


/** @name Accessors for the database extension
 *
 * This controls the extension used for sync databases. libkuzpkg will use this
 * extension to both lookup remote databases and as the name used when opening
 * reading them.
 *
 * This is useful for file databases. Seems as files can increase the size of
 * a database by quite a lot, a server could hold a database without files under
 * one extension, and another with files under another extension.
 *
 * Which one is downloaded and used then depends on this setting.
 * @{
 */

/** Gets the configured database extension.
 * @param handle the context handle
 * @return the configured database extension
 */
const char *klpm_option_get_dbext(klpm_handle_t *handle);

/** Sets the database extension.
 * @param handle the context handle
 * @param dbext the database extension to use
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_dbext(klpm_handle_t *handle, const char *dbext);
/* End of dbext accessors */
/** @} */


/** @name Accessors for the signature levels
 * @{
 */

/** Get the default siglevel.
 * @param handle the context handle
 * @return a \link klpm_siglevel_t \endlink bitfield of the siglevel
 */
int klpm_option_get_default_siglevel(klpm_handle_t *handle);

/** Set the default siglevel.
 * @param handle the context handle
 * @param level a \link klpm_siglevel_t \endlink bitfield of the level to set
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_default_siglevel(klpm_handle_t *handle, int level);

/** Get the configured local file siglevel.
 * @param handle the context handle
 * @return a \link klpm_siglevel_t \endlink bitfield of the siglevel
 */
int klpm_option_get_local_file_siglevel(klpm_handle_t *handle);

/** Set the local file siglevel.
 * @param handle the context handle
 * @param level a \link klpm_siglevel_t \endlink bitfield of the level to set
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_local_file_siglevel(klpm_handle_t *handle, int level);

/** Get the configured remote file siglevel.
 * @param handle the context handle
 * @return a \link klpm_siglevel_t \endlink bitfield of the siglevel
 */
int klpm_option_get_remote_file_siglevel(klpm_handle_t *handle);

/** Set the remote file siglevel.
 * @param handle the context handle
 * @param level a \link klpm_siglevel_t \endlink bitfield of the level to set
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_remote_file_siglevel(klpm_handle_t *handle, int level);
/* End of signature accessors */
/** @} */


/** @name Accessors for download timeout
 *
 * By default, libkuzpkg will timeout if a download has been transferring
 * less than 1 byte for 10 seconds.
 * @{
 */

/** Get the download timeout state
 * @param handle the context handle
 * @return 0 for enabled, 1 for disabled
*/
int klpm_option_get_disable_dl_timeout(klpm_handle_t *handle);

/** Enables/disables the download timeout.
 * @param handle the context handle
 * @param disable_dl_timeout 0 for enabled, 1 for disabled
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_disable_dl_timeout(klpm_handle_t *handle, unsigned short disable_dl_timeout);
/* End of disable_dl_timeout accessors */
/** @} */


/** @name Accessors for parallel downloads
 * \link klpm_db_update \endlink, \link klpm_fetch_pkgurl \endlink and
 * \link klpm_trans_commit \endlink can all download packages in parallel.
 * This setting configures how many packages can be downloaded in parallel,
 *
 * By default this value is set to 1, meaning packages are downloading
 * sequentially.
 *
 * @{
 */

/** Gets the number of parallel streams to download database and package files.
 * @param handle the context handle
 * @return the number of parallel streams to download database and package files
 */
int klpm_option_get_parallel_downloads(klpm_handle_t *handle);

/** Sets number of parallel streams to download database and package files.
 * @param handle the context handle
 * @param num_streams number of parallel download streams
 * @return 0 on success, -1 on error
 */
int klpm_option_set_parallel_downloads(klpm_handle_t *handle, unsigned int num_streams);
/* End of parallel_downloads accessors */
/** @} */

/** @name Accessors for sandbox
 *
 * By default, libkuzpkg will sandbox the downloader process.
 * @{
 */

/** Get the state of the sandbox
 * @param handle the context handle
 * @return 0 for enabled, 1 if any component is disabled, 2 if completely disabled
 */
int klpm_option_get_disable_sandbox(klpm_handle_t *handle);

/** Enables/disables all components of the sandbox.
 * @param handle the context handle
 * @param disable_sandbox 0 for enabled, 1 for disabled
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_disable_sandbox(klpm_handle_t *handle, unsigned short disable_sandbox);

/** Get the state of the filesystem part of the sandbox
 * @param handle the context handle
 * @return 0 for enabled, 1 for disabled
 */
int klpm_option_get_disable_sandbox_filesystem(klpm_handle_t *handle);

/** Enables/disables the filesystem part of the sandbox.
 * @param handle the context handle
 * @param disable_sandbox_filesystem 0 for enabled, 1 for disabled
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_disable_sandbox_filesystem(klpm_handle_t *handle, unsigned short disable_sandbox_filesystem);

/** Get the state of the syscalls part of the sandbox
 * @param handle the context handle
 * @return 0 for enabled, 1 for disabled
 */
int klpm_option_get_disable_sandbox_syscalls(klpm_handle_t *handle);

/** Enables/disables the syscalls part of the sandbox.
 * @param handle the context handle
 * @param disable_sandbox_syscalls 0 for enabled, 1 for disabled
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_option_set_disable_sandbox_syscalls(klpm_handle_t *handle, unsigned short disable_sandbox_syscalls);

/* End of disable_sandbox accessors */
/** @} */

/* End of libkuzpkg_options */
/** @} */


/** @addtogroup libkuzpkg_packages Package Functions
 * Functions to manipulate libkuzpkg packages
 * @{
 */

/** Package install reasons. */
typedef enum _klpm_pkgreason_t {
	/** Explicitly requested by the user. */
	KUZPKG_PKG_REASON_EXPLICIT = 0,
	/** Installed as a dependency for another package. */
	KUZPKG_PKG_REASON_DEPEND = 1,
	/** Failed parsing of local database */
	KUZPKG_PKG_REASON_UNKNOWN = 2
} klpm_pkgreason_t;

/** Location a package object was loaded from. */
typedef enum _klpm_pkgfrom_t {
	/** Loaded from a file via \link klpm_pkg_load \endlink */
	KUZPKG_PKG_FROM_FILE = 1,
	/** From the local database */
	KUZPKG_PKG_FROM_LOCALDB,
	/** From a sync database */
	KUZPKG_PKG_FROM_SYNCDB
} klpm_pkgfrom_t;


/** Method used to validate a package. */
typedef enum _klpm_pkgvalidation_t {
	/** The package's validation type is unknown */
	KUZPKG_PKG_VALIDATION_UNKNOWN = 0,
	/** The package does not have any validation */
	KUZPKG_PKG_VALIDATION_NONE = (1 << 0),
	/** The package is validated with md5 */
	KUZPKG_PKG_VALIDATION_MD5SUM = (1 << 1),
	/** The package is validated with sha256 */
	KUZPKG_PKG_VALIDATION_SHA256SUM = (1 << 2),
	/** The package is validated with a PGP signature */
	KUZPKG_PKG_VALIDATION_SIGNATURE = (1 << 3)
} klpm_pkgvalidation_t;

/** Create a package from a file.
 * If full is false, the archive is read only until all necessary
 * metadata is found. If it is true, the entire archive is read, which
 * serves as a verification of integrity and the filelist can be created.
 * The allocated structure should be freed using klpm_pkg_free().
 * @param handle the context handle
 * @param filename location of the package tarball
 * @param full whether to stop the load after metadata is read or continue
 * through the full archive
 * @param level what level of package signature checking to perform on the
 * package; note that this must be a '.sig' file type verification
 * @param pkg address of the package pointer
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_pkg_load(klpm_handle_t *handle, const char *filename, int full,
		int level, klpm_pkg_t **pkg);

/** Fetch a list of remote packages.
 * @param handle the context handle
 * @param urls list of package URLs to download
 * @param fetched list of filepaths to the fetched packages, each item
 *    corresponds to one in `urls` list. This is an output parameter,
 *    the caller should provide a pointer to an empty list
 *    (*fetched === NULL) and the callee fills the list with data.
 * @return 0 on success or -1 on failure
 */
int klpm_fetch_pkgurl(klpm_handle_t *handle, const klpm_list_t *urls,
	  klpm_list_t **fetched);

/** Find a package in a list by name.
 * @param haystack a list of klpm_pkg_t
 * @param needle the package name
 * @return a pointer to the package if found or NULL
 */
klpm_pkg_t *klpm_pkg_find(klpm_list_t *haystack, const char *needle);

/** Free a package.
 * Only packages loaded with \link klpm_pkg_load \endlink can be freed.
 * Packages from databases will be freed by libkuzpkg when they are unregistered.
 * @param pkg package pointer to free
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_pkg_free(klpm_pkg_t *pkg);

/** Compare two version strings and determine which one is 'newer'.
 * Returns a value comparable to the way strcmp works. Returns 1
 * if a is newer than b, 0 if a and b are the same version, or -1
 * if b is newer than a.
 *
 * Different epoch values for version strings will override any further
 * comparison. If no epoch is provided, 0 is assumed.
 *
 * Keep in mind that the pkgrel is only compared if it is available
 * on both versions handed to this function. For example, comparing
 * 1.5-1 and 1.5 will yield 0; comparing 1.5-1 and 1.5-2 will yield
 * -1 as expected. This is mainly for supporting versioned dependencies
 * that do not include the pkgrel.
 */
int klpm_pkg_vercmp(const char *a, const char *b);

/** Computes the list of packages requiring a given package.
 * The return value of this function is a newly allocated
 * list of package names (char*), it should be freed by the caller.
 * @param pkg a package
 * @return the list of packages requiring pkg
 */
klpm_list_t *klpm_pkg_compute_requiredby(klpm_pkg_t *pkg);

/** Computes the list of packages optionally requiring a given package.
 * The return value of this function is a newly allocated
 * list of package names (char*), it should be freed by the caller.
 * @param pkg a package
 * @return the list of packages optionally requiring pkg
 */
klpm_list_t *klpm_pkg_compute_optionalfor(klpm_pkg_t *pkg);

/** Test if a package should be ignored.
 * Checks if the package is ignored via IgnorePkg, or if the package is
 * in a group ignored via IgnoreGroup.
 * @param handle the context handle
 * @param pkg the package to test
 * @return 1 if the package should be ignored, 0 otherwise
 */
int klpm_pkg_should_ignore(klpm_handle_t *handle, klpm_pkg_t *pkg);

/** @name Package Property Accessors
 * Any pointer returned by these functions points to internal structures
 * allocated by libkuzpkg. They should not be freed nor modified in any
 * way.
 *
 * For loaded packages, they will be freed when \link klpm_pkg_free \endlink is called.
 * For database packages, they will be freed when the database is unregistered.
 * @{
 */

/** Gets the handle of a package
 * @param pkg a pointer to package
 * @return the klpm handle that the package belongs to
 */
klpm_handle_t *klpm_pkg_get_handle(klpm_pkg_t *pkg);

/** Gets the name of the file from which the package was loaded.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_filename(klpm_pkg_t *pkg);

/** Returns the package base name.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_base(klpm_pkg_t *pkg);

/** Returns the package name.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_name(klpm_pkg_t *pkg);

/** Returns the package version as a string.
 * This includes all available epoch, version, and pkgrel components. Use
 * klpm_pkg_vercmp() to compare version strings if necessary.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_version(klpm_pkg_t *pkg);

/** Returns the origin of the package.
 * @return an klpm_pkgfrom_t constant, -1 on error
 */
klpm_pkgfrom_t klpm_pkg_get_origin(klpm_pkg_t *pkg);

/** Returns the package description.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_desc(klpm_pkg_t *pkg);

/** Returns the package URL.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_url(klpm_pkg_t *pkg);

/** Returns the build timestamp of the package.
 * @param pkg a pointer to package
 * @return the timestamp of the build time
 */
klpm_time_t klpm_pkg_get_builddate(klpm_pkg_t *pkg);

/** Returns the install timestamp of the package.
 * @param pkg a pointer to package
 * @return the timestamp of the install time
 */
klpm_time_t klpm_pkg_get_installdate(klpm_pkg_t *pkg);

/** Returns the packager's name.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_packager(klpm_pkg_t *pkg);

/** Returns the package's SHA256 checksum as a string.
 * The returned string is a sequence of 64 lowercase hexadecimal digits.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_sha256sum(klpm_pkg_t *pkg);

/** Returns the architecture for which the package was built.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_arch(klpm_pkg_t *pkg);

/** Returns the size of the package. This is only available for sync database
 * packages and package files, not those loaded from the local database.
 * @param pkg a pointer to package
 * @return the size of the package in bytes.
 */
off_t klpm_pkg_get_size(klpm_pkg_t *pkg);

/** Returns the installed size of the package.
 * @param pkg a pointer to package
 * @return the total size of files installed by the package.
 */
off_t klpm_pkg_get_isize(klpm_pkg_t *pkg);

/** Returns the package installation reason.
 * @param pkg a pointer to package
 * @return an enum member giving the install reason.
 */
klpm_pkgreason_t klpm_pkg_get_reason(klpm_pkg_t *pkg);

/** Returns the list of package licenses.
 * @param pkg a pointer to package
 * @return a pointer to an internal list of strings.
 */
klpm_list_t *klpm_pkg_get_licenses(klpm_pkg_t *pkg);

/** Returns the list of package groups.
 * @param pkg a pointer to package
 * @return a pointer to an internal list of strings.
 */
klpm_list_t *klpm_pkg_get_groups(klpm_pkg_t *pkg);

/** Returns the list of package dependencies as klpm_depend_t.
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_depends(klpm_pkg_t *pkg);

/** Returns the list of package optional dependencies.
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_optdepends(klpm_pkg_t *pkg);

/** Returns a list of package check dependencies
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_checkdepends(klpm_pkg_t *pkg);

/** Returns a list of package make dependencies
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_makedepends(klpm_pkg_t *pkg);

/** Returns the list of packages conflicting with pkg.
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_conflicts(klpm_pkg_t *pkg);

/** Returns the list of packages provided by pkg.
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_provides(klpm_pkg_t *pkg);

/** Returns the list of packages to be replaced by pkg.
 * @param pkg a pointer to package
 * @return a reference to an internal list of klpm_depend_t structures.
 */
klpm_list_t *klpm_pkg_get_replaces(klpm_pkg_t *pkg);

/** Returns the list of files installed by pkg.
 * The filenames are relative to the install root,
 * and do not include leading slashes.
 * @param pkg a pointer to package
 * @return a pointer to a filelist object containing a count and an array of
 * package file objects
 */
klpm_filelist_t *klpm_pkg_get_files(klpm_pkg_t *pkg);

/** Returns the list of files backed up when installing pkg.
 * @param pkg a pointer to package
 * @return a reference to a list of klpm_backup_t objects
 */
klpm_list_t *klpm_pkg_get_backup(klpm_pkg_t *pkg);

/** Returns the database containing pkg.
 * Returns a pointer to the klpm_db_t structure the package is
 * originating from, or NULL if the package was loaded from a file.
 * @param pkg a pointer to package
 * @return a pointer to the DB containing pkg, or NULL.
 */
klpm_db_t *klpm_pkg_get_db(klpm_pkg_t *pkg);

/** Returns the base64 encoded package signature.
 * @param pkg a pointer to package
 * @return a reference to an internal string
 */
const char *klpm_pkg_get_base64_sig(klpm_pkg_t *pkg);

/** Extracts package signature either from embedded package signature
 * or if it is absent then reads data from detached signature file.
 * @param pkg a pointer to package.
 * @param sig output parameter for signature data. Callee function allocates
 * a buffer needed for the signature data. Caller is responsible for
 * freeing this buffer.
 * @param sig_len output parameter for the signature data length.
 * @return 0 on success, negative number on error.
 */
int klpm_pkg_get_sig(klpm_pkg_t *pkg, unsigned char **sig, size_t *sig_len);

/** Returns the method used to validate a package during install.
 * @param pkg a pointer to package
 * @return an enum member giving the validation method
 */
int klpm_pkg_get_validation(klpm_pkg_t *pkg);

/** Gets the extended data field of a package.
 * @param pkg a pointer to package
 * @return a reference to a list of klpm_pkg_xdata_t objects
 */
klpm_list_t *klpm_pkg_get_xdata(klpm_pkg_t *pkg);

/** Returns whether the package has an install scriptlet.
 * @return 0 if FALSE, TRUE otherwise
 */
int klpm_pkg_has_scriptlet(klpm_pkg_t *pkg);

/** Returns the size of the files that will be downloaded to install a
 * package.
 * @param newpkg the new package to upgrade to
 * @return the size of the download
 */
off_t klpm_pkg_download_size(klpm_pkg_t *newpkg);

/** Set install reason for a package in the local database.
 * The provided package object must be from the local database or this method
 * will fail. The write to the local database is performed immediately.
 * @param pkg the package to update
 * @param reason the new install reason
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_pkg_set_reason(klpm_pkg_t *pkg, klpm_pkgreason_t reason);


/* End of libkuzpkg_pkg_t accessors */
/** @} */


/** @name Changelog functions
 *  Functions for reading the changelog
 * @{
 */

/** Open a package changelog for reading.
 * Similar to fopen in functionality, except that the returned 'file
 * stream' could really be from an archive as well as from the database.
 * @param pkg the package to read the changelog of (either file or db)
 * @return a 'file stream' to the package changelog
 */
void *klpm_pkg_changelog_open(klpm_pkg_t *pkg);

/** Read data from an open changelog 'file stream'.
 * Similar to fread in functionality, this function takes a buffer and
 * amount of data to read. If an error occurs pm_errno will be set.
 * @param ptr a buffer to fill with raw changelog data
 * @param size the size of the buffer
 * @param pkg the package that the changelog is being read from
 * @param fp a 'file stream' to the package changelog
 * @return the number of characters read, or 0 if there is no more data or an
 * error occurred.
 */
size_t klpm_pkg_changelog_read(void *ptr, size_t size,
		const klpm_pkg_t *pkg, void *fp);

/** Close a package changelog for reading.
 * @param pkg the package to close the changelog of (either file or db)
 * @param fp the 'file stream' to the package changelog to close
 * @return 0 on success, -1 on error
 */
int klpm_pkg_changelog_close(const klpm_pkg_t *pkg, void *fp);

/* End of changelog accessors */
/** @} */


/** @name Mtree functions
 *  Functions for reading the mtree
 * @{
 */

/** Open a package mtree file for reading.
 * @param pkg the local package to read the mtree of
 * @return an archive structure for the package mtree file
 */
struct archive *klpm_pkg_mtree_open(klpm_pkg_t *pkg);

/** Read next entry from a package mtree file.
 * @param pkg the package that the mtree file is being read from
 * @param archive the archive structure reading from the mtree file
 * @param entry an archive_entry to store the entry header information
 * @return 0 on success, 1 if end of archive is reached, -1 otherwise.
 */
int klpm_pkg_mtree_next(const klpm_pkg_t *pkg, struct archive *archive,
		struct archive_entry **entry);

/** Close a package mtree file.
 * @param pkg the local package to close the mtree of
 * @param archive the archive to close
 */
int klpm_pkg_mtree_close(const klpm_pkg_t *pkg, struct archive *archive);

/* End of mtree accessors */
/** @} */


/* End of libkuzpkg_packages */
/** @} */

/** @addtogroup libkuzpkg_trans Transaction
 * @brief Functions to manipulate libkuzpkg transactions
 *
 * Transactions are the way to add/remove packages to/from the system.
 * Only one transaction can exist at a time.
 *
 * The basic workflow of a transaction is to:
 *
 * - Initialize with \link klpm_trans_init \endlink
 * - Choose which packages to add with \link klpm_add_pkg \endlink and \link klpm_remove_pkg \endlink
 * - Prepare the transaction with \link klpm_trans_prepare \endlink
 * - Commit the transaction with \link klpm_trans_commit \endlink
 * - Release the transaction with \link klpm_trans_release \endlink
 *
 * A transaction can be released at any time. A transaction does not have to be committed.
 * @{
 */

/** Transaction flags */
typedef enum _klpm_transflag_t {
	/** Ignore dependency checks. */
	KUZPKG_TRANS_FLAG_NODEPS = 1,
	/* (1 << 1) flag can go here */
	/** Delete files even if they are tagged as backup. */
	KUZPKG_TRANS_FLAG_NOSAVE = (1 << 2),
	/** Ignore version numbers when checking dependencies. */
	KUZPKG_TRANS_FLAG_NODEPVERSION = (1 << 3),
	/** Remove also any packages depending on a package being removed. */
	KUZPKG_TRANS_FLAG_CASCADE = (1 << 4),
	/** Remove packages and their unneeded deps (not explicitly installed). */
	KUZPKG_TRANS_FLAG_RECURSE = (1 << 5),
	/** Modify database but do not commit changes to the filesystem. */
	KUZPKG_TRANS_FLAG_DBONLY = (1 << 6),
	/** Do not run hooks during a transaction */
	KUZPKG_TRANS_FLAG_NOHOOKS = (1 << 7),
	/** Use KUZPKG_PKG_REASON_DEPEND when installing packages. */
	KUZPKG_TRANS_FLAG_ALLDEPS = (1 << 8),
	/** Only download packages and do not actually install. */
	KUZPKG_TRANS_FLAG_DOWNLOADONLY = (1 << 9),
	/** Do not execute install scriptlets after installing. */
	KUZPKG_TRANS_FLAG_NOSCRIPTLET = (1 << 10),
	/** Ignore dependency conflicts. */
	KUZPKG_TRANS_FLAG_NOCONFLICTS = (1 << 11),
	/* (1 << 12) flag can go here */
	/** Do not install a package if it is already installed and up to date. */
	KUZPKG_TRANS_FLAG_NEEDED = (1 << 13),
	/** Use KUZPKG_PKG_REASON_EXPLICIT when installing packages. */
	KUZPKG_TRANS_FLAG_ALLEXPLICIT = (1 << 14),
	/** Do not remove a package if it is needed by another one. */
	KUZPKG_TRANS_FLAG_UNNEEDED = (1 << 15),
	/** Remove also explicitly installed unneeded deps (use with KUZPKG_TRANS_FLAG_RECURSE). */
	KUZPKG_TRANS_FLAG_RECURSEALL = (1 << 16),
	/** Do not lock the database during the operation. */
	KUZPKG_TRANS_FLAG_NOLOCK = (1 << 17)
} klpm_transflag_t;

/** Returns the bitfield of flags for the current transaction.
 * @param handle the context handle
 * @return the bitfield of transaction flags
 */
int klpm_trans_get_flags(klpm_handle_t *handle);

/** Returns a list of packages added by the transaction.
 * @param handle the context handle
 * @return a list of klpm_pkg_t structures
 */
klpm_list_t *klpm_trans_get_add(klpm_handle_t *handle);

/** Returns the list of packages removed by the transaction.
 * @param handle the context handle
 * @return a list of klpm_pkg_t structures
 */
klpm_list_t *klpm_trans_get_remove(klpm_handle_t *handle);

/** Initialize the transaction.
 * @param handle the context handle
 * @param flags flags of the transaction (like nodeps, etc; see klpm_transflag_t)
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_trans_init(klpm_handle_t *handle, int flags);

/** Prepare a transaction.
 * @param handle the context handle
 * @param data the address of an klpm_list where a list
 * of klpm_depmissing_t objects is dumped (conflicting packages)
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_trans_prepare(klpm_handle_t *handle, klpm_list_t **data);

/** Commit a transaction.
 * @param handle the context handle
 * @param data the address of an klpm_list where detailed description
 * of an error can be dumped (i.e. list of conflicting files)
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_trans_commit(klpm_handle_t *handle, klpm_list_t **data);

/** Interrupt a transaction.
 * @param handle the context handle
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_trans_interrupt(klpm_handle_t *handle);

/** Release a transaction.
 * @param handle the context handle
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_trans_release(klpm_handle_t *handle);

/** @name Add/Remove packages
 * These functions remove/add packages to the transactions
 * @{
 * */

/** Search for packages to upgrade and add them to the transaction.
 * @param handle the context handle
 * @param enable_downgrade allow downgrading of packages if the remote version is lower
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_sync_sysupgrade(klpm_handle_t *handle, int enable_downgrade);

/** Add a package to the transaction.
 * If the package was loaded by klpm_pkg_load(), it will be freed upon
 * \link klpm_trans_release \endlink invocation.
 * @param handle the context handle
 * @param pkg the package to add
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_add_pkg(klpm_handle_t *handle, klpm_pkg_t *pkg);

/** Add a package removal to the transaction.
 * @param handle the context handle
 * @param pkg the package to uninstall
 * @return 0 on success, -1 on error (pm_errno is set accordingly)
 */
int klpm_remove_pkg(klpm_handle_t *handle, klpm_pkg_t *pkg);

/* End of add/remove packages */
/** @} */


/* End of libkuzpkg_trans */
/** @} */


/** \addtogroup libkuzpkg_misc Miscellaneous Functions
 * @brief Various libkuzpkg functions
 * @{
 */

/** Check for new version of pkg in syncdbs.
 *
 * If the same package appears multiple dbs only the first will be checked
 *
 * This only checks the syncdb for a newer version. It does not access the network at all.
 * See \link klpm_db_update \endlink to update a database.
 */
klpm_pkg_t *klpm_sync_get_new_version(klpm_pkg_t *pkg, klpm_list_t *dbs_sync);

/** Get the md5 sum of file.
 * @param filename name of the file
 * @return the checksum on success, NULL on error
 */
char *klpm_compute_md5sum(const char *filename);

/** Get the sha256 sum of file.
 * @param filename name of the file
 * @return the checksum on success, NULL on error
 */
char *klpm_compute_sha256sum(const char *filename);

/** Remove the database lock file
 * @param handle the context handle
 * @return 0 on success, -1 on error
 *
 * @note Safe to call from inside signal handlers.
 */
int klpm_unlock(klpm_handle_t *handle);

/** Enum of possible compile time features */
enum klpm_caps {
        /** localization */
        KUZPKG_CAPABILITY_NLS = (1 << 0),
        /** Ability to download */
        KUZPKG_CAPABILITY_DOWNLOADER = (1 << 1),
        /** Signature checking */
        KUZPKG_CAPABILITY_SIGNATURES = (1 << 2)
};

/** Get the version of library.
 * @return the library version, e.g. "6.0.4"
 * */
const char *klpm_version(void);

/** Get the capabilities of the library.
 * @return a bitmask of the capabilities
 * */
int klpm_capabilities(void);

/** Drop privileges by switching to a different user.
 * @param handle the context handle
 * @param sandboxuser the user to switch to
 * @param sandbox_path if non-NULL, restrict writes to this filesystem path
 * @param restrict_syscalls whether to deny access to a list of dangerous syscalls
 * @return 0 on success, -1 on failure
 */
int klpm_sandbox_setup_child(klpm_handle_t *handle, const char *sandboxuser, const char *sandbox_path, bool restrict_syscalls);

/* End of libkuzpkg_misc */
/** @} */

/* End of libkuzpkg_api */
/** @} */

#ifdef __cplusplus
}
#endif
#endif /* KUZPKG_H */
