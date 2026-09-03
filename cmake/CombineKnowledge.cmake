# Joins the built-in knowledge base with every contributed database, so a merged
# contribution reaches everyone who builds Astral.
file(READ ${SEED} _combined)
file(GLOB _contributed ${CONTRIB}/*.astral)
list(SORT _contributed)
foreach(_file ${_contributed})
    file(READ ${_file} _text)
    get_filename_component(_name ${_file} NAME)
    string(APPEND _combined "\n# --- contributed: ${_name}\n" "${_text}")
endforeach()
file(WRITE ${OUTPUT} "${_combined}")
