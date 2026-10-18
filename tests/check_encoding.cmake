# check_encoding.cmake - i sorgenti e i test sono testo ASCII.
#
# Fallisce se un file in src/ o tests/ contiene un byte oltre 0x7F. Copre anche
# i byte C3 82 / C3 83 del mojibake (un accento UTF-8 riletto e riscritto come
# Latin-1): sono entrambi oltre 0x7F, quindi bastano per non trovarli mai piu'
# nei sorgenti. Gli accenti nei messaggi si compongono con le escape della
# codepage, non con byte nel file.
#
# Eseguito da ctest come:
#   cmake -DGG_SOURCE_DIR=<repo> -P questo-file

if(NOT DEFINED GG_SOURCE_DIR)
    message(FATAL_ERROR "GG_SOURCE_DIR non definita")
endif()

file(GLOB_RECURSE files "${GG_SOURCE_DIR}/src/*" "${GG_SOURCE_DIR}/tests/*")
foreach(f ${files})
    file(READ "${f}" hex HEX)
    # Toglie le coppie di cifre che rappresentano byte <= 0x7F. La scansione e'
    # sequenziale e non sovrapposta: una coppia a cavallo fra due byte non puo'
    # consumare un byte alto (perche' il suo primo carattere sarebbe comunque
    # una cifra <= 7), quindi quel che resta e' non vuoto se c'e' un byte alto.
    string(REGEX REPLACE "[0-7][0-9A-Fa-f]" "" rest "${hex}")
    if(NOT rest STREQUAL "")
        message(FATAL_ERROR
                "${f}: byte oltre 0x7F; src/ e tests/ sono ASCII (nessun "
                "accento, nessun C3 82 / C3 83)")
    endif()
endforeach()
