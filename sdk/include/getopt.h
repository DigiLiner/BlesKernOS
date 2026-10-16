#ifndef BLESKERNOS_POSIX_GETOPT_H
#define BLESKERNOS_POSIX_GETOPT_H
extern char *optarg;
extern int optind, opterr, optopt;
struct option { const char *name; int has_arg; int *flag; int val; };
#define no_argument 0
#define required_argument 1
#define optional_argument 2
int getopt(int argc, char *const argv[], const char *options);
int getopt_long(int argc, char *const argv[], const char *options,
                const struct option *long_options, int *long_index);
int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *long_options, int *long_index);
#endif
