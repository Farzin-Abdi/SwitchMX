/* Forced into every MuPDF file when building for the Switch: newlib's <sys/types.h> defines
   `quad` as a macro, which breaks MuPDF's `quad` struct members. */
#include <sys/types.h>
#undef quad
/* newlib has no timegm; SwitchMX supplies one (source/reader.cpp). */
#include <time.h>
time_t timegm(struct tm *tm);
