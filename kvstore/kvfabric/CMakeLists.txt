cmake_minimum_required(VERSION 3.18)
project(KVFabric LANGUAGES C)

option(KVF_BUILD_TESTS "Build KV-Fabric protocol and directory tests" ON)
option(KVF_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(KVF_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=address,undefined -no-pie)
endif()

add_library(kvfabric STATIC src/identity.c src/wire.c src/net.c src/crc32c.c
    src/directory.c src/control.c src/client.c)
add_library(KVFabric::kvfabric ALIAS kvfabric)
target_include_directories(kvfabric PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_options(kvfabric PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(kvfabric PUBLIC OpenSSL::Crypto Threads::Threads)

foreach(tool kvfabricd kvfctl)
    add_executable(${tool} tools/${tool}.c)
    target_link_libraries(${tool} PRIVATE kvfabric)
    target_compile_options(${tool} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

if(KVF_BUILD_TESTS)
    enable_testing()
    foreach(test protocol directory)
        add_executable(test_kvf_${test} tests/test_${test}.c)
        target_link_libraries(test_kvf_${test} PRIVATE kvfabric)
        target_compile_options(test_kvf_${test} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME kvf_${test} COMMAND test_kvf_${test})
    endforeach()
endif()

set(KVF_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KVFabric")
configure_package_config_file(cmake/KVFabricConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/KVFabricConfig.cmake"
    INSTALL_DESTINATION "${KVF_CMAKE_DIR}")
install(TARGETS kvfabric kvfabricd kvfctl EXPORT KVFabricTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/kvfabric DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT KVFabricTargets NAMESPACE KVFabric:: DESTINATION "${KVF_CMAKE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/KVFabricConfig.cmake" DESTINATION "${KVF_CMAKE_DIR}")
