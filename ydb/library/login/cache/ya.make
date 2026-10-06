LIBRARY()

# gn: move into parent

PEERDIR()

SRCS(
    lru.cpp
)

END()

RECURSE_FOR_TESTS(
    ut
)
