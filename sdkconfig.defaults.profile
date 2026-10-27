# Profiling builds, loaded after sdkconfig.defaults.release.
# Release builds skip the PSRAM test at boot; profiling builds run it like
# debug builds do.
CONFIG_SPIRAM_MEMTEST=y
