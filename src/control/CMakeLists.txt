option(KVF_BUILD_TESTS "Build KV-Fabric protocol and directory tests" ON)
include(CMakePackageConfigHelpers)

add_library(kvfabric STATIC identity.c wire.c net.c directory.c control.c client.c
    "${PROJECT_SOURCE_DIR}/third_party/infer-io/src/base/crc32c.c")
target_compile_definitions(kvfabric PRIVATE KVF_CRC32C=1)
add_library(KVFabric::kvfabric ALIAS kvfabric)
target_include_directories(kvfabric PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_options(kvfabric PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(kvfabric PUBLIC OpenSSL::Crypto Threads::Threads)

foreach(tool kvfabricd kvfctl)
    add_executable(${tool} "${PROJECT_SOURCE_DIR}/tools/${tool}.c")
    target_link_libraries(${tool} PRIVATE kvfabric)
    target_compile_options(${tool} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

if(KVF_BUILD_TESTS)
    foreach(test protocol directory)
        add_executable(test_kvf_${test} "${PROJECT_SOURCE_DIR}/tests/control/test_${test}.c")
        if(test STREQUAL "directory")
            target_sources(test_kvf_${test} PRIVATE directory.c)
            target_compile_definitions(test_kvf_${test} PRIVATE KVF_DIRECTORY_TEST)
            target_link_options(test_kvf_${test} PRIVATE -Wl,--wrap=kvf_uuid_new)
        endif()
        target_link_libraries(test_kvf_${test} PRIVATE kvfabric)
        target_compile_options(test_kvf_${test} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME kvf_${test} COMMAND test_kvf_${test})
    endforeach()
endif()

set(KVF_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KVFabric")
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/KVFabricConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/KVFabricConfig.cmake"
    INSTALL_DESTINATION "${KVF_CMAKE_DIR}")
install(TARGETS kvfabric kvfabricd kvfctl EXPORT KVFabricTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/kvfabric" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT KVFabricTargets NAMESPACE KVFabric:: DESTINATION "${KVF_CMAKE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/KVFabricConfig.cmake" DESTINATION "${KVF_CMAKE_DIR}")
