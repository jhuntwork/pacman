/*
 *  conf.c
 *
 *  Copyright (c) 2006-2013 Pacman Development Team <pacman-dev@archlinux.org>
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
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

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <locale.h> /* setlocale */
#include <fcntl.h> /* open */
#include <stdlib.h>
#include <stdio.h>
#include <string.h> /* strdup */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h> /* uname */
#include <unistd.h>

/* pacman */
#include "conf.h"
#include "util.h"
#include "pacman.h"
#include "callback.h"

/* global config variable */
config_t *config = NULL;

#define NOCOLOR       "\033[0m"

#define BOLD          "\033[0;1m"

#define BLACK         "\033[0;30m"
#define RED           "\033[0;31m"
#define GREEN         "\033[0;32m"
#define YELLOW        "\033[0;33m"
#define BLUE          "\033[0;34m"
#define MAGENTA       "\033[0;35m"
#define CYAN          "\033[0;36m"
#define WHITE         "\033[0;37m"

#define BOLDBLACK     "\033[1;30m"
#define BOLDRED       "\033[1;31m"
#define BOLDGREEN     "\033[1;32m"
#define BOLDYELLOW    "\033[1;33m"
#define BOLDBLUE      "\033[1;34m"
#define BOLDMAGENTA   "\033[1;35m"
#define BOLDCYAN      "\033[1;36m"
#define BOLDWHITE     "\033[1;37m"

void enable_colors(int colors)
{
	colstr_t *colstr = &config->colstr;

	if(colors == PM_COLOR_ON) {
		colstr->colon   = BOLDBLUE "::" BOLD " ";
		colstr->title   = BOLD;
		colstr->repo    = BOLDMAGENTA;
		colstr->version = BOLDGREEN;
		colstr->groups  = BOLDBLUE;
		colstr->meta    = BOLDCYAN;
		colstr->warn    = BOLDYELLOW;
		colstr->err     = BOLDRED;
		colstr->nocolor = NOCOLOR;
	}
}

config_t *config_new(void)
{
	config_t *newconfig = calloc(1, sizeof(config_t));
	if(!newconfig) {
			pm_printf(ALPM_LOG_ERROR,
					_("malloc failure: could not allocate %zd bytes\n"),
					sizeof(config_t));
			return NULL;
	}
	/* defaults which may get overridden later */
	newconfig->op = PM_OP_MAIN;
	newconfig->logmask = ALPM_LOG_ERROR | ALPM_LOG_WARNING;
	newconfig->configfile = strdup(CONFFILE);
	newconfig->deltaratio = 0.0;
	if(alpm_capabilities() & ALPM_CAPABILITY_SIGNATURES) {
		newconfig->siglevel = ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL |
			ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL;
		newconfig->localfilesiglevel = ALPM_SIG_USE_DEFAULT;
		newconfig->remotefilesiglevel = ALPM_SIG_USE_DEFAULT;
	}

	newconfig->colstr.colon   = ":: ";
	newconfig->colstr.title   = "";
	newconfig->colstr.repo    = "";
	newconfig->colstr.version = "";
	newconfig->colstr.groups  = "";
	newconfig->colstr.meta    = "";
	newconfig->colstr.warn    = "";
	newconfig->colstr.err     = "";
	newconfig->colstr.nocolor = "";

	return newconfig;
}

int config_free(config_t *oldconfig)
{
	if(oldconfig == NULL) {
		return -1;
	}

	alpm_list_free(oldconfig->explicit_adds);
	alpm_list_free(oldconfig->explicit_removes);

	FREELIST(oldconfig->holdpkg);
	FREELIST(oldconfig->ignorepkg);
	FREELIST(oldconfig->ignoregrp);
	FREELIST(oldconfig->noupgrade);
	FREELIST(oldconfig->noextract);
	free(oldconfig->configfile);
	free(oldconfig->rootdir);
	free(oldconfig->dbpath);
	free(oldconfig->logfile);
	free(oldconfig->gpgdir);
	FREELIST(oldconfig->cachedirs);
	free(oldconfig->xfercommand);
	free(oldconfig->print_format);
	free(oldconfig->arch);
	free(oldconfig);

	return 0;
}

/** Helper function for download_with_xfercommand() */
static char *get_filename(const char *url)
{
	char *filename = strrchr(url, '/');
	if(filename != NULL) {
		filename++;
	}
	return filename;
}

