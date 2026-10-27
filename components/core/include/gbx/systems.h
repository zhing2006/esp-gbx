#pragma once

// The systems whose games the build runs, 1 or 0: GBX_HAS_GB for the GB
// and GBC, GBX_HAS_GBA for the GBA. The core's build defines them from the
// GBX_SYSTEMS choice, see components/core/Kconfig; the PC build runs both.
// Code that tests them includes this header, so that a component built
// without them fails here rather than taking them as 0.
#if !defined(GBX_HAS_GB) || !defined(GBX_HAS_GBA)
#error "GBX_HAS_GB and GBX_HAS_GBA come from components/core/CMakeLists.txt"
#endif
#if !GBX_HAS_GB && !GBX_HAS_GBA
#error "The build runs no system"
#endif
