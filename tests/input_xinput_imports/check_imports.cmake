# cmake -DOBJDUMP=<objdump> -DEXE=<exe> -P check_imports.cmake
# Passes when EXE imports no XInput DLL: controllers go through SDL3, which loads
# XInput at run time, so XINPUT1_3.dll (the DirectX redistributable, absent on
# stock Windows) cannot stop the exe at start-up.
execute_process(COMMAND ${OBJDUMP} -p ${EXE}
                OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OBJDUMP} -p ${EXE} failed (${rc})")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" names "${dump}")
set(xinput "")
foreach(n IN LISTS names)
    string(REPLACE "DLL Name: " "" n "${n}")
    string(TOUPPER "${n}" n)
    if(n MATCHES "^XINPUT")
        list(APPEND xinput "${n}")
    endif()
endforeach()
message(STATUS "XInput imports: ${xinput}")
if(NOT xinput STREQUAL "")
    message(FATAL_ERROR "expected no XInput import, got '${xinput}': the exe would need that DLL to start")
endif()
