LIBRARY()

# gn: into ydb/library/arrow_clickhouse

PEERDIR(
    contrib/libs/apache/arrow
)

ADDINCL(
    ydb/library/arrow_clickhouse/base
    ydb/library/arrow_clickhouse
)

SRCS(
    AggregatingBlockInputStream.cpp
    IBlockInputStream.cpp
    MergingAggregatedBlockInputStream.cpp
)

END()