/** Helper function for download_with_xfercommand() */
static char *get_destfile(const char *path, const char *filename)
{
	char *destfile;
	/* len = localpath len + filename len + null */
	size_t len = strlen(path) + strlen(filename) + 1;
	destfile = calloc(len, sizeof(char));
	snprintf(destfile, len, "%s%s", path, filename);

	return destfile;
}

/** Helper function for download_with_xfercommand() */
static char *get_tempfile(const char *path, const char *filename)
{
	char *tempfile;
	/* len = localpath len + filename len + '.part' len + null */
	size_t len = strlen(path) + strlen(filename) + 6;
	tempfile = calloc(len, sizeof(char));
	snprintf(tempfile, len, "%s%s.part", path, filename);

	return tempfile;
}

/** External fetch callback */
static int download_with_xfercommand(const char *url, const char *localpath,
		int force)
{
	int ret = 0, retval;
	int usepart = 0;
	int cwdfd;
	struct stat st;
	char *parsedcmd, *tempcmd;
	char *destfile, *tempfile, *filename;

	if(!config->xfercommand) {
		return -1;
	}

	filename = get_filename(url);
	if(!filename) {
		return -1;
	}
	destfile = get_destfile(localpath, filename);
	tempfile = get_tempfile(localpath, filename);

	if(force && stat(tempfile, &st) == 0) {
		unlink(tempfile);
	}
	if(force && stat(destfile, &st) == 0) {
		unlink(destfile);
	}

	tempcmd = strdup(config->xfercommand);
	/* replace all occurrences of %o with fn.part */
	if(strstr(tempcmd, "%o")) {
		usepart = 1;
		parsedcmd = strreplace(tempcmd, "%o", tempfile);
		free(tempcmd);
		tempcmd = parsedcmd;
	}
	/* replace all occurrences of %u with the download URL */
	parsedcmd = strreplace(tempcmd, "%u", url);
	free(tempcmd);

	/* save the cwd so we can restore it later */
	do {
		cwdfd = open(".", O_RDONLY);
	} while(cwdfd == -1 && errno == EINTR);
	if(cwdfd < 0) {
		pm_printf(ALPM_LOG_ERROR, _("could not get current working directory\n"));
	}

	/* cwd to the download directory */
	if(chdir(localpath)) {
		pm_printf(ALPM_LOG_WARNING, _("could not chdir to download directory %s\n"), localpath);
		ret = -1;
		goto cleanup;
	}
	/* execute the parsed command via /bin/sh -c */
	pm_printf(ALPM_LOG_DEBUG, "running command: %s\n", parsedcmd);
	retval = system(parsedcmd);

	if(retval == -1) {
		pm_printf(ALPM_LOG_WARNING, _("running XferCommand: fork failed!\n"));
		ret = -1;
	} else if(retval != 0) {
		/* download failed */
		pm_printf(ALPM_LOG_DEBUG, "XferCommand command returned non-zero status "
				"code (%d)\n", retval);
		ret = -1;
	} else {
		/* download was successful */
		ret = 0;
		if(usepart) {
			if(rename(tempfile, destfile)) {
				pm_printf(ALPM_LOG_ERROR, _("could not rename %s to %s (%s)\n"),
						tempfile, destfile, strerror(errno));
				ret = -1;
			}
		}
	}

cleanup:
	/* restore the old cwd if we have it */
	if(cwdfd >= 0) {
		if(fchdir(cwdfd) != 0) {
			pm_printf(ALPM_LOG_ERROR, _("could not restore working directory (%s)\n"),
					strerror(errno));
		}
		close(cwdfd);
	}

	if(ret == -1) {
		/* hack to let an user the time to cancel a download */
		sleep(2);
	}
	free(destfile);
	free(tempfile);
	free(parsedcmd);

	return ret;
}


int config_set_arch(const char *arch)
{
	if(strcmp(arch, "auto") == 0) {
		struct utsname un;
		uname(&un);
		config->arch = strdup(un.machine);
	} else {
		config->arch = strdup(arch);
	}
	pm_printf(ALPM_LOG_DEBUG, "config: arch: %s\n", config->arch);
	return 0;
}

/**
 * Parse a signature verification level line.
 * @param values the list of parsed option values
 * @param storage location to store the derived signature level; any existing
 * value here is used as a starting point
 * @param file path to the config file
 * @param linenum current line number in file
 * @return 0 on success, 1 on any parsing error
 */
