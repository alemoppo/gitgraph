# check_encoding.cmake - igiene dei file di testo del progetto.
#
# Due regole, su ogni file di sorgente, script e documentazione:
#   1. i byte devono essere UTF-8 valido secondo le sole regole di codifica
#      (lead e code unit ammessi; esclusi codici overlong, surrogate e
#      punte oltre U+10FFFF);
#   2. non devono comparire i caratteri C3 82 e C3 83 (nel testo UTF-8
#      sono esattamente "A con accento sinistro" e "A con tilde"): non
#      sono mai legittimi in questo progetto (gli accenti si scrivono con
#      l'apostrofo ASCII) e compaiono solo come mojibake, cioe' un UTF-8
#      letto e riscritto come se fosse Latin-1 (per esempio il
#      superscritto 2 diventato "G" piu' quei due byte).
#
# Uso: cmake -DGG_SOURCE_DIR=<root> -P tests/check_encoding.cmake
#
# Il file script stesso e' nella lista controllata: per questo non contiene
# nessun byte oltre lo 0x7F, nemmeno nei messaggi.

if(NOT GG_SOURCE_DIR)
    message(FATAL_ERROR "check_encoding: GG_SOURCE_DIR mancante")
endif()

# Un token e' una sequenza UTF-8 completa in hex minuscolo (file(READ HEX)):
# ASCII, o un lead seguito dalle sue code unit. Le alternative sono disgiunte
# sulla prima cifra (0-7, c, d, e, f) e sulla seconda dove serve, quindi il
# motore di regex di CMake non deve fare backtrack: ogni lead ammette un solo
# ramo possibile.
string(CONCAT gg_tok
       "([0-7][0-9a-f]" # ASCII
       "|c[2-9a-f][89ab][0-9a-f]" # lead C2-DF (C0/C1 sarebbero overlong)
       "|d[0-9a-f][89ab][0-9a-f]" # lead D0-DF
       "|e0[a-b][0-9a-f][89ab][0-9a-f]" # E0: seconda code unit A0-BF, no overlong
       "|e[1-c][89ab][0-9a-f][89ab][0-9a-f]" # E1-EC
       "|ed[8-9][0-9a-f][89ab][0-9a-f]" # ED: seconda code unit 80-9F, no surrogate
       "|e[e-f][89ab][0-9a-f][89ab][0-9a-f]" # EE-EF
       "|f0[9-b][0-9a-f][89ab][0-9a-f][89ab][0-9a-f]" # F0: 90-BF, no overlong
       "|f[1-3][89ab][0-9a-f][89ab][0-9a-f][89ab][0-9a-f]" # F1-F3
       "|f4[8][0-9a-f][89ab][0-9a-f][89ab][0-9a-f]" # F4: 80-8F, fino a U+10FFFF
       ")")

# Valida l'hex di un file per pezzi: il regex (ricorsivo) va in crash oltre
# poche decine di KB, quindi il file si percorre a pezzi di 8000 hex (4000
# byte) sovrapposti di 16. La sovrapposizione garantisce che ogni byte stia
# strettamente dentro almeno un pezzo: un residuo che non e' vuoto ne' un
# suffisso di coda (una sequenza tagliata pesa al massimo 6 hex) spiega il
# pezzo, quindi quel byte non sta in nessuna sequenza UTF-8 valida. Il
# residuo di testa (il pezzo comincia a meta' di una sequenza) si elimina
# provando a tagliare 0, 2, 4 o 6 hex dall'inizio.
function(gg_valida_hex hex dove)
    string(LENGTH "${hex}" tot)
    set(L 8000)
    math(EXPR S "${L} - 16")
    set(i 0)
    while(i LESS tot)
        string(SUBSTRING "${hex}" ${i} ${L} chunk)
        set(ok FALSE)
        foreach(k 0 2 4 6)
            string(SUBSTRING "${chunk}" ${k} -1 c2)
            string(REGEX REPLACE "${gg_tok}+" "" left "${c2}")
            string(LENGTH "${left}" ll)
            if(ll EQUAL 0)
                set(ok TRUE)
                break()
            endif()
            if(ll LESS_EQUAL 6)
                string(LENGTH "${c2}" l2)
                math(EXPR st "${l2} - ${ll}")
                string(SUBSTRING "${c2}" ${st} ${ll} frag)
                if(frag STREQUAL left)
                    set(ok TRUE)
                    break()
                endif()
            endif()
        endforeach()
        if(NOT ok)
            string(SUBSTRING "${left}" 0 2 b)
            math(EXPR off "${i} / 2")
            message("encoding FAIL: ${dove}")
            message("  byte 0x${b} non valido come UTF-8 verso il byte ${off}")
            set(gg_bad 1 PARENT_SCOPE)
            return()
        endif()
        math(EXPR i "${i} + ${S}")
    endwhile()
    set(gg_bad 0 PARENT_SCOPE)
endfunction()

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

    gg_valida_hex("${gg_hex}" "${f}")
    if(gg_bad)
        set(gg_any 1)
    endif()

    # mojibake: in UTF-8 valido quei due byte sono sempre e solo quei due
    # caratteri (un lead C3 puo' stare solo all'inizio di una sequenza di
    # due byte, quindi la coppia non e' mai interna a un'altra sequenza)
    foreach(gg_moj "c382" "c383")
        string(FIND "${gg_hex}" "${gg_moj}" gg_pos)
        if(NOT gg_pos EQUAL -1)
            math(EXPR gg_off "${gg_pos} / 2")
            message("encoding FAIL: ${f}")
            message("  byte 0x${gg_moj} (mojibake di un accento) al byte ${gg_off}")
            set(gg_any 1)
        endif()
    endforeach()
endforeach()

if(gg_any)
    message(FATAL_ERROR
            "encoding: file non UTF-8 valido o con mojibake: correggere i "
            "byte indicati (il testo del progetto resta in ASCII con "
            "apostrofi)")
endif()

message("encoding: ${gg_n} file UTF-8 valido, nessun mojibake")
