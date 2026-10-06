LIBRARY()

SRCS(
    metrics_printer.cpp
)

PEERDIR(
    ydb/library/actors/core
    ydb/library/yql/providers/solomon/actors
)

YQL_LAST_ABI_VERSION()

END()
