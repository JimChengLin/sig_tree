cmake_minimum_required(VERSION 3.8)
project(sig_tree)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse4.2 -Wall")

add_executable(sig_tree main.cpp
        bench/sig_tree_bench.cpp
        src/allocator.h
        src/autovector.h
        src/coding.h
        src/kv_trans_trait.h
        src/likely.h
        src/page_size.h
        src/sig_tree.h
        src/sig_tree_impl.h
        src/sig_tree_node_impl.h
        src/sig_tree_rebuild.h
        src/sig_tree_visit_impl.h
        src/slice.h
        test/sig_tree_test.cpp)