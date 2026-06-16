# CMakeLists.txt for crankshaft-core tests

add_executable(crankshaft-core-tests
    test_main.cpp
    test_audio_router.cpp
)

target_link_libraries(crankshaft-core-tests PRIVATE crankshaft-core-lib gtest gmock)
target_link_libraries(crankshaft-core-tests PRIVATE Qt6::Core Qt6::Multimedia)

enable_testing()
add_test(NAME crankshaft-core-tests COMMAND crankshaft-core-tests)