static int process_siglevel(alpm_list_t *values, alpm_siglevel_t *storage,
		const char *file, int linenum)
{
	alpm_siglevel_t level = *storage;
	alpm_list_t *i;
	int ret = 0;

	/* Collapse the option names into a single bitmasked value */
	for(i = values; i; i = alpm_list_next(i)) {
		const char *original = i->data, *value;
		int package = 0, database = 0;

		if(strncmp(original, "Package", strlen("Package")) == 0) {
			/* only packages are affected, don't flip flags for databases */
			value = original + strlen("Package");
			package = 1;
		} else if(strncmp(original, "Database", strlen("Database")) == 0) {
			/* only databases are affected, don't flip flags for packages */
			value = original + strlen("Database");
			database = 1;
		} else {
			/* no prefix, so anything found will affect both packages and dbs */
			value = original;
			package = database = 1;
		}

		/* now parse out and store actual flag if it is valid */
		if(strcmp(value, "Never") == 0) {
			if(package) {
				level &= ~ALPM_SIG_PACKAGE;
				level |= ALPM_SIG_PACKAGE_SET;
			}
			if(database) {
				level &= ~ALPM_SIG_DATABASE;
			}
		} else if(strcmp(value, "Optional") == 0) {
			if(package) {
				level |= ALPM_SIG_PACKAGE;
				level |= ALPM_SIG_PACKAGE_OPTIONAL;
				level |= ALPM_SIG_PACKAGE_SET;
			}
			if(database) {
				level |= ALPM_SIG_DATABASE;
				level |= ALPM_SIG_DATABASE_OPTIONAL;
			}
		} else if(strcmp(value, "Required") == 0) {
			if(package) {
				level |= ALPM_SIG_PACKAGE;
				level &= ~ALPM_SIG_PACKAGE_OPTIONAL;
				level |= ALPM_SIG_PACKAGE_SET;
			}
			if(database) {
				level |= ALPM_SIG_DATABASE;
				level &= ~ALPM_SIG_DATABASE_OPTIONAL;
			}
		} else if(strcmp(value, "TrustedOnly") == 0) {
			if(package) {
				level &= ~ALPM_SIG_PACKAGE_MARGINAL_OK;
				level &= ~ALPM_SIG_PACKAGE_UNKNOWN_OK;
				level |= ALPM_SIG_PACKAGE_TRUST_SET;
			}
			if(database) {
				level &= ~ALPM_SIG_DATABASE_MARGINAL_OK;
				level &= ~ALPM_SIG_DATABASE_UNKNOWN_OK;
			}
		} else if(strcmp(value, "TrustAll") == 0) {
			if(package) {
				level |= ALPM_SIG_PACKAGE_MARGINAL_OK;
				level |= ALPM_SIG_PACKAGE_UNKNOWN_OK;
				level |= ALPM_SIG_PACKAGE_TRUST_SET;
			}
			if(database) {
				level |= ALPM_SIG_DATABASE_MARGINAL_OK;
				level |= ALPM_SIG_DATABASE_UNKNOWN_OK;
			}
		} else {
			pm_printf(ALPM_LOG_ERROR,
					_("config file %s, line %d: invalid value for '%s' : '%s'\n"),
					file, linenum, "SigLevel", original);
			ret = 1;
		}
		level &= ~ALPM_SIG_USE_DEFAULT;
	}

	/* ensure we have sig checking ability and are actually turning it on */
	if(!(alpm_capabilities() & ALPM_CAPABILITY_SIGNATURES) &&
			level & (ALPM_SIG_PACKAGE | ALPM_SIG_DATABASE)) {
		pm_printf(ALPM_LOG_ERROR,
				_("config file %s, line %d: '%s' option invalid, no signature support\n"),
				file, linenum, "SigLevel");
		ret = 1;
	}

	if(!ret) {
		*storage = level;
	}
	return ret;
}

/**
 * Merge the package entires of two signature verification levels.
 * @param base initial siglevel
 * @param over overridden siglevel, derived value is stored here
 */
static void merge_siglevel(alpm_siglevel_t *base, alpm_siglevel_t *over)
{
	alpm_siglevel_t level = *over;
	if(level & ALPM_SIG_USE_DEFAULT) {
		level = *base;
	} else {
		if(!(level & ALPM_SIG_PACKAGE_SET)) {
			level |= *base & ALPM_SIG_PACKAGE;
			level |= *base & ALPM_SIG_PACKAGE_OPTIONAL;
		}
		if(!(level & ALPM_SIG_PACKAGE_TRUST_SET)) {
			level |= *base & ALPM_SIG_PACKAGE_MARGINAL_OK;
			level |= *base & ALPM_SIG_PACKAGE_UNKNOWN_OK;
		}
	}

	*over = level;
}

