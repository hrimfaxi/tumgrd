# OpenWRT 交叉编译工具链配置
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ==================== SDK 路径配置 ====================
set(OPENWRT_SDK_PATH "$ENV{HOME}/temp/openwrt-sdk-25.12.2-x86-64_gcc-14.3.0_musl.Linux-x86_64")
set(TOOLCHAIN_PATH "${OPENWRT_SDK_PATH}/staging_dir/toolchain-x86_64_gcc-14.3.0_musl/bin")
set(CMAKE_SYSROOT "${OPENWRT_SDK_PATH}/staging_dir/target-x86_64_musl")

set(ENV{STAGING_DIR} "${OPENWRT_SDK_PATH}/staging_dir")

# ==================== 编译器设置 ====================
set(CMAKE_C_COMPILER "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-g++")
set(CMAKE_AR "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-ar")
set(CMAKE_RANLIB "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-ranlib")
set(CMAKE_STRIP "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-strip")
set(CMAKE_OBJCOPY "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-objcopy")
set(CMAKE_OBJDUMP "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-objdump")
set(CMAKE_NM "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-nm")
set(CMAKE_LINKER "${TOOLCHAIN_PATH}/x86_64-openwrt-linux-musl-ld")

# ==================== 查找路径设置 ====================
# 强制 find_package, find_path, find_library 只在 Sysroot 里面找
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ==================== 链接标志 ====================
# 让连接器能找到间接依赖的 .so
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib" CACHE STRING "Linker flags" FORCE)

# ==================== 其他设置 ====================
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

list(APPEND CMAKE_SYSTEM_INCLUDE_PATH "${CMAKE_SYSROOT}/usr/include")
list(APPEND CMAKE_SYSTEM_LIBRARY_PATH "${CMAKE_SYSROOT}/usr/lib")
