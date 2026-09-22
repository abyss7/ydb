#define GN_SLOT_STR_(x) #x
#define GN_SLOT_STR(x) GN_SLOT_STR_(x)

#include "slot.h"
#include GN_SLOT_STR(build/gn/slots/GN_SLOT.h)

#ifdef GN_SLOT_GUARD_LIST
GN_SLOT_GUARD_LIST(GN_SLOT_DECL_CXX, GN_SLOT_DECL_C)
GN_SLOT_GUARD_LIST(GN_SLOT_GUARD_CXX, GN_SLOT_GUARD_C)
#endif
