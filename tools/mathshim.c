/* Local fabs/sqrt/floor for build-win.sh: with MSYS2's msvcrt import library lld would bind these to
   data imports that need runtime pseudo-relocations, which this CRT-less DLL does not support. */
double fabs(double x) { return __builtin_fabs(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x) { return __builtin_floor(x); }
