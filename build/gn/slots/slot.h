#pragma once

// The guard list of a link slot (build/gn/slots/<slot>.h, see
// //build/gn/link_slots.gni):
//
//     #define GN_SLOT_<slot>(CXX, C) \
//         CXX(<namespace>, <return type>, <name>, (<parameters>)) \
//         C(<attributes>, <return type>, <name>, (<parameters>))
//
// slot_guard.cpp expands it with GN_SLOT_GUARD_* into strong references,
// linked into every executable that selects the slot: a missing provider
// fails that link. The signatures must match the interface: a mismatch is a
// compile error (extern "C") or a reference nobody defines (C++ overload).

#define GN_SLOT_DECL_CXX(ns, ret, name, params) \
    namespace ns { ret name params; }
#define GN_SLOT_DECL_C(attrs, ret, name, params) \
    extern "C" attrs ret name params;

#define GN_SLOT_GUARD_CXX(ns, ret, name, params) \
    __attribute__((used, retain)) static ret (*const GnSlotGuard_##name) params = &ns::name;
#define GN_SLOT_GUARD_C(attrs, ret, name, params) \
    __attribute__((used, retain)) static ret (*const GnSlotGuard_##name) params = &::name;
