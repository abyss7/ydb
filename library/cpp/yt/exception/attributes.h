#pragma once

#include <util/generic/string.h>

#include <variant>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TExceptionAttribute
{
    using TKey = TString;
    using TValue = std::variant<i64, double, bool, TString>;

    TKey Key;
    TValue Value;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
