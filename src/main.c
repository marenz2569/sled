// Main loader.

#include "types.h"
#include "matrix.h"
#include "modloader.h"
#include "timers.h"
#include "random.h"
#include "util.h"
#include "asl.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include "oscore.h"

static int modcount;
int *outmodno;
struct module *outmod;

static oscore_mutex rmod_lock;
// Usually -1.
static int main_rmod_override = -1;
static int main_rmod_override_argc;
static char* *main_rmod_override_argv;

const char default_moduledir[] = DEFAULT_MODULEDIR;
static char* modpath = NULL;

static int deinit(void) {
	printf("Cleaning up...\n");
	int ret;
	if ((ret = modules_deinit()) != 0)
		return ret;
	if ((ret = matrix_deinit()) != 0)
		return ret;
	if ((ret = timers_deinit()) != 0)
		return ret;
	oscore_mutex_free(rmod_lock);
	if (main_rmod_override != -1)
		asl_free_argv(main_rmod_override_argc, main_rmod_override_argv);

	free(modpath);
	free(outmodno);

	printf("Goodbye. :(\n");
	return 0;
}

static int pick_other(int mymodno, ulong in) {
	oscore_mutex_lock(rmod_lock);
	if (main_rmod_override != -1) {
		int res = timer_add(in, main_rmod_override, main_rmod_override_argc, main_rmod_override_argv);
		main_rmod_override = -1;
		oscore_mutex_unlock(rmod_lock);
		return res;
	}
	oscore_mutex_unlock(rmod_lock);
	int mod;
	int lastvalidmod = 0;
	int usablemodcount = 0;
	for (mod = 0; mod < modcount; mod++) {
		if (strcmp(modules_get(mod)->type, "gfx") != 0)
			continue;
		usablemodcount++;
		lastvalidmod = mod;
	}
	if (usablemodcount > 1) {
		mod = -1;
		while (mod == -1) {
			int random = randn(modcount);
			mod = random;

			// Checks after.
			if (mod == mymodno) mod = -1;
			if (strcmp(modules_get(mod)->type, "gfx") != 0) mod = -1;
		}
	} else if (usablemodcount == 1) {
		mod = lastvalidmod;
	} else {
		in += 5000000;
		mod = -2;
	}
	return timer_add(in, mod, 0, NULL);
}

void main_force_random(int mnum, int argc, char ** argv) {
	while (!timers_quitting) {
		oscore_mutex_lock(rmod_lock);
		if (main_rmod_override == -1) {
			main_rmod_override = mnum;
			main_rmod_override_argc = argc;
			main_rmod_override_argv = argv;
			oscore_mutex_unlock(rmod_lock);
			return;
		}
		oscore_mutex_unlock(rmod_lock);
		usleep(5000);
	}
	// Quits out without doing anything to prevent deadlock.
	asl_free_argv(argc, argv);
}

int usage(char* name) {
	printf("Usage: %s [-of]\n", name);
	printf("\t-m --modpath: Set directory that contains the modules to load.\n");
	printf("\t-o --output:  Set output module. Defaults to dummy.\n");
	printf("\t-f --filter:  Add a filter, can be used multiple times.\n");
	return 1;
}

static struct option longopts[] = {
	{ "modpath", required_argument, NULL, 'm' },
	{ "output",  required_argument, NULL, 'o' },
	{ "filter",  optional_argument, NULL, 'f' },
	{ NULL,      0,                 NULL, 0},
};

static int interrupt_count = 0;
static void interrupt_handler(int sig) {
	//
	if (interrupt_count == 0) {
		printf("sled: Quitting due to interrupt...\n");
		timers_doquit();
	} else if (interrupt_count == 1) {
		eprintf("sled: Warning: One more interrupt until ungraceful exit!\n");
	} else {
		eprintf("sled: Instantly panic-exiting. Bye.\n");
		exit(1);
	}

	interrupt_count++;
}

