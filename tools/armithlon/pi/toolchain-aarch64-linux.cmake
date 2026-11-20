# Cross-build ARMithlon for 64-bit Raspberry Pi OS (AArch64 Linux) from an x86-64 Linux host.
# Generated helpers (Musashi's m68kmake) and CTest run through qemu-aarch64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Raspberry Pi 4 (Cortex-A72) is ARMv8.0-A: nothing newer may be emitted.
set(CMAKE_C_FLAGS_INIT "-march=armv8-a -mtune=cortex-a72")
set(QRX_PI_CPU "cortex-a72" CACHE STRING "qemu CPU model for tests: cortex-a72 (Pi 4) or cortex-a76 (Pi 5)")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-cpu;${QRX_PI_CPU};-L;/usr/aarch64-linux-gnu)
