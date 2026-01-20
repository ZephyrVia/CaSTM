# src/CMakeLists.txt

# 将 calculator.cpp 编译成一个名为 "mylib" 的静态库
add_library(mylib STATIC
    # CentralHeap 相关
    TierAlloc/CentralHeap/ChunkFreelist.cpp
    TierAlloc/CentralHeap/SystemChunkAllocator.cpp
    TierAlloc/CentralHeap/CentralHeap.cpp

    TierAlloc/ThreadHeap/Slab.cpp
    TierAlloc/ThreadHeap/SizeClassPool.cpp
    TierAlloc/ThreadHeap/ThreadHeap.cpp

    TierAlloc/common/SizeClassConfig.cpp

    EBRManager/EBRManager.cpp
    EBRManager/GarbageNode.cpp
    EBRManager/LockFreeSingleLinkedList.cpp
    EBRManager/ThreadSlot.cpp
    EBRManager/ThreadSlotManager.cpp

    OccSTM/Transaction.cpp
)

# 头文件目录（公开给依赖 mylib 的目标）
target_include_directories(mylib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/TierAlloc
)
