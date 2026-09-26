LIBRARY()

SRCS(
    actor.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/tx/tracing/service
    ydb/core/kqp/compute_actor
    yql/essentials/core/issue
)

END()
