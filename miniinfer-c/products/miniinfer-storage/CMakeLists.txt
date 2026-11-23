cmake_minimum_required(VERSION 3.18)
project(MiniInferCStorage VERSION 0.1.0 LANGUAGES C)

option(MINIINFER_WITH_CUDA "Build the C CUDA Runtime transfer backend" OFF)
option(MINIINFER_WITH_URING "Build the Infer io_uring backend" OFF)
option(MINIINFER_WITH_SPDK "Build the Infer SPDK bdev backend" OFF)
option(MINIINFER_BUILD_TESTS "Build storage tests" ON)
option(MINIINFER_BUILD_EXAMPLES "Build example programs" ON)
option(MINIINFER_SANITIZE "Enable AddressSanitizer, UndefinedBehaviorSanitizer and leak checking" OFF)
option(MINIINFER_WITH_KVFABRIC "Build MiniInfer remote storage for KV-Fabric" OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MINIINFER_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=address,undefined -no-pie)
endif()

find_package(Threads REQUIRED)
# 组装分层存储、内存池和 KV 相关源码。
add_library(miniinfer_storage STATIC
    src/common.c
    src/allocator.c
    src/cuda_adapter.c
    src/device_transfer.c
    src/model_mapping.c
    src/segment_pool.c
    src/graph_allocator.c
    src/storage_backend.c
    src/tiered_storage.c
    src/file_io.c
    src/direct_io.c
    src/kv_cache.c
    src/paged_kv.c)
target_include_directories(miniinfer_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(miniinfer_storage PUBLIC Threads::Threads)
target_compile_options(miniinfer_storage PRIVATE -Wall -Wextra -Wpedantic)

set(INFER_IO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/infer-io")
target_include_directories(miniinfer_storage PRIVATE "${INFER_IO_DIR}/include/c")
if(MINIINFER_WITH_KVFABRIC)
    find_package(KVFabric CONFIG REQUIRED)
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_KVFABRIC=1)
    target_link_libraries(miniinfer_storage PUBLIC KVFabric::kvfabric)
else()
    target_sources(miniinfer_storage PRIVATE "${INFER_IO_DIR}/src/base/crc32c.c")
endif()
# 仅在启用时编译并链接 io_uring 后端。
if(MINIINFER_WITH_URING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
    target_sources(miniinfer_storage PRIVATE src/uring_io.c)
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_URING)
    target_link_libraries(miniinfer_storage PUBLIC PkgConfig::URING)
endif()
# 仅在启用时编译 SPDK 后端并链接 SDK。
if(MINIINFER_WITH_SPDK)
    include("${INFER_IO_DIR}/cmake/InferSpdk.cmake")
    target_sources(miniinfer_storage PRIVATE src/spdk_bridge.c)
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_SPDK)
    target_link_libraries(miniinfer_storage PUBLIC InferSpdk::dependencies)
endif()