static int process_cleanmethods(alpm_list_t *values,
		const char *file, int linenum)
{
	alpm_list_t *i;
	for(i = values; i; i = alpm_list_next(i)) {
		const char *value = i->data;
		if(strcmp(value, "KeepInstalled") == 0) {
			config->cleanmethod |= PM_CLEAN_KEEPINST;
		} else if(strcmp(value, "KeepCurrent") == 0) {
			config->cleanmethod |= PM_CLEAN_KEEPCUR;
		} else {
			pm_printf(ALPM_LOG_ERROR,
					_("config file %s, line %d: invalid value for '%s' : '%s'\n"),
					file, linenum, "CleanMethod", value);
			return 1;
		}
	}
	return 0;
}

/** Add repeating options such as NoExtract, NoUpgrade, etc to libalpm
 * settings. Refactored out of the parseconfig code since all of them did
 * the exact same thing and duplicated code.
 * @param ptr a pointer to the start of the multiple options
 * @param option the string (friendly) name of the option, used for messages
 * @param list the list to add the option to
 */
static void setrepeatingoption(char *ptr, const char *option,
		alpm_list_t **list)
{
	char *val, *saveptr;

	val = strtok_r(ptr, " ", &saveptr);
	while(val) {
		*list = alpm_list_add(*list, strdup(val));
		pm_printf(ALPM_LOG_DEBUG, "config: %s: %s\n", option, val);
		val = strtok_r(NULL, " ", &saveptr);
	}
}

