LIBRARY()

# gn: into ydb/core/tx/tracing/usage

SRCS(
    global.cpp
    actor.cpp
)

PEERDIR(
    ydb/core/protos
    ydb/core/tx/tracing/usage
)

END()
