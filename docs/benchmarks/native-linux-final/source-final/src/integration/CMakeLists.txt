set(MINIINFER_NANO_POOL "MINIINFER" CACHE STRING "Integrated host KV buffers: NANO or MINIINFER")
set_property(CACHE MINIINFER_NANO_POOL PROPERTY STRINGS NANO MINIINFER)
set(MINIINFER_NANO_CUDA_POOL "MINIINFER" CACHE STRING "Integrated CUDA buffers: DIRECT or MINIINFER")
set_property(CACHE MINIINFER_NANO_CUDA_POOL PROPERTY STRINGS DIRECT MINIINFER)

set(NANO_DIR "${PROJECT_SOURCE_DIR}/src/nanovllm")
# 集成目标用同一套推理源码直接接入存储库。
add_executable(miniinfer
    "${NANO_DIR}/main.c"
    "${NANO_DIR}/cli.c"
    "${NANO_DIR}/cache.c"
    "${NANO_DIR}/engine.c"
    "${NANO_DIR}/model.c"
    "${NANO_DIR}/sampler.c"
    "${NANO_DIR}/tokenizer.c"
    "${NANO_DIR}/nano_spill.c")
target_compile_features(miniinfer PRIVATE c_std_11)
target_include_directories(miniinfer PRIVATE "${PROJECT_SOURCE_DIR}/include/nanovllm")
target_compile_definitions(miniinfer PRIVATE NANO_WITH_STORAGE=1)
target_compile_options(miniinfer PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic>)
target_link_libraries(miniinfer PRIVATE miniinfer_storage m)

if(TARGET miniinfer_remote)
    target_sources(miniinfer PRIVATE "${NANO_DIR}/nano_remote_kv.c")
    target_compile_definitions(miniinfer PRIVATE NANO_WITH_REMOTE_KV=1)
    target_link_libraries(miniinfer PRIVATE miniinfer_remote OpenSSL::Crypto)
endif()

if(NANO_WITH_CUDA)
    target_sources(miniinfer PRIVATE "${NANO_DIR}/nano_cuda.cu")
    target_compile_definitions(miniinfer PRIVATE NANO_WITH_CUDA=1)
    set_target_properties(miniinfer PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
endif()
if(MINIINFER_NANO_BLAS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENBLAS QUIET IMPORTED_TARGET openblas)
    endif()
    if(TARGET PkgConfig::OPENBLAS)
        target_compile_definitions(miniinfer PRIVATE NV_USE_OPENBLAS=1)
        target_link_libraries(miniinfer PRIVATE PkgConfig::OPENBLAS)
    endif()
endif()
if(MINIINFER_NANO_OPENMP)
    find_package(OpenMP REQUIRED COMPONENTS C)
    target_link_libraries(miniinfer PRIVATE OpenMP::OpenMP_C)
endif()

# 选择由 MiniInfer 内存池提供主机 KV 缓冲区。
if(MINIINFER_NANO_POOL STREQUAL "MINIINFER")
    target_compile_definitions(miniinfer PRIVATE NV_USE_MINIINFER_POOL=1)
elseif(NOT MINIINFER_NANO_POOL STREQUAL "NANO")
    message(FATAL_ERROR "MINIINFER_NANO_POOL must be NANO or MINIINFER")
endif()
# 选择由 MiniInfer 内存池提供 CUDA 缓冲区。
if(MINIINFER_NANO_CUDA_POOL STREQUAL "MINIINFER")
    if(NANO_WITH_CUDA)
        if(NOT MINIINFER_WITH_CUDA)
            message(FATAL_ERROR "The integrated CUDA pool requires MINIINFER_WITH_CUDA=ON or MINIINFER_NANO_CUDA_POOL=DIRECT")
        endif()
        target_compile_definitions(miniinfer PRIVATE NV_USE_MINIINFER_CUDA_POOL=1)
    endif()
elseif(NOT MINIINFER_NANO_CUDA_POOL STREQUAL "DIRECT")
    message(FATAL_ERROR "MINIINFER_NANO_CUDA_POOL must be DIRECT or MINIINFER")
endif()

install(TARGETS miniinfer RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

option(MINIINFER_VLLM_CUDA "Build CUDA page packing for the vLLM bridge" OFF)
option(MINIINFER_BUILD_VLLM_BRIDGE "Build the vLLM V1 KV-Fabric shared library" OFF)
if(MINIINFER_BUILD_VLLM_BRIDGE)
    if(NOT TARGET miniinfer_remote)
        message(FATAL_ERROR "The vLLM bridge requires MINIINFER_WITH_KVFABRIC")
    endif()
    find_path(KVF_NVTX_INCLUDE_DIR nvtx3/nvToolsExt.h
        HINTS "${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}" /usr/local/cuda/include REQUIRED)
    set_target_properties(kvfabric miniinfer_storage miniinfer_remote
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(miniinfer_storage SYSTEM PRIVATE "${KVF_NVTX_INCLUDE_DIR}")
    target_compile_definitions(miniinfer_storage PRIVATE MINIINFER_WITH_NVTX=1)
    target_link_libraries(miniinfer_storage PUBLIC ${CMAKE_DL_LIBS})

    add_library(kvf_vllm SHARED vllm_bridge.c)
    target_include_directories(kvf_vllm SYSTEM PRIVATE "${KVF_NVTX_INCLUDE_DIR}")
    target_compile_options(kvf_vllm PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic>)
    if(MINIINFER_VLLM_CUDA)
        find_package(CUDAToolkit REQUIRED)
        target_sources(kvf_vllm PRIVATE vllm_kv_cuda.cu)
        target_compile_options(kvf_vllm PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>)
        set_target_properties(kvf_vllm PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
        add_executable(kvf_cuda_bench "${PROJECT_SOURCE_DIR}/tools/kvf_cuda_bench.cu")
        target_include_directories(kvf_cuda_bench PRIVATE "${PROJECT_SOURCE_DIR}/include")
        target_link_libraries(kvf_cuda_bench PRIVATE kvf_vllm CUDA::cudart)
        set_target_properties(kvf_cuda_bench PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
    endif()
    target_link_libraries(kvf_vllm PRIVATE miniinfer_remote)
    install(TARGETS kvf_vllm LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
