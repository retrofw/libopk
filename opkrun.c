#define _BSD_SOURCE 1
#define _XOPEN_SOURCE 1

#include "opk.h"

#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MY_NAME
#define MY_NAME "opkrun"
#endif

#ifndef VTCON_FILE
#define VTCON_FILE "/sys/devices/virtual/vtconsole/vtcon1/bind"
#endif

#ifndef LINKDEV_ALT_MAP_FILE
#define LINKDEV_ALT_MAP_FILE "/sys/devices/platform/linkdev/alt_key_map"
#endif

#ifndef JZ4770FB_ENABLE_DOWNSCALING_FILE
#define JZ4770FB_ENABLE_DOWNSCALING_FILE "/sys/devices/platform/jz-lcd.0/allow_downscaling"
#endif

#define NB_PARAMS_MAX 256

struct params {
	char *mountpoint, *exec[NB_PARAMS_MAX];
	bool needs_terminal, needs_joystick, needs_gsensor, needs_downscaling;
};


static const struct option options[] = {
	  {"help", no_argument, 0, 'h'},
	  {"metadata", required_argument, 0, 'm'},
	  {0, 0, 0, 0},
};

static const char *options_descriptions[] = {
	"Show this help and quit.",
	"Metadata file to use (default: first one found)",
};


static void usage(void)
{
	printf("Usage:\n\t" MY_NAME " [OPTIONS] OPK_FILE [ARGS ...]\n\nOptions:\n");

	for (size_t i = 0; options[i].name; i++)
		printf("\t-%c, --%s\n\t\t\t%s\n",
					options[i].val, options[i].name,
					options_descriptions[i]);
}

static struct OPK * open_opk(const char *filename, const char *metadata)
{
	struct OPK *opk = opk_open(filename);
	if (!opk) {
		fprintf(stderr, "Unable to open OPK\n");
		return NULL;
	}

	for (;;) {
		const char *meta_file;
		int ret = opk_open_metadata(opk, &meta_file);
		if (ret < 0) {
			fprintf(stderr, "Unable to open metadata file within OPK\n");
			goto close_opk;
		}

		if (!ret) {
			fprintf(stderr, "Metadata file not found in OPK\n");
			goto close_opk;
		}

		if (!metadata || !strcmp(metadata, meta_file))
			break;
	}

	return opk;

close_opk:
	opk_close(opk);
	return NULL;
}

static int read_params(struct OPK *opk, struct params *params)
{
	memset(params, 0, sizeof(*params));
	const char *exec_name = NULL, *name = NULL;
	size_t exec_name_len = 0, name_len = 0;
	bool x_od_selector = false;

	for (;;) {
		const char *key, *val;
		size_t skey, sval;
		int ret = opk_read_pair(opk, &key, &skey, &val, &sval);
		if (ret < 0) {
			fprintf(stderr, "Unable to read key/value pair from metadata\n");
			return ret;
		}

		if (!ret)
			break;

		if (!strncmp(key, "Name", skey)) {
			name_len = sval;
			name = val;
			continue;
		}

		if (!strncmp(key, "Exec", skey)) {
			exec_name_len = sval;
			exec_name = val;
			continue;
		}

		if (!strncmp(key, "Terminal", skey)) {
			params->needs_terminal = !strncmp(val, "true", sval);
			continue;
		}

		if (!strncmp(key, "X-OD-NeedsJoystick", skey)) {
			params->needs_joystick =  !strncmp(val, "true", sval);
			continue;
		}

		if (!strncmp(key, "X-OD-NeedsGSensor", skey)) {
			params->needs_gsensor = !strncmp(val, "true", sval);
			continue;
		}

		if (!strncmp(key, "X-OD-NeedsDownscaling", skey)) {
			params->needs_downscaling = !strncmp(val, "true", sval);
			continue;
		}

		if (!strncmp(key, "X-OD-Selector", skey)) {
			x_od_selector = true;
			continue;
		}
	}

	if (!exec_name || !name) {
		fprintf(stderr, "Unable to find the executable name\n");
		return -1;
	}

	char *exec = malloc(exec_name_len + 4);
	memcpy(exec, exec_name, exec_name_len);
	exec[exec_name_len] = '\0';

	if (x_od_selector)
		strcat(exec, " %f");

	/* Split the Exec command into an array of parameters */
	char *ptr;
	unsigned int arg;
	for (ptr = exec, arg = 0; ptr && arg < NB_PARAMS_MAX - 1; arg++) {
		params->exec[arg] = ptr;
		ptr = strchr(ptr, ' ');

		if (ptr) {
			*ptr++ = '\0';
			while (*ptr == ' ') /* Skip eventual additional spaces */
				ptr++;
		}
	}

	params->exec[arg] = NULL;

	params->mountpoint = "/mnt";

	for (ptr = params->mountpoint + 5; *ptr; ptr++) {
		if (*ptr == '\'' || *ptr == '\\')
			*ptr = '_';
	}
	return 0;
}

static void enable_vtcon(void)
{
	FILE *f = fopen(VTCON_FILE, "w");
	if (!f) {
		perror("Unable to open vtcon file");
		return;
	}

	char one = '1';
	fwrite(&one, 1, 1, f);
	fclose(f);
}

