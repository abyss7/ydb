LIBRARY()

# gn: into ydb/core/tx/columnshard/engines

SRCS(
    index.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/scheme/versions
)

END()
