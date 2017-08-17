#ifdef __MINGW32__

// Define some symbols referred to by MSVC libs compiled with /GS. Not having these symbols
// may cause linking errors when linking (e.g. undefined references to __security_check_cookie,
// __GSHandlerCheck and __chkstk when building with IPP)

#include <stdint.h>

#ifdef _WIN64
    #define HH_GS_CALL __cdecl
#else
    #define HH_GS_CALL __fastcall
#endif

extern "C"
{
    void HH_GS_CALL __chkstk();
    void HH_GS_CALL __chkstk() {}

    void HH_GS_CALL __GSHandlerCheck();
    void HH_GS_CALL __GSHandlerCheck() {}

    void HH_GS_CALL __security_check_cookie(uintptr_t);
    void HH_GS_CALL __security_check_cookie(uintptr_t) {}
}

#endif /*__MINGW32__*/
