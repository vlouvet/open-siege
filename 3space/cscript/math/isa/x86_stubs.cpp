// x86 ISA stubs: we have no SSE2/SSE4.1/AVX implementations yet,
// so redirect all x86 backends to scalar. The dispatch table initializer
// in math_backend.cpp will call these if the CPU reports the capability;
// falling back to scalar is correct and safe.
//
// mInstallLibrary_ASM: T3D legacy assembly math installer. We use the
// math_backend dispatch system instead — this is a no-op.
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)

void mInstallLibrary_ASM() {}

#include "float4_dispatch.h"
#include "float3_dispatch.h"
#include "mat44_dispatch.h"

namespace math_backend::float4::dispatch {
    void install_sse2()  { install_scalar(); }
    void install_sse41() { install_scalar(); }
    void install_avx()   { install_scalar(); }
    void install_avx2()  { install_scalar(); }
}

namespace math_backend::float3::dispatch {
    void install_sse2()  { install_scalar(); }
    void install_sse41() { install_scalar(); }
    void install_avx()   { install_scalar(); }
    void install_avx2()  { install_scalar(); }
}

namespace math_backend::mat44::dispatch {
    void install_sse2()  { install_scalar(); }
    void install_sse41() { install_scalar(); }
    void install_avx()   { install_scalar(); }
    void install_avx2()  { install_scalar(); }
}

#endif
