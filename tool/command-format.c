#include "command.h"
#include "interface.h"
#include "util.h"

#include <errno.h>
#include <dirent.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

static long read_file(FILE *file, char **contentsp) {
        _cleanup_(freep) char *contents = NULL;
        unsigned long allocated = 0;
        unsigned long size = 0;

        while (!feof(file)) {
                long n;

                if (size == allocated) {
                        allocated = MAX(allocated * 2, 4096);
                        contents = realloc(contents, allocated);
                        if (!contents)
                                return -ENOMEM;
                }

                n = fread(contents + size, 1, allocated - size, file);
                if (n == 0 && ferror(file))
                        return -errno;

                size += n;
        }

        contents[size] = '\0';

        *contentsp = contents;
        contents = NULL;

        return 0;
}

static long format_run(Cli *cli, int argc, char **argv) {
        static const struct option options[] = {
                { "help",   no_argument,       NULL, 'h' },
                {}
        };
        int c;
        const char *in_filename = NULL;
        _cleanup_(varlink_interface_freep) VarlinkInterface *interface = NULL;
        _cleanup_(scanner_freep) Scanner *scanner = NULL;
        _cleanup_(fclosep) FILE *in_file = NULL;
        _cleanup_(freep) char *in = NULL;
        _cleanup_(freep) char *out = NULL;
        long r;

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {
                switch (c) {
                        case 'h':
                                printf("Usage: %s format [OPTIONS]... ARGUMENTS\n", program_invocation_short_name);
                                printf("\n");
                                printf("Format a varlink service file.\n");
                                printf("\n");
                                printf("  -h, --help             display this help text and exit\n");
                                return 0;

                        default:
                                fprintf(stderr, "Try '%s --help' for more information\n",
                                        program_invocation_short_name);
                                return -CLI_ERROR_INVALID_ARGUMENT;
                }
        }

        in_filename = argv[optind];
        if (!in_filename) {
                fprintf(stderr, "Usage: %s [OPTIONS]... FILE\n", program_invocation_short_name);
                return -CLI_ERROR_MISSING_ARGUMENT;
        }

        if (strcmp(in_filename, "-") != 0) {
                in_file = fopen(in_filename, "r");
                if (!in_file) {
                        fprintf(stderr, "Error opening %s for reading: %s\n", in_filename, strerror(errno));
                        return -CLI_ERROR_PANIC;
                }
        } else
                in_file = stdin;

        r = read_file(in_file, &in);
        if (r < 0) {
                fprintf(stderr, "Error reading %s: %s\n", in_filename, strerror(-r));
                return -CLI_ERROR_PANIC;
        }

        r = varlink_interface_new(&interface, in, &scanner);
        if (r < 0) {
                fprintf(stderr, "%s:%lu:%lu: %s\n",
                        in_filename,
                        scanner->line_nr, scanner->error.pos_nr,
                        scanner_error_string(scanner->error.no));
                return -CLI_ERROR_PANIC;
        }

        r = varlink_interface_write_description(interface, &out,
                                                0,
                                                NULL, NULL,
                                                NULL, NULL,
                                                NULL, NULL,
                                                NULL, NULL);
        if (r < 0) {
                fprintf(stderr, "Error writing interface: %s", strerror(-r));
                return -CLI_ERROR_PANIC;
        }

        printf("%s", out);

        return 0;
}

static long format_complete(Cli *cli, int argc, char **argv, const char *current) {
        _cleanup_(freep) char *prefix = NULL;
        DIR *dir;
        char *p;

        if (argc != 1)
                return 0;

        p = strrchr(current, '/');
        if (p) {
                prefix = strndup(current, p - current + 1);
                if (!prefix)
                        return -CLI_ERROR_PANIC;

                dir = opendir(prefix);
        } else
                dir = opendir(".");
        if (!dir)
                return 0;

        for (struct dirent *d = readdir(dir); d; d = readdir(dir)) {
                if (d->d_name[0] == '.')
                        continue;

                switch (d->d_type) {
                        case DT_DIR:
                                cli_print_completion(current, "%s%s/", prefix ?: "", d->d_name);
                                break;

                        case DT_REG:
                        case DT_LNK: {
                                long l = strlen(d->d_name);

                                if (l < 9)
                                        break;

                                if (strcmp(d->d_name + l - 8, ".varlink") != 0)
                                        break;

                                cli_print_completion(current, "%s%s", prefix ?: "", d->d_name);
                                break;
                        }
                }
        }

        closedir(dir);

        return 0;
}

const CliCommand command_format = {
        .name = "format",
        .info = "Format a varlink service file",
        .run = format_run,
        .complete = format_complete
};
