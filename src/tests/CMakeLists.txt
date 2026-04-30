# CMakeLists.txt for crankshaft-core tests

add_executable(crankshaft-core-tests
    test_main.cpp
)

target_link_libraries(crankshaft-core-tests PRIVATE crankshaft-core-lib gtest gmock)

enable_testing()
add_test(NAME crankshaft-core-tests COMMAND crankshaft-core-tests)
