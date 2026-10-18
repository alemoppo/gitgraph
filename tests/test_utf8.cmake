# test_utf8.cmake - test della validazione UTF-8 (tests/utf8_check.cmake).
#
# Casi coperti:
#   - sequenze valide di 1, 2, 3 e 4 byte, compresi i limiti (U+07FF, U+0800,
#     U+D7FF, U+E000, U+FFFF, U+10000, U+10FFFF);
#   - sequenze non valide: overlong (C0/C1, E0 80-9F, F0 80-8F), surrogate
#     (ED A0-BF), punte oltre U+10FFFF (F4 90 e oltre), lead proibiti (F5-FF),
#     byte di continuazione orfani (80-BF) e lead incompleti;
#   - byte non validi o sequenze incompleti negli ultimi 1-3 byte del file:
#     devono essere rifiutati (sui pezzi non finali il residuo di coda e'
#     tollerato perche' il pezzo successivo lo completa; sull'ultimo pezzo no);
#   - sequenze a cavallo del confine fra i pezzi da 4000 byte, sia valide sia
#     non valide;
#   - mojibake (C3 82 e C3 83), che resta una regola a parte.
#
# Uso: cmake -P tests/test_utf8.cmake  (nessun argomento)

include("${CMAKE_CURRENT_LIST_DIR}/utf8_check.cmake")

# ---- casi validi -------------------------------------------------------------

function(gg_valido hex etichetta)
    gg_utf8_hex_valid("${hex}" bad)
    if(bad)
        message(FATAL_ERROR "utf8: '${etichetta}' doveva essere valido")
    endif()
endfunction()

gg_valido("" "vuoto")
gg_valido("41" "A (ASCII)")
gg_valido("7f" "DEL (ASCII)")
gg_valido("c2a9" "U+00A9, 2 byte")
gg_valido("c3a8" "U+00E8 (e accentata), 2 byte")
gg_valido("dfbf" "U+07FF, limite dei 2 byte")
gg_valido("e0a080" "U+0800, limite inferiore dei 3 byte")
gg_valido("e0a0a0" "3 byte generico")
gg_valido("ed9fbf" "U+D7FF, appena sotto i surrogate")
gg_valido("ee8080" "U+E000, appena sopra i surrogate")
gg_valido("efbfbf" "U+FFFF")
gg_valido("f0908080" "U+10000, limite inferiore dei 4 byte")
gg_valido("f0a0a0a0" "4 byte generico")
gg_valido("f3bfbfbf" "U+FFFFF")
gg_valido("f48fbfbf" "U+10FFFF, punta massima")
gg_valido("41c2a2e282acf09f9880" "piu' sequenze di lunghezza diversa")

# ---- casi non validi ---------------------------------------------------------

function(gg_non_valido hex etichetta)
    gg_utf8_hex_valid("${hex}" bad)
    if(NOT bad)
        message(FATAL_ERROR "utf8: '${etichetta}' doveva essere rifiutato")
    endif()
endfunction()

gg_non_valido("80" "continuazione orfana")
gg_non_valido("bf" "continuazione orfana massima")
gg_non_valido("c2" "lead 2 byte incompleto")
gg_non_valido("e180" "lead 3 byte incompleto")
gg_non_valido("f09080" "lead 4 byte incompleto")
gg_non_valido("c080" "overlong 2 byte")
gg_non_valido("c1bf" "overlong 2 byte (C1)")
gg_non_valido("e08080" "overlong 3 byte")
gg_non_valido("e09fbf" "overlong 3 byte (E0 9F)")
gg_non_valido("f0808080" "overlong 4 byte")
gg_non_valido("f08fbfbf" "overlong 4 byte (F0 8F)")
gg_non_valido("eda080" "surrogato U+D800")
gg_non_valido("edbfbf" "surrogato U+DFFF")
gg_non_valido("f4908080" "U+110000, oltre il massimo")
gg_non_valido("f5808080" "lead F5 proibito")
gg_non_valido("f7808080" "lead F7 proibito")
gg_non_valido("fe" "lead FE proibito")
gg_non_valido("ff" "lead FF proibito")
gg_non_valido("4180" "A piu' continuazione orfana")
gg_non_valido("41c341" "lead incompleto dentro il testo")
gg_non_valido("e18041" "sequenza di 3 byte troncata da ASCII")

