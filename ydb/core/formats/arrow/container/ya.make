RECURSE(

# gn: into ydb/core/formats/arrow

    filterable
)

YQL_LIBRARY()

PEERDIR(
    contrib/libs/apache/arrow
    ydb/library/accessor
    ydb/library/actors/core
    ydb/library/conclusion
    ydb/library/formats/arrow
)

SRCS(
    container.cpp
    container.h
)

END()
