# Copy the template restuff.toml next to the exe, but only when absent, so
# user edits (gpu_plugin, fullscreen, ...) survive rebuilds.
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
endif()
