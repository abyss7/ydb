YQL_LIBRARY()

# gn: move into parent

SRCS(
    tablets.h
    tablets.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/kqp/common/events
    ydb/core/kqp/runtime/common
    ydb/core/sys_view/common
    ydb/library/actors/core
)

END()