# ---- fondo del file: gli ultimi 1-3 byte ------------------------------------
#
# Un file che finisce con una sequenza incompleta o con un byte non valido non
# e' UTF-8 valido. La tolleranza del residuo di coda valeva anche sull'ultimo
# pezzo: qui non deve valere.

gg_non_valido("41c2" "ultimo byte: lead 2 byte incompleto")
gg_non_valido("41e1" "ultimo byte: lead 3 byte incompleto")
gg_non_valido("41f0" "ultimo byte: lead 4 byte incompleto")
gg_non_valido("4180" "ultimo byte: continuazione orfana")
gg_non_valido("41e180" "ultimi 2 byte: sequenza di 3 troncata")
gg_non_valido("41eda080" "ultimi 3 byte: surrogato intero (non e' il troncamento, e' il codice)")
gg_non_valido("41f09080" "ultimi 3 byte: sequenza di 4 troncata")

# ---- sequenze a cavallo del confine fra due pezzi ---------------------------
#
# I pezzi sono di 8000 cifre (4000 byte): se un pezzo finisce a meta' di una
# sequenza, il residuo viene ripercorso dal pezzo successivo a partire dal suo
# inizio, quindi una sequenza che attraversa la cifra 8000 viene vista intera.

string(REPEAT "41" 3999 gg_testa) # 7998 cifre: la prossima sequenza scavalca
string(REPEAT "41" 5000 gg_coda)  # 10000 cifre: piu' di un pezzo

gg_valido("${gg_testa}c3a9${gg_coda}" "sequenza di 2 byte a cavallo del confine")
gg_valido("${gg_testa}e18080${gg_coda}" "sequenza di 3 byte a cavallo del confine")
gg_valido("${gg_testa}f0908080${gg_coda}" "sequenza di 4 byte a cavallo del confine")
gg_non_valido("${gg_testa}80${gg_coda}" "continuazione orfana a cavallo del confine")
gg_non_valido("${gg_testa}f09041${gg_coda}" "sequenza di 4 byte interrotta a cavallo del confine")

# ---- fondo del file su un file lungo (piu' di un pezzo) ---------------------

string(REPEAT "41" 4200 gg_lungo) # 8400 cifre: l'ultimo pezzo esiste

gg_valido("${gg_lungo}c3a9" "file lungo: sequenza completa in fondo")
gg_valido("${gg_lungo}f09f9880" "file lungo: sequenza di 4 byte completa in fondo")
gg_non_valido("${gg_lungo}c2" "file lungo: lead 2 byte incompleto in fondo")
gg_non_valido("${gg_lungo}e180" "file lungo: sequenza di 3 byte troncata in fondo")
gg_non_valido("${gg_lungo}f09080" "file lungo: sequenza di 4 byte troncata in fondo")
gg_non_valido("${gg_lungo}80" "file lungo: continuazione orfana in fondo")

# ---- mojibake ----------------------------------------------------------------

gg_hex_has_mojibake("c382" gg_moj)
if(NOT gg_moj)
    message(FATAL_ERROR "utf8: C3 82 doveva essere segnalato come mojibake")
endif()
gg_hex_has_mojibake("c383" gg_moj)
if(NOT gg_moj)
    message(FATAL_ERROR "utf8: C3 83 doveva essere segnalato come mojibake")
endif()
gg_hex_has_mojibake("41c3a941" gg_moj)
if(gg_moj)
    message(FATAL_ERROR "utf8: U+00E8 non e' mojibake")
endif()
gg_hex_has_mojibake("41" gg_moj)
if(gg_moj)
    message(FATAL_ERROR "utf8: ASCII puro non e' mojibake")
endif()

message("utf8: validazione UTF-8 verificata (valide, overlong, surrogate, "
        "oltre U+10FFFF, fondi di file, confini fra pezzi, mojibake)")