static int _parse_options(const char *key, char *value,
		const char *file, int linenum)
{
	if(value == NULL) {
		/* options without settings */
		if(strcmp(key, "UseSyslog") == 0) {
			config->usesyslog = 1;
			pm_printf(ALPM_LOG_DEBUG, "config: usesyslog\n");
		} else if(strcmp(key, "ILoveCandy") == 0) {
			config->chomp = 1;
			pm_printf(ALPM_LOG_DEBUG, "config: chomp\n");
		} else if(strcmp(key, "VerbosePkgLists") == 0) {
			config->verbosepkglists = 1;
			pm_printf(ALPM_LOG_DEBUG, "config: verbosepkglists\n");
		} else if(strcmp(key, "UseDelta") == 0) {
			config->deltaratio = 0.7;
			pm_printf(ALPM_LOG_DEBUG, "config: usedelta (default 0.7)\n");
		} else if(strcmp(key, "TotalDownload") == 0) {
			config->totaldownload = 1;
			pm_printf(ALPM_LOG_DEBUG, "config: totaldownload\n");
		} else if(strcmp(key, "CheckSpace") == 0) {
			config->checkspace = 1;
		} else if(strcmp(key, "Color") == 0) {
			if(config->color == PM_COLOR_UNSET) {
				config->color = isatty(fileno(stdout)) ? PM_COLOR_ON : PM_COLOR_OFF;
				enable_colors(config->color);
			}
		} else {
			pm_printf(ALPM_LOG_WARNING,
					_("config file %s, line %d: directive '%s' in section '%s' not recognized.\n"),
					file, linenum, key, "options");
		}
	} else {
		/* options with settings */
		if(strcmp(key, "NoUpgrade") == 0) {
			setrepeatingoption(value, "NoUpgrade", &(config->noupgrade));
		} else if(strcmp(key, "NoExtract") == 0) {
			setrepeatingoption(value, "NoExtract", &(config->noextract));
		} else if(strcmp(key, "IgnorePkg") == 0) {
			setrepeatingoption(value, "IgnorePkg", &(config->ignorepkg));
		} else if(strcmp(key, "IgnoreGroup") == 0) {
			setrepeatingoption(value, "IgnoreGroup", &(config->ignoregrp));
		} else if(strcmp(key, "HoldPkg") == 0) {
			setrepeatingoption(value, "HoldPkg", &(config->holdpkg));
		} else if(strcmp(key, "CacheDir") == 0) {
			setrepeatingoption(value, "CacheDir", &(config->cachedirs));
		} else if(strcmp(key, "Architecture") == 0) {
			if(!config->arch) {
				config_set_arch(value);
			}
		} else if(strcmp(key, "UseDelta") == 0) {
			double ratio;
			char *endptr;
			const char *oldlocale;

			/* set the locale to 'C' for consistent decimal parsing (0.7 and never
			 * 0,7) from config files, then restore old setting when we are done */
			oldlocale = setlocale(LC_NUMERIC, NULL);
			setlocale(LC_NUMERIC, "C");
			ratio = strtod(value, &endptr);
			setlocale(LC_NUMERIC, oldlocale);

			if(*endptr != '\0' || ratio < 0.0 || ratio > 2.0) {
				pm_printf(ALPM_LOG_ERROR,
						_("config file %s, line %d: invalid value for '%s' : '%s'\n"),
						file, linenum, "UseDelta", value);
				return 1;
			}
			config->deltaratio = ratio;
			pm_printf(ALPM_LOG_DEBUG, "config: usedelta = %f\n", ratio);
		} else if(strcmp(key, "DBPath") == 0) {
			/* don't overwrite a path specified on the command line */
			if(!config->dbpath) {
				config->dbpath = strdup(value);
				pm_printf(ALPM_LOG_DEBUG, "config: dbpath: %s\n", value);
			}
		} else if(strcmp(key, "RootDir") == 0) {
			/* don't overwrite a path specified on the command line */
			if(!config->rootdir) {
				config->rootdir = strdup(value);
				pm_printf(ALPM_LOG_DEBUG, "config: rootdir: %s\n", value);
			}
		} else if(strcmp(key, "GPGDir") == 0) {
			if(!config->gpgdir) {
				config->gpgdir = strdup(value);
				pm_printf(ALPM_LOG_DEBUG, "config: gpgdir: %s\n", value);
			}
		} else if(strcmp(key, "LogFile") == 0) {
			if(!config->logfile) {
				config->logfile = strdup(value);
				pm_printf(ALPM_LOG_DEBUG, "config: logfile: %s\n", value);
			}
		} else if(strcmp(key, "XferCommand") == 0) {
			config->xfercommand = strdup(value);
			pm_printf(ALPM_LOG_DEBUG, "config: xfercommand: %s\n", value);
		} else if(strcmp(key, "CleanMethod") == 0) {
			alpm_list_t *methods = NULL;
			setrepeatingoption(value, "CleanMethod", &methods);
			if(process_cleanmethods(methods, file, linenum)) {
				FREELIST(methods);
				return 1;
			}
			FREELIST(methods);
		} else if(strcmp(key, "SigLevel") == 0) {
			alpm_list_t *values = NULL;
			setrepeatingoption(value, "SigLevel", &values);
			if(process_siglevel(values, &config->siglevel, file, linenum)) {
				FREELIST(values);
				return 1;
			}
			FREELIST(values);
		} else if(strcmp(key, "LocalFileSigLevel") == 0) {
			alpm_list_t *values = NULL;
			setrepeatingoption(value, "LocalFileSigLevel", &values);
			if(process_siglevel(values, &config->localfilesiglevel, file, linenum)) {
				FREELIST(values);
				return 1;
			}
			FREELIST(values);
		} else if(strcmp(key, "RemoteFileSigLevel") == 0) {
			alpm_list_t *values = NULL;
			setrepeatingoption(value, "RemoteFileSigLevel", &values);
			if(process_siglevel(values, &config->remotefilesiglevel, file, linenum)) {
				FREELIST(values);
				return 1;
			}
			FREELIST(values);
		} else {
			pm_printf(ALPM_LOG_WARNING,
					_("config file %s, line %d: directive '%s' in section '%s' not recognized.\n"),
					file, linenum, key, "options");
		}

	}
	return 0;
}

static int _add_mirror(alpm_db_t *db, char *value)
{
	const char *dbname = alpm_db_get_name(db);
	/* let's attempt a replacement for the current repo */
	char *temp = strreplace(value, "$repo", dbname);
	/* let's attempt a replacement for the arch */
	const char *arch = config->arch;
	char *server;
	if(arch) {
		server = strreplace(temp, "$arch", arch);
		free(temp);
	} else {
		if(strstr(temp, "$arch")) {
			free(temp);
			pm_printf(ALPM_LOG_ERROR,
					_("mirror '%s' contains the '%s' variable, but no '%s' is defined.\n"),
					value, "$arch", "Architecture");
			return 1;
		}
		server = temp;
	}

	if(alpm_db_add_server(db, server) != 0) {
		/* pm_errno is set by alpm_db_setserver */
		pm_printf(ALPM_LOG_ERROR, _("could not add server URL to database '%s': %s (%s)\n"),
				dbname, server, alpm_strerror(alpm_errno(config->handle)));
		free(server);
		return 1;
	}

	free(server);
	return 0;
}

/** Sets up libalpm global stuff in one go. Called after the command line
 * and initial config file parsing. Once this is complete, we can see if any
 * paths were defined. If a rootdir was defined and nothing else, we want all
 * of our paths to live under the rootdir that was specified. Safe to call
 * multiple times (will only do anything the first time).
 */
