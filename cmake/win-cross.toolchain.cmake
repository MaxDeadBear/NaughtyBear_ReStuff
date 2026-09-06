# Linux->Windows cross toolchain for the MAIN (native-renderer) tree:
# clang-cl (MSVC ABI) + lld via msvc-wine, the win-amd64 rexglue 0.8.1.68 SDK
# (exact same g8dadea6 build the Linux tree links), the prebuilt win shaderc,
# and the case-correct include shadow so mixed-case Windows #includes resolve
# on this case-sensitive filesystem. Adapted from the proven lua_mods recipe.
#
#   cmake -S . -B out/build/wincross -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/win-cross.toolchain.cmake" \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo
include("/opt/msvc/cmake/toolchain-x64-clang.cmake")
add_compile_options(/utf-8 "-imsvc/home/Tynan/winsdk-case")
# The msvc toolchain sets FIND_ROOT_PATH_MODE_PACKAGE ONLY, so anything
# find_package/find_library must locate needs listing here explicitly.
# M4.27 era: v0.10.0 win SDK (headers carry the same LOCAL PATCHes as the
# Linux SDK builds -- textureCompressionBC field + simde fma include).
list(APPEND CMAKE_FIND_ROOT_PATH
     "/run/media/Tynan/Data/Restuff Project/Rexglue SDK/0.10.0/win-amd64/win-amd64"
     "/run/media/Tynan/Data/Restuff Project/Rexglue SDK/shaderc-win-amd64")
