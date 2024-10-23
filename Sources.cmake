target_sources(Queijo.Lib PRIVATE
    lib/include/queijo/io.h
    lib/include/queijo/net.h

    lib/src/io.cpp
    lib/src/net.cpp
)

target_sources(Queijo.CLI PRIVATE
    cli/FileUtils.h
    cli/FileUtils.cpp

    cli/queijo.cpp
)
