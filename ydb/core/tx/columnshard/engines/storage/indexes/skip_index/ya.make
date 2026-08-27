LIBRARY()


SRCS(
    meta.cpp
    constructor.cpp
    index_info_ext.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/scheme
    ydb/core/tx/columnshard/engines/scheme/abstract
    ydb/core/tx/columnshard/engines/scheme/indexes/abstract
)

END()
