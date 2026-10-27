include_directories(${CLANG_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/src)
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

add_executable(refcount_checker refcount_checker.cpp)
target_link_libraries(refcount_checker clang-cpp ${LLVM_LIBS})
