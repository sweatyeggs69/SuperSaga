# Usage: cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed.cmake
# Writes a C++ source defining `const unsigned char SYMBOL[]` and `const unsigned SYMBOL_size`.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}\nextern const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\nextern const unsigned ${SYMBOL}_size = ${size};\n")
