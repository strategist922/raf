# Set options for 3rdparty/tvm
message("########## Configuring TVM ##########")
set(USE_LLVM "${MNM_USE_LLVM}" CACHE STRING "USE_LLVM for building tvm" FORCE)
set(USE_CUDA "${MNM_USE_CUDA}" CACHE STRING "USE_CUDA for building tvm" FORCE)
set(USE_SORT ON)
set(OpenGL_GL_PREFERENCE "GLVND")
# Introduce targets from tvm
add_subdirectory(${PROJECT_SOURCE_DIR}/3rdparty/tvm/)
# Get rid of clang's warning: argument unused during compilation: '-rdynamic'
set_target_properties(tvm tvm_runtime tvm_topi PROPERTIES
  COMPILE_FLAGS -Wno-unused-command-line-argument
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
add_custom_target(tvm_runtime_fix # PRE_BUILD
                  COMMAND cd ${PROJECT_SOURCE_DIR} &&
                  patch -f -p1 < ${PROJECT_SOURCE_DIR}/3rdparty/tvm_patch/tvm_fix.patch && echo 1 || echo 0)
add_dependencies(tvm_runtime tvm_runtime_fix)
# Avoid the side effect on cache variables introduced by tvm
unset(USE_CUDA CACHE)
unset(USE_OPENCL CACHE)
unset(USE_VULKAN CACHE)
unset(USE_OPENGL CACHE)
unset(USE_METAL CACHE)
unset(USE_ROCM CACHE)
unset(ROCM_PATH CACHE)
unset(USE_RPC CACHE)
unset(USE_THREADS CACHE)
unset(USE_LLVM CACHE)
unset(USE_STACKVM_RUNTIME CACHE)
unset(USE_GRAPH_RUNTIME CACHE)
unset(USE_GRAPH_RUNTIME_DEBUG CACHE)
unset(USE_RELAY_DEBUG CACHE)
unset(USE_SGX CACHE)
unset(USE_RTTI CACHE)
unset(USE_MSVC_MT CACHE)
unset(USE_MICRO CACHE)
unset(INSTALL_DEV CACHE)
unset(HIDE_PRIVATE_SYMBOLS CACHE)
unset(DLPACK_PATH CACHE)
unset(DMLC_PATH CACHE)
unset(RANG_PATH CACHE)
unset(COMPILER_RT_PATH CACHE)
unset(PICOJSON_PATH CACHE)
unset(USE_BLAS CACHE)
unset(USE_MKL_PATH CACHE)
unset(USE_CUDNN CACHE)
unset(USE_CUBLAS CACHE)
unset(USE_MIOPEN CACHE)
unset(USE_ROCBLAS CACHE)
unset(USE_SORT CACHE)
unset(USE_NNPACK CACHE)
unset(USE_RANDOM CACHE)
unset(USE_MICRO_STANDALONE_RUNTIME CACHE)
unset(USE_ANTLR CACHE)
message("########## Done configuring TVM ##########")
