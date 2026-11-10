#ifndef INTEGRITY_H
#define INTEGRITY_H

#include "audit.h"
#include "buf.h"

#include <stdbool.h>

/*
 * Integrity of the installed AUR (foreign) packages: inkaur --integrity.
 *
 *  - files: pacman -Qkk compares every installed file with the checksums,
 *    sizes, permissions and owners recorded when the package was installed
 *  - origin: every package inkaur installs is recorded (version, AUR commit,
 *    SHA-256 of the package file) in ~/.local/share/inkaur/installed; an
 *    installed version that differs was installed by something else
 *  - AUR: packages deleted from the AUR, orphaned, flagged out of date, or
 *    with an update available
 * The report uses the same plain-language explanations and verdicts as the
 * security audit.
 */

/* Directory of the install records ($XDG_DATA_HOME or ~/.local/share). */
void integrity_set_home(const char *home);

typedef struct {
    char name[256];
    char base[256];
    char version[256];
    char commit[80];
    char sha256[80];
    long installed;   /* unix time */
} InstallRecord;

/* Writes the record of a package inkaur just installed. 0 on success. */
int integrity_record(const InstallRecord *rec);
/* Reads the record of name; false if there is none (or it is invalid). */
bool integrity_load(const char *name, InstallRecord *rec);

/* Exposed for tests: parse a record file. */
bool integrity_parse_record(const char *text, InstallRecord *rec);

/*
 * Exposed for tests: adds the findings for package `name` from the output
 * of "pacman -Qkk" (LC_ALL=C). *checked is set when pacman reported a
 * summary line for it.
 */
void integrity_parse_qkk(const char *text, const char *name, Report *r, bool *checked);

/* inkaur --integrity [pkg...]: exit status 0..3 = highest severity, 4 error. */
int cmd_integrity(const StrList *targets);

#endif /* INTEGRITY_H */
