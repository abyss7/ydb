YQL_LIBRARY()

# gn: into ydb/core/formats/arrow

PEERDIR(
    contrib/libs/apache/arrow
    ydb/core/formats/arrow/switch
    ydb/library/accessor
    ydb/library/actors/core
    ydb/library/conclusion
    ydb/library/formats/arrow
    ydb/library/yverify_stream
)

SRCS(
    filter.cpp
    filter.h
)

END()
