# Multiple small programs...

# TF-RT giexec
cuda_add_executable(tfrt_giexec tfrt_giexec.cpp)
target_link_libraries(tfrt_giexec nvinfer tensorflowrt glog gflags)

# TF-RT giexec
cuda_add_executable(tfrt_benchmark tfrt_benchmark.cpp)
target_link_libraries(tfrt_benchmark nvinfer tensorflowrt glog gflags)

# Testing some CUDA functions...
cuda_add_executable(cuda_tests cuda_tests.cpp cuda_tests.cu)
target_link_libraries(cuda_tests tensorflowrt visionworks nvxio glog gflags)

# Testing SegNet...
cuda_add_executable(segnet_tests segnet_tests.cpp)
target_link_libraries(segnet_tests tensorflowrt visionworks nvxio glog gflags)

# Installation
install(TARGETS tfrt_giexec DESTINATION bin)
