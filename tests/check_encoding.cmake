# check_encoding.cmake - igiene dei file di testo del progetto.
#
# Due regole, su ogni file di sorgente, script e documentazione:
#   1. i byte devono essere UTF-8 valido secondo le sole regole di codifica
#      (lead e code unit ammessi; esclusi codici overlong, surrogate e
#      punte oltre U+10FFFF). La validazione sta in tests/utf8_check.cmake,
#      insieme ai suoi test (tests/test_utf8.cmake): compresa la coda del file,
#      dove una sequenza incompleta o un byte non valido non e' tollerato;
#   2. non devono comparire i byte C3 82 e C3 83 (nel testo UTF-8 sono
#      esattamente "A con accento sinistro" e "A con tilde"): non sono mai
#      legittimi in questo progetto (gli accenti si scrivono con l'apostrofo
#      ASCII) e compaiono solo come mojibake, cioe' un UTF-8 letto e riscritto
#      come se fosse Latin-1 (per esempio il superscritto 2 diventato "G" piu'
#      quei due byte).
#
# Uso: cmake -DGG_SOURCE_DIR=<root> -P tests/check_encoding.cmake
#
# Lo script e' nella lista controllata: per questo non contiene nessun byte
# oltre lo 0x7F, nemmeno nei messaggi.

if(NOT GG_SOURCE_DIR)
    message(FATAL_ERROR "check_encoding: GG_SOURCE_DIR mancante")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/utf8_check.cmake")

set(gg_candidates "${GG_SOURCE_DIR}/README.md"
                   "${GG_SOURCE_DIR}/LICENSE"
                   "${GG_SOURCE_DIR}/CMakeLists.txt")
file(GLOB gg_src "${GG_SOURCE_DIR}/src/*.c" "${GG_SOURCE_DIR}/src/*.h")
file(GLOB gg_tests "${GG_SOURCE_DIR}/tests/*.c" "${GG_SOURCE_DIR}/tests/*.h"
                   "${GG_SOURCE_DIR}/tests/*.cmake" "${GG_SOURCE_DIR}/tests/*.bat")
list(APPEND gg_candidates ${gg_src} ${gg_tests})
set(gg_files "")
foreach(f IN LISTS gg_candidates)
    if(EXISTS "${f}")
        list(APPEND gg_files "${f}")
    endif()
endforeach()
list(LENGTH gg_files gg_n)
if(gg_n EQUAL 0)
    message(FATAL_ERROR "check_encoding: nessun file trovato sotto ${GG_SOURCE_DIR}")
endif()

set(gg_any 0)
foreach(f IN LISTS gg_files)
    file(READ "${f}" gg_hex HEX)
    string(TOLOWER "${gg_hex}" gg_hex)

    gg_utf8_hex_valid("${gg_hex}" gg_bad)
    if(gg_bad)
        message("encoding FAIL: ${f}")
        message("  byte 0x${gg_utf8_fail_byte} non valido come UTF-8 verso il byte "
                "${gg_utf8_fail_offset}")
        set(gg_any 1)
    endif()

    gg_hex_has_mojibake("${gg_hex}" gg_moj)
    if(gg_moj)
        foreach(m "c382" "c383")
            string(FIND "${gg_hex}" "${m}" gg_pos)
            if(NOT gg_pos EQUAL -1)
                math(EXPR gg_off "${gg_pos} / 2")
                message("encoding FAIL: ${f}")
                message("  byte 0x${m} (mojibake di un accento) al byte ${gg_off}")
                set(gg_any 1)
            endif()
        endforeach()
    endif()
endforeach()

if(gg_any)
    message(FATAL_ERROR
            "encoding: file non UTF-8 valido o con mojibake: correggere i "
            "byte indicati (il testo del progetto resta in ASCII con "
            "apostrofi)")
endif()

message("encoding: ${gg_n} file UTF-8 valido, nessun mojibake")
