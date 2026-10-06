UNITTEST_FOR(ydb/library/arrow_clickhouse)

FORK_SUBTESTS()

SPLIT_FACTOR(60)
SIZE(MEDIUM)

SRCS(
    ut_aggregator.cpp
)

PEERDIR(
    ydb/library/yql/udfs/common/clickhouse/client
)

END()
