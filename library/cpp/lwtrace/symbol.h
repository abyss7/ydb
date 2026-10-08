#pragma once

#include <util/generic/string.h>
#include <util/stream/output.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/system/src_location.h>

#define LWTRACE_DEFINE_SYMBOL(variable, text)         \
    static TString variable##_holder(text);           \
    ::NLWTrace::TSymbol variable(&variable##_holder); \
    /**/

#define LWTRACE_INLINE_SYMBOL(text)           \
    [&] {                                     \
        static TString _holder(text);         \
        return ::NLWTrace::TSymbol(&_holder); \
    }() /**/

#define LWTRACE_LOCATION_SYMBOL                                                          \
    [](const char* func) {                                                               \
        static TString _holder(TStringBuilder() << func << " (" << __LOCATION__ << ")"); \
        return ::NLWTrace::TSymbol(&_holder);                                            \
    }(Y_FUNC_SIGNATURE) /**/

namespace NLWTrace {
    struct TSymbol {
        TString* Str;

        TSymbol()
            : Str(nullptr)
        {
        }

        explicit TSymbol(TString* str)
            : Str(str)
        {
        }

        TSymbol& operator=(const TSymbol& o) {
            Str = o.Str;
            return *this;
        }

        TSymbol(const TSymbol& o)
            : Str(o.Str)
        {
        }

        bool operator<(const TSymbol& rhs) const {
            return Str < rhs.Str;
        }
        bool operator>(const TSymbol& rhs) const {
            return Str > rhs.Str;
        }
        bool operator<=(const TSymbol& rhs) const {
            return Str <= rhs.Str;
        }
        bool operator>=(const TSymbol& rhs) const {
            return Str >= rhs.Str;
        }
        bool operator==(const TSymbol& rhs) const {
            return Str == rhs.Str;
        }
        bool operator!=(const TSymbol& rhs) const {
            return Str != rhs.Str;
        }
    };

}

// defined in symbol.cpp: declared before any use, which would otherwise
// instantiate the primary templates
template <>
NLWTrace::TSymbol FromStringImpl<NLWTrace::TSymbol, char>(const char*, size_t);

template <>
void Out<NLWTrace::TSymbol>(IOutputStream& o, TTypeTraits<NLWTrace::TSymbol>::TFuncParam t);
