option(MINIINFER_WITH_CUDA "Build the C CUDA Runtime transfer backend" OFF)
option(MINIINFER_WITH_URING "Build the Infer io_uring backend" OFF)
option(MINIINFER_WITH_SPDK "Build the Infer SPDK bdev backend" OFF)
option(MINIINFER_BUILD_TESTS "Build storage tests" ON)
option(MINIINFER_BUILD_EXAMPLES "Build example programs" ON)

# 组装分层存储、内存池和 KV 相关源码。
add_library(miniinfer_storage STATIC
    common.c
    allocator.c
    cuda_adapter.c
    device_transfer.c
    model_mapping.c
    segment_pool.c
    graph_allocator.c
    storage_backend.c
    tiered_storage.c
    file_io.c
    direct_io.c
    kv_cache.c
    paged_kv.c)
target_include_directories(miniinfer_storage PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(miniinfer_storage PUBLIC Threads::Threads)
target_compile_options(miniinfer_storage PRIVATE -Wall -Wextra -Wpedantic)

set(INFER_IO_DIR "${PROJECT_SOURCE_DIR}/third_party/infer-io")
target_include_directories(miniinfer_storage PRIVATE "${INFER_IO_DIR}/include/c")
if(MINIINFER_WITH_KVFABRIC)
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_KVFABRIC=1)
    target_link_libraries(miniinfer_storage PUBLIC KVFabric::kvfabric)
else()
    target_sources(miniinfer_storage PRIVATE "${INFER_IO_DIR}/src/base/crc32c.c")
endif()
# 仅在启用时编译并链接 io_uring 后端。
if(MINIINFER_WITH_URING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
    target_sources(miniinfer_storage PRIVATE uring_io.c)
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_URING)
    target_link_libraries(miniinfer_storage PUBLIC PkgConfig::URING)
endif()
# 仅在启用时编译 SPDK 后端并链接 SDK。
if(MINIINFER_WITH_SPDK)
    include("${INFER_IO_DIR}/cmake/InferSpdk.cmake")
    target_sources(miniinfer_storage PRIVATE spdk_bridge.c)
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_SPDK)
    target_link_libraries(miniinfer_storage PUBLIC InferSpdk::dependencies)
endif()

if(MINIINFER_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_compile_definitions(miniinfer_storage PUBLIC MINIINFER_WITH_CUDA=1)
    target_link_libraries(miniinfer_storage PUBLIC CUDA::cudart)
endif()

if(MINIINFER_BUILD_EXAMPLES)
    add_executable(miniinfer_basic "${PROJECT_SOURCE_DIR}/examples/basic.c")
    target_link_libraries(miniinfer_basic PRIVATE miniinfer_storage)
endif()

if(MINIINFER_BUILD_TESTS)
    # 为六个独立测试创建并登记可执行目标。
    foreach(test_name pool graph tiered kv_cache paged_kv gpu_mock)
        add_executable(test_${test_name} "${PROJECT_SOURCE_DIR}/tests/worker/test_${test_name}.c")
        target_link_libraries(test_${test_name} PRIVATE miniinfer_storage)
        target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
        add_test(NAME ${test_name} COMMAND test_${test_name})
    endforeach()
    set_tests_properties(gpu_mock PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test_direct_io "${PROJECT_SOURCE_DIR}/tests/worker/test_direct_io.c")
    target_link_libraries(test_direct_io PRIVATE miniinfer_storage)
    target_compile_options(test_direct_io PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME direct_io_cpu COMMAND test_direct_io)
    set_tests_properties(direct_io_cpu PROPERTIES SKIP_RETURN_CODE 77)
    if(MINIINFER_WITH_CUDA)
        add_test(NAME direct_io_cuda COMMAND test_direct_io cuda)
        set_tests_properties(direct_io_cuda PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

install(TARGETS miniinfer_storage ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/miniinfer" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(MINIINFER_WITH_KVFABRIC)
    add_library(miniinfer_remote STATIC remote/worker.c remote/transfer.c
        remote/cache.c remote/peer.c remote/prefetch.c)
    target_link_libraries(miniinfer_remote PUBLIC miniinfer_storage)
    target_compile_options(miniinfer_remote PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(miniinfer_worker remote/cli.c)
    set_target_properties(miniinfer_worker PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
    target_link_libraries(miniinfer_worker PRIVATE miniinfer_remote)
    target_compile_options(miniinfer_worker PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(kvf_bench "${PROJECT_SOURCE_DIR}/tools/kvf_bench.c"
        "${PROJECT_SOURCE_DIR}/tools/fixture.c")
    set_target_properties(kvf_bench PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
    target_link_libraries(kvf_bench PRIVATE miniinfer_remote m)
    target_compile_options(kvf_bench PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS miniinfer_remote miniinfer_worker kvf_bench
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

    if(MINIINFER_BUILD_TESTS)
        add_executable(test_remote_lifecycle "${PROJECT_SOURCE_DIR}/tests/worker/test_remote_lifecycle.c")
        target_link_libraries(test_remote_lifecycle PRIVATE miniinfer_storage)
        target_compile_options(test_remote_lifecycle PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME remote_lifecycle COMMAND test_remote_lifecycle)
        add_executable(test_remote_faults "${PROJECT_SOURCE_DIR}/tests/worker/test_remote_faults.c")
        target_link_libraries(test_remote_faults PRIVATE miniinfer_remote)
        target_compile_options(test_remote_faults PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME remote_faults COMMAND test_remote_faults)
        set_tests_properties(remote_faults PROPERTIES TIMEOUT 30)
        add_executable(test_remote_e2e "${PROJECT_SOURCE_DIR}/tests/worker/test_remote_e2e.c"
            "${PROJECT_SOURCE_DIR}/tools/fixture.c")
        target_link_libraries(test_remote_e2e PRIVATE KVFabric::kvfabric m)
        target_compile_options(test_remote_e2e PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME remote_e2e_cpu COMMAND test_remote_e2e
            $<TARGET_FILE:kvfabricd> $<TARGET_FILE:miniinfer_worker> cpu)
        set_tests_properties(remote_e2e_cpu PROPERTIES TIMEOUT 150)
        add_test(NAME remote_restart COMMAND test_remote_e2e
            $<TARGET_FILE:kvfabricd> $<TARGET_FILE:miniinfer_worker> cpu restart)
        set_tests_properties(remote_restart PROPERTIES TIMEOUT 60)
        if(MINIINFER_WITH_CUDA)
            add_test(NAME remote_e2e_cuda COMMAND test_remote_e2e
                $<TARGET_FILE:kvfabricd> $<TARGET_FILE:miniinfer_worker> cuda)
            set_tests_properties(remote_e2e_cuda PROPERTIES TIMEOUT 150)
        endif()
    endif()
endif()
