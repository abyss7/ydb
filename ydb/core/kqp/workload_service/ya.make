LIBRARY()

SRCS(
    kqp_workload_service.cpp
)

PEERDIR(
    ydb/core/cms/console

    ydb/core/fq/libs/compute/common

    ydb/core/kqp/workload_service/actors  # gn: :cpu_load_actors

    ydb/core/mind  # gn: :tenant_node_enumeration

    ydb/library/actors/interconnect
)

YQL_LAST_ABI_VERSION()

END()

RECURSE(
    actors
    common
    tables
)

RECURSE_FOR_TESTS(
    ut
)
