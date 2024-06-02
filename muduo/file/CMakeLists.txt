add_library(muduo_uring STATIC Uring.cc)
target_compile_options(muduo_uring PUBLIC -std=c++23)
target_link_libraries(muduo_uring uring)
add_subdirectory(tests)