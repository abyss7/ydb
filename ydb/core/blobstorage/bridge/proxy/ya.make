LIBRARY()

    SRCS(
        bridge_proxy.cpp  # gn: into ydb/core/blobstorage/dsproxy
        bridge_proxy.h
        bridge_proxy_counters.h
        defs.h
    )

    PEERDIR(
        ydb/core/blobstorage/bridge
        ydb/core/blobstorage/dsproxy
        ydb/core/blobstorage/groupinfo
    )

END()
