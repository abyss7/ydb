YQL_LIBRARY()

# gn: move into parent

SRCS(
    nodes.h
    nodes.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/kqp/common/events
    ydb/core/kqp/runtime/common
    ydb/core/sys_view/common
    ydb/library/actors/core
)
# gn: peerdir ydb/core/mind:tenant_node_enumeration

END()
