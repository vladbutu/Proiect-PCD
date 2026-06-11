/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Compatibilitate getopt pentru analiza statica.
 */

#ifndef GETOPT_COMPAT_H
#define GETOPT_COMPAT_H

extern char *optarg; // argumentul curent pentru optiunea parsata de getopt
extern int optind; // indexul urmatorului argument neparsat dupa getopt
extern int getopt(int argc, char *const argv[], const char *optstring); // parser CLI POSIX

#endif /* GETOPT_COMPAT_H */
