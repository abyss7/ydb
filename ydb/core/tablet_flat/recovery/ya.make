LIBRARY()

SRCS(
    flat_executor_recovery.cpp
    flat_executor_recovery.h
)

GENERATE_ENUM_SERIALIZATION(flat_executor_recovery.h)

PEERDIR(
    ydb/core/base
    ydb/core/engine/minikql
    ydb/core/io_formats/cell_maker
    ydb/core/protos
    ydb/core/tablet_flat
    library/cpp/protobuf/json
    library/cpp/string_utils/base64
    ydb/library/actors/core
    yql/essentials/types/binary_json
)

YQL_LAST_ABI_VERSION()

END()