static int setup_libalpm(void)
{
	int ret = 0;
	alpm_errno_t err;
	alpm_handle_t *handle;

	pm_printf(ALPM_LOG_DEBUG, "setup_libalpm called\n");

	/* Configure root path first. If it is set and dbpath/logfile were not
	 * set, then set those as well to reside under the root. */
	if(config->rootdir) {
		char path[PATH_MAX];
		if(!config->dbpath) {
			snprintf(path, PATH_MAX, "%s/%s", config->rootdir, DBPATH + 1);
			config->dbpath = strdup(path);
		}
		if(!config->logfile) {
			snprintf(path, PATH_MAX, "%s/%s", config->rootdir, LOGFILE + 1);
			config->logfile = strdup(path);
		}
	} else {
		config->rootdir = strdup(ROOTDIR);
		if(!config->dbpath) {
			config->dbpath = strdup(DBPATH);
		}
	}

	/* initialize library */
	handle = alpm_initialize(config->rootdir, config->dbpath, &err);
	if(!handle) {
		pm_printf(ALPM_LOG_ERROR, _("failed to initialize alpm library\n(%s: %s)\n"),
		        alpm_strerror(err), config->dbpath);
		if(err == ALPM_ERR_DB_VERSION) {
			pm_printf(ALPM_LOG_ERROR, _("  try running pacman-db-upgrade\n"));
		}
		return -1;
	}
	config->handle = handle;

	alpm_option_set_logcb(handle, cb_log);
	alpm_option_set_dlcb(handle, cb_dl_progress);
	alpm_option_set_eventcb(handle, cb_event);
	alpm_option_set_questioncb(handle, cb_question);
	alpm_option_set_progresscb(handle, cb_progress);

	config->logfile = config->logfile ? config->logfile : strdup(LOGFILE);
	ret = alpm_option_set_logfile(handle, config->logfile);
	if(ret != 0) {
		pm_printf(ALPM_LOG_ERROR, _("problem setting logfile '%s' (%s)\n"),
				config->logfile, alpm_strerror(alpm_errno(handle)));
		return ret;
	}

	/* Set GnuPG's home directory.  This is not relative to rootdir, even if
	 * rootdir is defined. Reasoning: gpgdir contains configuration data. */
	config->gpgdir = config->gpgdir ? config->gpgdir : strdup(GPGDIR);
	ret = alpm_option_set_gpgdir(handle, config->gpgdir);
	if(ret != 0) {
		pm_printf(ALPM_LOG_ERROR, _("problem setting gpgdir '%s' (%s)\n"),
				config->gpgdir, alpm_strerror(alpm_errno(handle)));
		return ret;
	}

	/* add a default cachedir if one wasn't specified */
	if(config->cachedirs == NULL) {
		alpm_option_add_cachedir(handle, CACHEDIR);
	} else {
		alpm_option_set_cachedirs(handle, config->cachedirs);
	}

	alpm_option_set_default_siglevel(handle, config->siglevel);

	merge_siglevel(&config->siglevel, &config->localfilesiglevel);
	merge_siglevel(&config->siglevel, &config->remotefilesiglevel);
	alpm_option_set_local_file_siglevel(handle, config->localfilesiglevel);
	alpm_option_set_remote_file_siglevel(handle, config->remotefilesiglevel);

	if(config->xfercommand) {
		alpm_option_set_fetchcb(handle, download_with_xfercommand);
	} else if(!(alpm_capabilities() & ALPM_CAPABILITY_DOWNLOADER)) {
		pm_printf(ALPM_LOG_WARNING, _("no '%s' configured"), "XferCommand");
	}

	if(config->totaldownload) {
		alpm_option_set_totaldlcb(handle, cb_dl_total);
	}

	alpm_option_set_arch(handle, config->arch);
	alpm_option_set_checkspace(handle, config->checkspace);
	alpm_option_set_usesyslog(handle, config->usesyslog);
	alpm_option_set_deltaratio(handle, config->deltaratio);

	alpm_option_set_ignorepkgs(handle, config->ignorepkg);
	alpm_option_set_ignoregroups(handle, config->ignoregrp);
	alpm_option_set_noupgrades(handle, config->noupgrade);
	alpm_option_set_noextracts(handle, config->noextract);

	return 0;
}

/**
 * Allows parsing in advance of an entire config section before we start
 * calling library methods.
 */
