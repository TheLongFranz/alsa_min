add_executable(tests src/test.cpp)

find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)

    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2
        GIT_TAG v3.9.1
        SYSTEM
    )
    FetchContent_MakeAvailable(Catch2)
endif()
include(Catch)

target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)
target_link_libraries(tests PRIVATE alsa_min::alsa_min)

catch_discover_tests(tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "MY_LIB_PCM_DEVICE=null")