int sled_main(int argc, char** argv) {
	int ch;
	char outmod_c[256] = DEFAULT_OUTMOD;

	char* filternames[MAX_MODULES];
	char* filterargs[MAX_MODULES];
	int filterno = 0;
	char* outarg = NULL;
	while ((ch = getopt_long(argc, argv, "m:o:f:", longopts, NULL)) != -1) {
		switch(ch) {
		case 'm': {
			int len = strlen(optarg);
			char* str = calloc(len + 1, sizeof(char));
			util_strlcpy(str, optarg, len + 1);
			modpath = str;
			break;
		}
		case 'o': {
			int len = strlen(optarg);
			char* tmp = malloc((len + 1) * sizeof(char));
			util_strlcpy(tmp, optarg, len + 1);
			char* arg = tmp;

			char* modname = strsep(&arg, ":");
			if (arg != NULL)
				outarg = strdup(arg);
			else
				modname = optarg;
			util_strlcpy(outmod_c, modname, 256);
			free(tmp);
			break;
		}
		case 'f': {
			char* arg = strdup(optarg);

			char* modname = strsep(&arg, ":");
			char* fltarg = NULL;
			if (arg != NULL) {
				int len = strlen(arg); // optarg is now the string after the colon
				fltarg = malloc((len + 1) * sizeof(char)); // i know, its a habit. a good one.
				util_strlcpy(fltarg, arg, len + 1);
			} else
				modname = optarg;
			int len = strlen(modname);
			char* str = malloc((len + 1) * sizeof(char));
			util_strlcpy(str, modname, len + 1);
			filternames[filterno] = str;
			filterargs[filterno] = fltarg;
			filterno++;
			break;
		}
		case '?':
		default:
			return usage(argv[0]);
		}
	}
	argc -= optind;
	argv += optind;

	int ret;
	rmod_lock = oscore_mutex_new();

	// Initialize pseudo RNG.
	random_seed();

	// Load modules
	if (modpath == NULL)
		modpath = strdup(default_moduledir);
	int* filters = NULL;
	if (filterno > 0) {
		filters = malloc(filterno * sizeof(int));
		int i;
		for (i = 0; i < filterno; ++i)
			filters[i] = -1;
	}
	int *outmodno = NULL;
	outmodno = malloc(sizeof(int));
	if (outmodno == NULL) {
		deinit();
		return 1;
	}

	if ((ret = modules_loaddir(modpath, outmod_c, outmodno, filternames, &filterno, filters)) != 0) {
		deinit();
		return ret;
	}

	outmod = modules_get(*outmodno);

	// Initialize Timers.
	ret = timers_init(*outmodno);
	if (ret) {
		printf("Timers failed to initialize.\n");
		oscore_mutex_free(rmod_lock);
		return ret;
	}

	// Initialize Matrix.
	ret = matrix_init(*outmodno, filters, filterno, outarg, filterargs);
	if (ret) {
		// Fail.
		printf("Matrix: Output plugin failed to initialize.\n");
		timers_deinit();
		oscore_mutex_free(rmod_lock);
		return ret;
	}

	// Initialize modules (this can offset outmodno)
	ret = modules_init(&outmodno);
	if (ret) {
		printf("Modules: Init failed.\n");
		return ret;
	}

	modcount = modules_count();

	signal(SIGINT, interrupt_handler);

	// Startup.
	pick_other(-1, udate());

	int lastmod = -1;
	while (!timers_quitting) {
		timer tnext = timer_get();
		if (tnext.moduleno == -1) {
			// Queue random.
			pick_other(lastmod, udate() + RANDOM_TIME * T_SECOND);
		} else {
			if (tnext.time > wait_until(tnext.time)) {
				// Early break. Set this timer up for elimination by any 0-time timers that have come along
				if (tnext.time == 0)
					tnext.time = 1;
				timer_add(tnext.time, tnext.moduleno, tnext.argc, tnext.argv);
				continue;
			}
			if (tnext.moduleno >= 0) {
				module* mod = modules_get(tnext.moduleno);
				if (tnext.moduleno != lastmod) {
					printf("\n>> Now drawing %s", mod->name);
					fflush(stdout);
					if (mod->reset)
						mod->reset();
				} else {
					printf(".");
					fflush(stdout);
				};
				ret = mod->draw(tnext.argc, tnext.argv);
				asl_free_argv(tnext.argc, tnext.argv);
				lastmod = tnext.moduleno;
				if (ret != 0) {
					if (ret == 1) {
						if (lastmod != tnext.moduleno) // not an animation.
							printf("\nModule chose to pass its turn to draw.");
						pick_other(lastmod, udate() + T_MILLISECOND);
					} else {
						eprintf("Module %s failed to draw: Returned %i", mod->name, ret);
						timers_quitting = 1;
						deinit();
						return 7;
					}
				}
			} else {
				// Virtual null module
				printf(">> using virtual null module\n");
				asl_free_argv(tnext.argc, tnext.argv);
			}
		}
	}

	return deinit();
}