struct section_t {
	/* useful for all sections */
	char *name;
	int is_options;
	/* db section option gathering */
	alpm_siglevel_t siglevel;
	alpm_list_t *servers;
};

/**
 * Wrap up a section once we have reached the end of it. This should be called
 * when a subsequent section is encountered, or when we have reached the end of
 * the root config file. Once called, all existing saved config pieces on the
 * section struct are freed.
 * @param section the current parsed and saved section data
 * @param parse_options whether we are parsing options or repo data
 * @return 0 on success, 1 on failure
 */
static int finish_section(struct section_t *section, int parse_options)
{
	int ret = 0;
	alpm_list_t *i;
	alpm_db_t *db;

	pm_printf(ALPM_LOG_DEBUG, "config: finish section '%s'\n", section->name);

	/* parsing options (or nothing)- nothing to do except free the pieces */
	if(!section->name || parse_options || section->is_options) {
		goto cleanup;
	}

	/* if we are not looking at options sections only, register a db */
	db = alpm_register_syncdb(config->handle, section->name, section->siglevel);
	if(db == NULL) {
		pm_printf(ALPM_LOG_ERROR, _("could not register '%s' database (%s)\n"),
				section->name, alpm_strerror(alpm_errno(config->handle)));
		ret = 1;
		goto cleanup;
	}

	for(i = section->servers; i; i = alpm_list_next(i)) {
		char *value = i->data;
		if(_add_mirror(db, value) != 0) {
			pm_printf(ALPM_LOG_ERROR,
					_("could not add mirror '%s' to database '%s' (%s)\n"),
					value, section->name, alpm_strerror(alpm_errno(config->handle)));
			ret = 1;
			goto cleanup;
		}
		free(value);
	}

cleanup:
	alpm_list_free(section->servers);
	section->servers = NULL;
	section->siglevel = ALPM_SIG_USE_DEFAULT;
	free(section->name);
	section->name = NULL;
	return ret;
}

/** The "real" parseconfig. Each "Include" directive will recall this method so
 * recursion and stack depth are limited to 10 levels. The publicly visible
 * parseconfig calls this with a NULL section argument so we can recall from
 * within ourself on an include.
 * @param file path to the config file
 * @param section the current active section
 * @param parse_options whether to parse and call methods for the options
 * section; if 0, parse and call methods for the repos sections
 * @param depth the current recursion depth
 * @return 0 on success, 1 on failure
 */
