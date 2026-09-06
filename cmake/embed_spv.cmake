# Embed a SPIR-V binary as an alignas(4) uint32_t C array.
# util::CreateShaderModule takes const uint32_t* (alignment matters), so a
# plain xxd -i unsigned-char array is not safe.
# Usage: cmake -DSPV=<in.spv> -DOUT=<out.h> -DVAR=<identifier> -P embed_spv.cmake
file(READ "${SPV}" _hex HEX)
string(LENGTH "${_hex}" _len)  # nibbles; SPIR-V is always a multiple of 4 bytes
set(_body "")
set(_i 0)
while(_i LESS _len)
  string(SUBSTRING "${_hex}" ${_i} 8 _w)  # 4 bytes in file order b0 b1 b2 b3
  string(SUBSTRING "${_w}" 0 2 _b0)
  string(SUBSTRING "${_w}" 2 2 _b1)
  string(SUBSTRING "${_w}" 4 2 _b2)
  string(SUBSTRING "${_w}" 6 2 _b3)
  string(APPEND _body "0x${_b3}${_b2}${_b1}${_b0},")  # little-endian word
  math(EXPR _i "${_i} + 8")
endwhile()
file(WRITE "${OUT}" "// Generated from ${SPV} - do not edit.\n#include <cstdint>\n#include <cstddef>\nalignas(4) static const uint32_t ${VAR}[] = {${_body}};\nstatic const size_t ${VAR}_size_bytes = sizeof(${VAR});\n")
