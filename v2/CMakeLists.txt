cmake_minimum_required(VERSION 3.10)
project(memory_pool)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
add_compile_options(-Wall -O2)

# 查找pthread库
find_package(Threads REQUIRED)

# 设置目录
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)
set(TEST_DIR ${CMAKE_SOURCE_DIR}/tests)

# 源文件
file(GLOB SOURCES "${SRC_DIR}/*.cpp")

# 添加头文件目录
include_directories(${INC_DIR})

# 创建单元测试可执行文件
add_executable(unit_test 
    ${SOURCES}
    ${TEST_DIR}/UnitTest.cpp
)

# 创建性能测试可执行文件
add_executable(perf_test 
    ${SOURCES}
    ${TEST_DIR}/PerformanceTest.cpp
)

# PageCache 空闲链表桶回归测试
add_executable(page_cache_empty_free_list_test
    ${SRC_DIR}/PageCache.cpp
    ${TEST_DIR}/PageCacheEmptyFreeListTest.cpp
)

# PageCache 分割 Span 映射登记回归测试
add_executable(page_cache_span_mapping_test
    ${SRC_DIR}/PageCache.cpp
    ${TEST_DIR}/SpanMappingTest.cpp
)

# PageCache 重复释放回归测试
add_executable(page_cache_double_free_test
    ${SRC_DIR}/PageCache.cpp
    ${TEST_DIR}/PageCacheDoubleFreeTest.cpp
)

# PageCache 双向 Span 合并回归测试
add_executable(page_cache_bidirectional_coalescing_test
    ${SRC_DIR}/PageCache.cpp
    ${TEST_DIR}/PageCacheBidirectionalCoalescingTest.cpp
)

# 三级缓存 Span 生命周期、追踪器与对象安全回归测试
add_executable(allocator_safety_test
    ${SOURCES}
    ${TEST_DIR}/AllocatorSafetyTest.cpp
)

# 零字节请求批量归还回归测试
add_executable(zero_size_batch_return_test
    ${SOURCES}
    ${TEST_DIR}/ZeroSizeBatchReturnTest.cpp
)

# ThreadCache TLS 析构归还回归测试
add_executable(thread_cache_exit_test
    ${SOURCES}
    ${TEST_DIR}/ThreadCacheExitTest.cpp
)

# 链接pthread库
target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)
target_link_libraries(page_cache_empty_free_list_test PRIVATE Threads::Threads)
target_link_libraries(page_cache_span_mapping_test PRIVATE Threads::Threads)
target_link_libraries(page_cache_double_free_test PRIVATE Threads::Threads)
target_link_libraries(page_cache_bidirectional_coalescing_test PRIVATE Threads::Threads)
target_link_libraries(allocator_safety_test PRIVATE Threads::Threads)
target_link_libraries(zero_size_batch_return_test PRIVATE Threads::Threads)
target_link_libraries(thread_cache_exit_test PRIVATE Threads::Threads)

# 添加测试命令
add_custom_target(test
    COMMAND ./unit_test
    COMMAND ./page_cache_empty_free_list_test
    COMMAND ./page_cache_span_mapping_test
    COMMAND ./page_cache_double_free_test
    COMMAND ./page_cache_bidirectional_coalescing_test
    COMMAND ./allocator_safety_test
    COMMAND ./zero_size_batch_return_test
    COMMAND ./thread_cache_exit_test
    DEPENDS
        unit_test
        page_cache_empty_free_list_test
        page_cache_span_mapping_test
        page_cache_double_free_test
        page_cache_bidirectional_coalescing_test
        allocator_safety_test
        zero_size_batch_return_test
        thread_cache_exit_test
)

add_custom_target(perf
    COMMAND ./perf_test
    DEPENDS perf_test
)