static int _parseconfig(const char *file, struct section_t *section,
		int parse_options, int depth)
{
	FILE *fp = NULL;
	char line[PATH_MAX];
	int linenum = 0;
	int ret = 0;
	const int max_depth = 10;

	if(depth >= max_depth) {
		pm_printf(ALPM_LOG_ERROR,
				_("config parsing exceeded max recursion depth of %d.\n"), max_depth);
		ret = 1;
		goto cleanup;
	}

	pm_printf(ALPM_LOG_DEBUG, "config: attempting to read file %s\n", file);
	fp = fopen(file, "r");
	if(fp == NULL) {
		pm_printf(ALPM_LOG_ERROR, _("config file %s could not be read: %s\n"),
				file, strerror(errno));
		ret = 1;
		goto cleanup;
	}

	while(fgets(line, PATH_MAX, fp)) {
		char *key, *value, *ptr;
		size_t line_len;

		linenum++;

		/* ignore whole line and end of line comments */
		if((ptr = strchr(line, '#'))) {
			*ptr = '\0';
		}

		line_len = strtrim(line);

		if(line_len == 0) {
			continue;
		}

		if(line[0] == '[' && line[line_len - 1] == ']') {
			char *name;
			/* only possibility here is a line == '[]' */
			if(line_len <= 2) {
				pm_printf(ALPM_LOG_ERROR, _("config file %s, line %d: bad section name.\n"),
						file, linenum);
				ret = 1;
				goto cleanup;
			}
			/* new config section, skip the '[' */
			name = strdup(line + 1);
			name[line_len - 2] = '\0';
			/* we're at a new section; perform any post-actions for the prior */
			if(finish_section(section, parse_options)) {
				ret = 1;
				goto cleanup;
			}
			pm_printf(ALPM_LOG_DEBUG, "config: new section '%s'\n", name);
			section->name = name;
			section->is_options = (strcmp(name, "options") == 0);
			continue;
		}

		/* directive */
		/* strsep modifies the 'line' string: 'key \0 value' */
		key = line;
		value = line;
		strsep(&value, "=");
		strtrim(key);
		strtrim(value);

		if(key == NULL) {
			pm_printf(ALPM_LOG_ERROR, _("config file %s, line %d: syntax error in config file- missing key.\n"),
					file, linenum);
			ret = 1;
			goto cleanup;
		}
		/* For each directive, compare to the camelcase string. */
		if(section->name == NULL) {
			pm_printf(ALPM_LOG_ERROR, _("config file %s, line %d: All directives must belong to a section.\n"),
					file, linenum);
			ret = 1;
			goto cleanup;
		}
		/* Include is allowed in both options and repo sections */
		if(strcmp(key, "Include") == 0) {
			glob_t globbuf;
			int globret;
			size_t gindex;

			if(value == NULL) {
				pm_printf(ALPM_LOG_ERROR, _("config file %s, line %d: directive '%s' needs a value\n"),
						file, linenum, key);
				ret = 1;
				goto cleanup;
			}
			/* Ignore include failures... assume non-critical */
			globret = glob(value, GLOB_NOCHECK, NULL, &globbuf);
			switch(globret) {
				case GLOB_NOSPACE:
					pm_printf(ALPM_LOG_DEBUG,
							"config file %s, line %d: include globbing out of space\n",
							file, linenum);
				break;
				case GLOB_ABORTED:
					pm_printf(ALPM_LOG_DEBUG,
							"config file %s, line %d: include globbing read error for %s\n",
							file, linenum, value);
				break;
				case GLOB_NOMATCH:
					pm_printf(ALPM_LOG_DEBUG,
							"config file %s, line %d: no include found for %s\n",
							file, linenum, value);
				break;
				default:
					for(gindex = 0; gindex < globbuf.gl_pathc; gindex++) {
						pm_printf(ALPM_LOG_DEBUG, "config file %s, line %d: including %s\n",
								file, linenum, globbuf.gl_pathv[gindex]);
						_parseconfig(globbuf.gl_pathv[gindex], section, parse_options, depth + 1);
					}
				break;
			}
			globfree(&globbuf);
			continue;
		}
		if(parse_options && section->is_options) {
			/* we are either in options ... */
			if((ret = _parse_options(key, value, file, linenum)) != 0) {
				goto cleanup;
			}
		} else if(!parse_options && !section->is_options) {
			/* ... or in a repo section */
			if(strcmp(key, "Server") == 0) {
				if(value == NULL) {
					pm_printf(ALPM_LOG_ERROR, _("config file %s, line %d: directive '%s' needs a value\n"),
							file, linenum, key);
					ret = 1;
					goto cleanup;
				}
				section->servers = alpm_list_add(section->servers, strdup(value));
			} else if(strcmp(key, "SigLevel") == 0) {
				alpm_list_t *values = NULL;
				setrepeatingoption(value, "SigLevel", &values);
				if(values) {
					if(section->siglevel == ALPM_SIG_USE_DEFAULT) {
						section->siglevel = config->siglevel;
					}
					if(process_siglevel(values, &section->siglevel, file, linenum)) {
						FREELIST(values);
						ret = 1;
						goto cleanup;
					}
					FREELIST(values);
				}
			} else {
				pm_printf(ALPM_LOG_WARNING,
						_("config file %s, line %d: directive '%s' in section '%s' not recognized.\n"),
						file, linenum, key, section->name);
			}
		}
	}

	if(depth == 0) {
		ret = finish_section(section, parse_options);
	}

cleanup:
	if(fp) {
		fclose(fp);
	}
	pm_printf(ALPM_LOG_DEBUG, "config: finished parsing %s\n", file);
	return ret;
}

/** Parse a configuration file.
 * @param file path to the config file
 * @return 0 on success, non-zero on error
 */
int parseconfig(const char *file)
{
	int ret;
	struct section_t section;
	memset(&section, 0, sizeof(struct section_t));
	section.siglevel = ALPM_SIG_USE_DEFAULT;
	/* the config parse is a two-pass affair. We first parse the entire thing for
	 * the [options] section so we can get all default and path options set.
	 * Next, we go back and parse everything but [options]. */

	/* call the real parseconfig function with a null section & db argument */
	pm_printf(ALPM_LOG_DEBUG, "parseconfig: options pass\n");
	if((ret = _parseconfig(file, &section, 1, 0))) {
		return ret;
	}
	if((ret = setup_libalpm())) {
		return ret;
	}
	/* second pass, repo section parsing */
	pm_printf(ALPM_LOG_DEBUG, "parseconfig: repo pass\n");
	return _parseconfig(file, &section, 0, 0);
}

/* vim: set ts=2 sw=2 noet: */
