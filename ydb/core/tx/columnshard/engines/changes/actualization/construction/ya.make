LIBRARY()

# gn: into ydb/core/tx/columnshard/engines

SRCS(
    context.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/changes/abstract
)

END()
