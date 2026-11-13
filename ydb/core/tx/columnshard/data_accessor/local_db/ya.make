LIBRARY()

# gn: into ydb/core/tx/columnshard/data_accessor/abstract

SRCS(
    manager.cpp
    GLOBAL constructor.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/data_accessor/abstract
)

END()
