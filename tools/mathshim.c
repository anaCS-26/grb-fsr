/* Local fabs/sqrt/floor for build-win.sh: with MSYS2's msvcrt import library lld would bind these to
   data imports that need runtime pseudo-relocations, which this CRT-less DLL does not support.
   floor is written out: baseline x86-64 has no floor instruction (roundsd is SSE4.1), so __builtin_floor
   compiles to a call to floor, which here would be this function jumping to itself forever. */
double fabs(double x) { return __builtin_fabs(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x)
{
    if (!(__builtin_fabs(x) < 4503599627370496.0)) /* 2^52: already integral, or inf/NaN */
        return x;
    double t = (double)(long long)x; /* truncates toward zero */
    return t > x ? t - 1.0 : t;
}
