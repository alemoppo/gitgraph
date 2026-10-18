# utf8_check.cmake - validazione UTF-8 dei byte di un file, riusabile.
#
# Serve a due usi: tests/check_encoding.cmake la applica ai file del progetto,
# tests/test_utf8.cmake la prova su sequenze scelte (compresi i casi limite in
# fondo al file).
#
# Un token e' una sequenza UTF-8 completa in hex minuscolo (file(READ ... HEX)):
# ASCII, o un lead seguito dalle sue code unit. Le alternative sono disgiunte
# sulla prima cifra (0-7, c, d, e, f) e sulla seconda dove serve, quindi il
# motore di regex di CMake non deve fare backtrack: ogni lead ammette un solo
# ramo possibile. Sono esclusi i codici overlong (C0/C1, E0 80-9F, F0 80-8F), i
# surrogate (ED A0-BF), i lead che non possono comparire (F5-FF) e le punte
# oltre U+10FFFF (F4 90 e oltre).
#
# Uso: include("${CMAKE_CURRENT_LIST_DIR}/utf8_check.cmake")
#      gg_utf8_hex_valid(<hex> <esito>)   # esito: 0 valido, 1 non valido
#      gg_hex_has_mojibake(<hex> <esito>) # esito: 1 se c'e' mojibake

string(CONCAT gg_utf8_tok
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

# Un parziale: le prime 1-3 code unit di una sequenza UTF-8 valida, le sole che
# possono comparire in coda a un pezzo quando la sequenza prosegue nel pezzo
# successivo. Un byte di continuazione da solo (80-BF), o un lead proibito,
# non e' un parziale: un file non comincia con una continuazione e ogni lead
# deve avere tutte le sue code unit.
#
# Nota: i lead di 2 o 3 byte con tutte le loro continuazioni sono sequenze
# complete, quindi non restano mai come residuo (le consuma gg_utf8_tok): nel
# parziale compaiono solo i lead con le code unit ancora mancanti.
string(CONCAT gg_utf8_parziale
       "(c[2-9a-f]|d[0-9a-f]|e[0-9a-f]|f[0-4]"
       "|e0[a-b][0-9a-f]|e[1-c][89ab][0-9a-f]|ed[8-9][0-9a-f]|e[e-f][89ab][0-9a-f]"
       "|f0[9-b][0-9a-f]|f[1-3][89ab][0-9a-f]|f4[8][0-9a-f]"
       "|f0[9-b][0-9a-f][89ab][0-9a-f]|f[1-3][89ab][0-9a-f][89ab][0-9a-f]"
       "|f4[8][0-9a-f][89ab][0-9a-f])")

# gg_utf8_hex_valid(<hex> <out>): 0 se i byte sono UTF-8 validi, 1 altrimenti.
# In caso di rifiuto lascia in gg_utf8_fail_byte la prima cifra esadecimale del
# residuo e in gg_utf8_fail_offset la posizione in byte dell'inizio del pezzo
# che lo ha rifiutato (per il messaggio all'utente).
#
# L'hex si percorre a pezzi di 8000 cifre (4000 byte): il regex ricorsivo di
# CMake va in crash oltre poche decine di KB. Se un pezzo finisce a meta' di una
# sequenza, il residuo (un parziale, al massimo 6 cifre) viene riportato al
# pezzo successivo, che comincia esattamente da li' e completa la sequenza:
# nessun byte viene saltato senza essere controllato, e l'ultimo pezzo non ha
# nessun pezzo successivo che possa completare un residuo, quindi una sequenza
# incompleta o un byte non valido in fondo al file viene rifiutato.
function(gg_utf8_hex_valid hex out)
    set(L 8000)
    string(LENGTH "${hex}" tot)
    set(i 0)
    while(i LESS tot)
        string(SUBSTRING "${hex}" ${i} ${L} chunk)
        string(LENGTH "${chunk}" lc)
        math(EXPR fine "${i} + ${lc}")
        if(fine GREATER_EQUAL tot)
            set(ultimo TRUE) # il pezzo arriva alla fine effettiva del file
        else()
            set(ultimo FALSE)
        endif()
        string(REGEX REPLACE "${gg_utf8_tok}+" "" resto "${chunk}")
        string(LENGTH "${resto}" rl)
        set(ok FALSE)
        if(rl EQUAL 0)
            set(ok TRUE)
        elseif(NOT ultimo)
            # il residuo deve essere un parziale valido *in coda* al pezzo (se
            # ci fosse altro materiale non valido prima, la concatenazione non
            # sarebbe un parziale valido): il pezzo successivo riparte da li'
            string(REGEX MATCH "^${gg_utf8_parziale}$" pr "${resto}")
            math(EXPR st "${lc} - ${rl}")
            string(SUBSTRING "${chunk}" ${st} ${rl} frag)
            if(pr AND frag STREQUAL resto)
                set(ok TRUE)
            endif()
        endif()
        if(NOT ok)
            string(SUBSTRING "${resto}" 0 2 b)
            math(EXPR off "${i} / 2")
            set(gg_utf8_fail_byte "${b}" PARENT_SCOPE)
            set(gg_utf8_fail_offset "${off}" PARENT_SCOPE)
            set("${out}" 1 PARENT_SCOPE)
            return()
        endif()
        if(rl EQUAL 0)
            math(EXPR i "${i} + ${L}")
        else()
            math(EXPR i "${i} + ${lc} - ${rl}") # riparte dal parziale in coda
        endif()
    endwhile()
    set(gg_utf8_fail_byte "" PARENT_SCOPE)
    set(gg_utf8_fail_offset 0 PARENT_SCOPE)
    set("${out}" 0 PARENT_SCOPE)
endfunction()

# gg_hex_has_mojibake(<hex> <out>): 1 se compare C3 82 o C3 83. In UTF-8 valido
# quei due byte sono sempre e solo "A con accento sinistro" e "A con tilde" (un
# lead C3 puo' stare solo all'inizio di una sequenza di due byte, quindi la
# coppia non e' mai interna a un'altra sequenza): nel progetto non sono mai
# legittimi (gli accenti si scrivono con l'apostrofo ASCII) e compaiono solo
# come mojibake.
function(gg_hex_has_mojibake hex out)
    set(bad 0)
    foreach(m "c382" "c383")
        string(FIND "${hex}" "${m}" pos)
        if(NOT pos EQUAL -1)
            set(bad 1)
        endif()
    endforeach()
    set("${out}" "${bad}" PARENT_SCOPE)
endfunction()
