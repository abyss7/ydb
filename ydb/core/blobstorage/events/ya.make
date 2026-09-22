LIBRARY()

PEERDIR(
    ydb/core/base
    ydb/core/blobstorage/base
    ydb/core/blobstorage/pdisk
    ydb/core/protos
)

SRCS(
    blobstorage_events.cpp
    blobstorage_events.h
)

END()
