add_executable(demangle EXCLUDE_FROM_ALL demangle.cpp)
add_executable(mcjitcache EXCLUDE_FROM_ALL mcjitcache.cpp)

add_executable(publicize publicize.cpp)
target_link_libraries(publicize curses dl pthread ${LLVM_LIBS})
