add_executable(ttt2_relay_daemon
    main.cpp
    relay_engine.cpp
    session_manager.cpp
    http_api.cpp
)
target_compile_features(ttt2_relay_daemon PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(ttt2_relay_daemon PRIVATE /O2 /W4)
    target_compile_definitions(ttt2_relay_daemon PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(ttt2_relay_daemon PRIVATE ws2_32)
else()
    target_compile_options(ttt2_relay_daemon PRIVATE -O3 -Wall -Wextra -Wpedantic)
    if(UNIX)
        target_link_libraries(ttt2_relay_daemon PRIVATE pthread)
    endif()
endif()
