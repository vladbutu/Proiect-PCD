/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Compatibilitate getopt pentru analiza statica (clang-tidy)
 * Unele configuratii cu macro-uri POSIX stricte pot face ca analiza sa nu
 * asocieze corect simbolurile getopt/optarg cu headerele glibc. Headerul
 * acesta centralizeaza declaratiile folosite de client/server.
 */

#ifndef GETOPT_COMPAT_H
#define GETOPT_COMPAT_H

extern char *optarg; // argumentul curent pentru optiunea parsata de getopt
extern int getopt(int argc, char *const argv[], const char *optstring); // parser CLI POSIX

#endif /* GETOPT_COMPAT_H */
