LIBRARY()

SRCS(
    cpu_load_actors.cpp  # gn: cpu_load_actors
    pool_handlers_actors.cpp
    scheme_actors.cpp  # gn: scheme_actors
)

PEERDIR(
    ydb/core/kqp/workload_service/common
    ydb/core/kqp/workload_service/tables

    ydb/core/tx/tx_proxy
)

YQL_LAST_ABI_VERSION()

END()
