LIBRARY()

PEERDIR(
    contrib/libs/apache/arrow
    ydb/library/conclusion
    ydb/services/metadata/abstract
    ydb/library/actors/core
    ydb/core/formats/arrow/accessor/common
    ydb/library/formats/arrow/protos
)

SRCS(
    common.cpp  # gn: into ydb/core/formats/arrow
    constructor.cpp  # gn: into ydb/core/formats/arrow
    request.cpp  # gn: into ydb/core/formats/arrow
    accessor.cpp  # gn: into ydb/core/formats/arrow
)

GENERATE_ENUM_SERIALIZATION(accessor.h)

END()