static void enable_in_sysfs(const char *fn)
{
	FILE *f = fopen(fn, "w");
	if (!f)
		return;

	char yes = 'Y';
	fwrite(&yes, 1, 1, f);
	fclose(f);
}

static void enable_alt_key_map(void)
{
	enable_in_sysfs(LINKDEV_ALT_MAP_FILE);
}

static void enable_downscaling(void)
{
	enable_in_sysfs(JZ4770FB_ENABLE_DOWNSCALING_FILE);
}

static void enable_gsensor(void)
{
	system("/usr/sbin/gsensor --start");

	/* Fix for SDL apps to recognize the g-sensor */
	putenv("SDL_JOYSTICK_DEVICE=/dev/input/gsensor");
}

static char *get_url(const char *file)
{
	char *url = realpath(file, NULL);
	if (!url)
		return strdup(file);

	char *tmp = malloc(strlen(url) + sizeof "file://");
	sprintf(tmp, "file://%s", url);
	free(url);
	return tmp;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Error: Too few arguments given.\n\n");
		usage();
		return EXIT_SUCCESS;
	}

	int c, option_index = 0, arg_index = 1;
	const char *metadata = NULL;

	while ((c = getopt_long(argc, argv, "+hm:",
						options, &option_index)) != -1) {
		switch (c) {
			case 'h':
				usage();
				return EXIT_SUCCESS;

			case 'm':
				metadata = optarg;
				arg_index += 2;
				break;

			case '?':
				return EXIT_FAILURE;
		}
	}

	if (arg_index >= argc) {
		fprintf(stderr, "Incorrect number of arguments.\n\n");
		usage();
		return EXIT_FAILURE;
	}

	const char *opk_name = argv[arg_index];
	struct OPK *opk = open_opk(opk_name, metadata);
	if (!opk)
		return EXIT_FAILURE;

	struct params params;
	int ret = read_params(opk, &params);
	opk_close(opk);
	if (ret < 0)
		return EXIT_FAILURE;

	char **opk_argv = argv + arg_index + 1;
	int opk_argc = argc - arg_index - 1;
	if (opk_argc > NB_PARAMS_MAX - 2)
		opk_argc = NB_PARAMS_MAX - 2;

	char *args[NB_PARAMS_MAX];
	memset(args, 0, sizeof(args));

	/* This loop is used to replace the [%f %F %u %U] tokens
	 * with the filenames passed as parameter of opkrun */
	char **ptr;
	unsigned int arg;
	for (arg = 0, ptr = params.exec; *ptr && arg < NB_PARAMS_MAX; ptr++, arg++) {
		if (!strcmp("%f", *ptr)) {
			if (!opk_argc) {
				fprintf(stderr, "WARNING: OPK requires a file as parameter, but none was given\n");
			} else {
				args[arg] = realpath(*opk_argv++, NULL);
				if (--opk_argc)
					fprintf(stderr, "WARNING: OPK requires only one file as parameter\n");
			}
		} else if (!strcmp("%F", *ptr)) {
			while (opk_argc && arg < NB_PARAMS_MAX) {
				args[arg++] = realpath(*opk_argv++, NULL);
				opk_argc--;
			}
			arg--; /* Compensate the arg++ in the 'for' */
		} else if (!strcmp("%u", *ptr)) {
			if (!opk_argc) {
				fprintf(stderr, "WARNING: OPK requires an URL as parameter, but none was given\n");
			} else {
				args[arg] = get_url(*opk_argv++);
				if (--opk_argc)
					fprintf(stderr, "WARNING: OPK requires only one URL as parameter\n");
			}
		} else if (!strcmp("%U", *ptr)) {
			while (opk_argc && arg < NB_PARAMS_MAX) {
				args[arg++] = get_url(*opk_argv++);
				opk_argc--;
			}
			arg--; /* Compensate the arg++ in the 'for' */
		} else {
			args[arg] = strdup(*ptr);
		}
	}

	free(params.exec[0]);

	umount(params.mountpoint);

	char buf[256];
	sprintf(buf, "mount -o loop,nodev,nosuid,ro \'%s\' \'%s\' >/dev/null 2>&1",
				opk_name, params.mountpoint);
	ret = system(buf);
	if (ret < 0) {
		perror("Unable to mount OPK");
		free(params.mountpoint);
		return EXIT_FAILURE;
	}

	chdir(params.mountpoint);

	if (params.needs_terminal)
		enable_vtcon();

	if (params.needs_joystick)
		enable_alt_key_map();

	if (params.needs_gsensor)
		enable_gsensor();

	if (params.needs_downscaling)
		enable_downscaling();

	pid_t son = fork();
	if (!son) {
		/* Drop privileges */
		seteuid(getuid());

		if (!access(args[0], X_OK)) /* Not in the root of the OPK */
			execv(args[0], args);   /* Maybe in the $PATH? */
		execvp(args[0], args);
	}

	int status;
	waitpid(son, &status, 0);

	chdir("/");
	umount(params.mountpoint);
	free(params.mountpoint);

	for (char **ptr = args; *ptr; ptr++)
		free(*ptr);

	return status;
}
