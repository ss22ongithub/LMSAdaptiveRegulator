# Thanks to Philipp Schuster https://gitlab.com/phip1611/cmake-kernel-module and all the previous folks
# This Setup helps Clion IDE to support with syntax highlightning and other conveniences while developing kernel modules. 
# Note: It does not build the kernel module using cmake , though it is possible to modify this to achieve the build 
# Prerequisites to install  1) Latest cmake 2) linux-headers installed under /usr/src/linux-headers-$(uname -r)


cmake_minimum_required(VERSION 3.5.0 FATAL_ERROR)

project("AdaptiveRegulator CLion support / CMake" VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find kernel headers
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(KernelHeaders REQUIRED)


# find MODULE_LICENSE("GPL"), MODULE_AUTHOR() etc.
# thanks to "merseyviking" from stack overflow
add_definitions(-D__KERNEL__ -DMODULE)

# this is needed in order for CLion IDE to provide syntax highlightning
# this is independent from the actual kernel object that is built
add_library(dummy  OBJECT
        # add all *.h and *.c files here that # CLion should cover
    ar.c
    ar.h
    ar_debugfs.c
    ar_debugfs.h
    ar_perfs.c
    ar_perfs.h
    kernel_headers.h
    master.c
    master.h
    model.c
    model.h
    mlp_weights.h
    l2_bw_scalers.h
    utils.c
    utils.h
)

set_target_properties(dummy PROPERTIES LINKER_LANGUAGE C)

# CLion IDE will find symbols from <linux/*>
target_include_directories("dummy" PRIVATE ${KERNELHEADERS_INCLUDE_DIRS})

#add_custom_target(kbuild ALL
#        COMMAND make -C /lib/modules/${CMAKE_SYSTEM_VERSION}/build M=${CMAKE_CURRENT_SOURCE_DIR} modules
#        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
#        COMMENT "Building Kernel Module..."
#)
