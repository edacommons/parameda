include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW ON
)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(parameda_tests
    test_main.cpp
    test_version.cpp
    test_core.cpp
)
target_link_libraries(parameda_tests PRIVATE parameda::parameda doctest::doctest)

add_test(NAME parameda_tests COMMAND parameda_tests)
