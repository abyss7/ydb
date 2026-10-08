#pragma once

#include <util/stream/output.h>
#include <util/string/cast.h>

namespace NLWTrace {
    struct TCheck {
        int Value;
        static int ObjCount;

        TCheck()
            : Value(0)
        {
            ObjCount++;
        }

        explicit TCheck(int value)
            : Value(value)
        {
            ObjCount++;
        }

        TCheck(const TCheck& o)
            : Value(o.Value)
        {
            ObjCount++;
        }

        ~TCheck() {
            ObjCount--;
        }

        bool operator<(const TCheck& rhs) const {
            return Value < rhs.Value;
        }
        bool operator>(const TCheck& rhs) const {
            return Value > rhs.Value;
        }
        bool operator<=(const TCheck& rhs) const {
            return Value <= rhs.Value;
        }
        bool operator>=(const TCheck& rhs) const {
            return Value >= rhs.Value;
        }
        bool operator==(const TCheck& rhs) const {
            return Value == rhs.Value;
        }
        bool operator!=(const TCheck& rhs) const {
            return Value != rhs.Value;
        }
        TCheck operator+(const TCheck& rhs) const {
            return TCheck(Value + rhs.Value);
        }
        TCheck operator-(const TCheck& rhs) const {
            return TCheck(Value - rhs.Value);
        }
        TCheck operator*(const TCheck& rhs) const {
            return TCheck(Value * rhs.Value);
        }
        TCheck operator/(const TCheck& rhs) const {
            return TCheck(Value / rhs.Value);
        }
        TCheck operator%(const TCheck& rhs) const {
            return TCheck(Value % rhs.Value);
        }

        void Add(const TCheck rhs) {
            Value += rhs.Value;
        }
        void Sub(const TCheck rhs) {
            Value -= rhs.Value;
        }
        void Inc() {
            ++Value;
        }
        void Dec() {
            --Value;
        }
    };

}

// defined in check.cpp: declared before any use, which would otherwise
// instantiate the primary templates
template <>
NLWTrace::TCheck FromStringImpl<NLWTrace::TCheck, char>(const char*, size_t);

template <>
void Out<NLWTrace::TCheck>(IOutputStream& o, TTypeTraits<NLWTrace::TCheck>::TFuncParam t);
