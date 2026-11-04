find_package(Threads REQUIRED)

# Keep the historical per-backend choices by default.  The ablation builds
# configure the same target twice with -DSTM_ALLOCATOR_MODE=system and tier.
set(STM_ALLOCATOR_MODE "native" CACHE STRING
    "Allocator mode for the benchmark: native, system, or tier")
set_property(CACHE STM_ALLOCATOR_MODE PROPERTY STRINGS native system tier)
string(TOLOWER "${STM_ALLOCATOR_MODE}" STM_ALLOCATOR_MODE_NORMALIZED)
if(STM_ALLOCATOR_MODE_NORMALIZED STREQUAL "native")
    set(STM_ALLOCATOR_MODE_VALUE -1)
elseif(STM_ALLOCATOR_MODE_NORMALIZED STREQUAL "system")
    set(STM_ALLOCATOR_MODE_VALUE 0)
elseif(STM_ALLOCATOR_MODE_NORMALIZED STREQUAL "tier")
    set(STM_ALLOCATOR_MODE_VALUE 1)
else()
    message(FATAL_ERROR
        "STM_ALLOCATOR_MODE must be native, system, or tier; got '${STM_ALLOCATOR_MODE}'")
endif()

add_executable(stm_benchmark
    stm_benchmark.cpp
)

target_link_libraries(stm_benchmark PRIVATE
    mylib
    Threads::Threads
)

target_include_directories(stm_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/TierAlloc
)

# Benchmark binaries are deliberately independent from the test-only hooks and
# VERIFY mode.  Keep -march=native opt-in so recorded results remain portable.
target_compile_definitions(stm_benchmark PRIVATE
    NDEBUG
    STM_WW_VERIFY_LOGIC_MODE=0
    STM_WW_TEST_HOOKS=0
    STM_ALLOCATOR_MODE=${STM_ALLOCATOR_MODE_VALUE}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(stm_benchmark PRIVATE -O3)
endif()

option(STM_BENCHMARK_NATIVE "Enable -march=native for stm_benchmark" OFF)
if(STM_BENCHMARK_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(stm_benchmark PRIVATE -march=native)
endif()

# Explicitly built validation driver; the runtime library has no telemetry hooks.
add_executable(occ_memory_soak ../tests/validation/occ_memory_soak.cpp)
target_link_libraries(occ_memory_soak PRIVATE mylib Threads::Threads)
target_compile_definitions(occ_memory_soak PRIVATE
    NDEBUG STM_WW_VERIFY_LOGIC_MODE=0 STM_WW_TEST_HOOKS=0
    STM_ALLOCATOR_MODE=${STM_ALLOCATOR_MODE_VALUE})
