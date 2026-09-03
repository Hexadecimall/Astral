# Embeds a text file into a C++ translation unit as a raw string literal, so a
# data file has exactly one source of truth and the library never depends on
# finding it on disk.
file(READ ${INPUT} _content)
get_filename_component(_name ${INPUT} NAME)
file(WRITE ${OUTPUT}
"// Generated from ${_name}. Do not edit.
namespace astral_internal {
extern const char *const ${SYMBOL};
const char *const ${SYMBOL} = R\"${TAG}(${_content})${TAG}\";
}
")
