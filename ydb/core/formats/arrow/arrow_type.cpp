#include "arrow_helpers.h"

#include "switch/switch_type.h"

namespace NKikimr::NArrow {

template <typename TType>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl() {
    return std::make_shared<TType>();
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::BooleanType>() {
    return arrow::uint8();
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::Decimal128Type>() {
    return arrow::fixed_size_binary(NScheme::FSB_SIZE);
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::FixedSizeBinaryType>() {
    return arrow::fixed_size_binary(NScheme::FSB_SIZE);
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::TimestampType>() {
    return arrow::timestamp(arrow::TimeUnit::TimeUnit::MICRO);
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::DurationType>() {
    return arrow::duration(arrow::TimeUnit::TimeUnit::MICRO);
}

arrow::Result<std::shared_ptr<arrow::DataType>> GetArrowType(NScheme::TTypeInfo typeInfo) {
    std::shared_ptr<arrow::DataType> result;
    bool success = SwitchYqlTypeToArrowType(typeInfo, [&]<typename TType>(TTypeWrapper<TType> typeHolder) {
        Y_UNUSED(typeHolder);
        result = CreateEmptyArrowImpl<TType>();
        return true;
    });

    if (success) {
        return result;
    }

    return arrow::Status::TypeError("unsupported type ", NKikimr::NScheme::TypeName(typeInfo));
}

}   // namespace NKikimr::NArrow
