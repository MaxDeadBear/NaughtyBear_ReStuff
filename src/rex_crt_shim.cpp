// CRT shim for the Linux->Windows cross build.
// The recompiler emits calls to C23 round-to-nearest-even helpers for the PPC
// vrfin/round vector ops, but the msvc-wine UCRT (10.0.26100) doesn't export
// them. Provide them here. nearbyint ties-to-even in the default FE_TONEAREST
// mode the guest runs in, which is exactly roundeven's semantics.
#include <cmath>
extern "C" float       roundevenf(float x)       { return std::nearbyint(x); }
extern "C" double      roundeven(double x)        { return std::nearbyint(x); }
extern "C" long double roundevenl(long double x)  { return std::nearbyint(x); }
