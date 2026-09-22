#pragma once

#include <string.h>
#include <util/system/compiler.h>

namespace NMalloc {
    struct TMallocInfo {
        TMallocInfo();

        const char* Name;

        bool (*SetParam)(const char* param, const char* value);
        const char* (*GetParam)(const char* param);

        bool (*CheckParam)(const char* param, bool defaultValue);
    };

    extern volatile bool IsAllocatorCorrupted;
    void AbortFromCorruptedAllocator(const char* errorMessage = nullptr);

    // this function should be implemented by malloc implementations
    // gn: weak references to the allocator link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_allocator)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif
    TMallocInfo MallocInfo();
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_allocator)
#pragma clang attribute pop
#endif

    struct TAllocHeader {
        void* Block;
        size_t AllocSize;
        void Y_FORCE_INLINE Encode(void* block, size_t size, size_t signature) {
            Block = block;
            AllocSize = size | signature;
        }
    };
}