if(MINIINFER_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_compile_definitions(miniinfer_storage PUBLIC MINIINFER_WITH_CUDA=1)
    target_link_libraries(miniinfer_storage PUBLIC CUDA::cudart)
endif()

if(MINIINFER_BUILD_EXAMPLES)
    # 集成目标用同一套推理源码直接接入存储库。
    add_executable(miniinfer_basic examples/basic.c)
    target_link_libraries(miniinfer_basic PRIVATE miniinfer_storage)
endif()

if(MINIINFER_BUILD_TESTS)
    enable_testing()
    # 为六个独立测试创建并登记可执行目标。
    foreach(test_name pool graph tiered kv_cache paged_kv gpu_mock)
        add_executable(test_${test_name} tests/test_${test_name}.c)
        target_link_libraries(test_${test_name} PRIVATE miniinfer_storage)
        target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
        add_test(NAME ${test_name} COMMAND test_${test_name})
    endforeach()
    set_tests_properties(gpu_mock PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test_direct_io tests/test_direct_io.c)
    target_link_libraries(test_direct_io PRIVATE miniinfer_storage)
    target_compile_options(test_direct_io PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME direct_io_cpu COMMAND test_direct_io)
    set_tests_properties(direct_io_cpu PROPERTIES SKIP_RETURN_CODE 77)
    if(MINIINFER_WITH_CUDA)
        add_test(NAME direct_io_cuda COMMAND test_direct_io cuda)
        set_tests_properties(direct_io_cuda PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

install(TARGETS miniinfer_storage ARCHIVE DESTINATION lib)
install(DIRECTORY include/miniinfer DESTINATION include)

if(MINIINFER_WITH_KVFABRIC)
    add_library(miniinfer_remote STATIC src/remote/worker.c src/remote/transfer.c
        src/remote/cache.c src/remote/peer.c src/remote/prefetch.c)
    set_target_properties(miniinfer_remote PROPERTIES C_EXTENSIONS OFF)
    target_link_libraries(miniinfer_remote PUBLIC miniinfer_storage)
    target_compile_options(miniinfer_remote PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(miniinfer_worker src/remote/cli.c)
    set_target_properties(miniinfer_worker PROPERTIES C_EXTENSIONS OFF INSTALL_RPATH_USE_LINK_PATH TRUE)
    target_link_libraries(miniinfer_worker PRIVATE miniinfer_remote)
    target_compile_options(miniinfer_worker PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(kvf_bench tools/kvf_bench.c tools/fixture.c)
    set_target_properties(kvf_bench PROPERTIES C_EXTENSIONS OFF INSTALL_RPATH_USE_LINK_PATH TRUE)
    target_link_libraries(kvf_bench PRIVATE miniinfer_remote m)
    target_compile_options(kvf_bench PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS miniinfer_remote miniinfer_worker kvf_bench
        ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)

    if(MINIINFER_BUILD_TESTS)
        add_executable(test_remote_lifecycle tests/test_remote_lifecycle.c)
        set_target_properties(test_remote_lifecycle PROPERTIES C_EXTENSIONS OFF)
        target_link_libraries(test_remote_lifecycle PRIVATE miniinfer_storage)
        target_compile_options(test_remote_lifecycle PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME remote_lifecycle COMMAND test_remote_lifecycle)
        add_executable(test_remote_faults tests/test_remote_faults.c)
        set_target_properties(test_remote_faults PROPERTIES C_EXTENSIONS OFF)
        target_link_libraries(test_remote_faults PRIVATE miniinfer_remote)
        target_compile_options(test_remote_faults PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME remote_faults COMMAND test_remote_faults)
        set_tests_properties(remote_faults PROPERTIES TIMEOUT 30)
        add_executable(test_remote_e2e tests/test_remote_e2e.c tools/fixture.c)
        set_target_properties(test_remote_e2e PROPERTIES C_EXTENSIONS OFF)
        target_link_libraries(test_remote_e2e PRIVATE KVFabric::kvfabric m)
        target_compile_options(test_remote_e2e PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME remote_e2e_cpu COMMAND test_remote_e2e
            $<TARGET_FILE:KVFabric::kvfabricd> $<TARGET_FILE:miniinfer_worker> cpu)
        set_tests_properties(remote_e2e_cpu PROPERTIES TIMEOUT 150)
        add_test(NAME remote_restart COMMAND test_remote_e2e
            $<TARGET_FILE:KVFabric::kvfabricd> $<TARGET_FILE:miniinfer_worker> cpu restart)
        set_tests_properties(remote_restart PROPERTIES TIMEOUT 60)
        if(MINIINFER_WITH_CUDA)
            add_test(NAME remote_e2e_cuda COMMAND test_remote_e2e
                $<TARGET_FILE:KVFabric::kvfabricd> $<TARGET_FILE:miniinfer_worker> cuda)
            set_tests_properties(remote_e2e_cuda PROPERTIES TIMEOUT 150)
        endif()
    endif()
endif()
