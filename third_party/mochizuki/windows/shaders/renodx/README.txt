RenoDX DLSS5 add-on codec shaders, v8.5.0-rc10 (PEQHUB/RenoDX-DLSS5-Generic, branch v8.5dev), copied unchanged
apart from the speed changes below, which leave every output bit as it was. MIT, see LICENSE. Built for Vulkan by
linux/build/build_renodx.sh the way RenoDX builds its own Vulkan path (Slang, RENODX_VULKAN). Used by the
RenoDX-style option ([RenoDX] in dlssnr-amd.ini, linux/src/core/nr_renodx.cpp).

Changes (DLSSNR-AMD):
  look_compose.cs_5_1.hlsl   the 3x3 clip's neighbour values computed once per pixel for the group's tile
  look_common.hlsli,         the Detail stability history's three words in an RG32_UINT and an R32_UINT image
  look_compose.cs_5_1.hlsl   (12 bytes a texel; the RGBA32_UINT image carried a spare word)
  v6_resolve_commit.cs_5_1.hlsl (new)  the last pass's resolve and the commit in one pass when the commit takes no
                             pedestal
