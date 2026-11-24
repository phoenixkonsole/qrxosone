# The runtimes build for m68k64-unknown-aros uses CMAKE_SYSTEM_NAME=Generic, which has no shared
# libraries: the shared targets (not built) would then write the same .a as the static ones.
# Included after project() (CMAKE_PROJECT_INCLUDE), when the platform file has set it to FALSE.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
