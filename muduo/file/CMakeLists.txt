add_library(uring STATIC Uring.cc)
target_compile_options(uring PUBLIC -std=c++23)
add_subdirectory(tests)