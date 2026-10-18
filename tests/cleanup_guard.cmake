# cleanup_guard.cmake - protezione delle operazioni distruttive degli script di
# test (la pulizia prima della generazione dei repository).
#
# I repository di prova vivono sotto la directory di build, ma "sotto" non si
# giudica con un confronto lessicale del prefisso: un componente del percorso
# puo' essere una junction (o un symlink, o un altro reparse point) che porta
# fuori dalla build, e cancellare quel percorso cancellerebbe lontano. Il
# controllo si fa quindi anche sul percorso reale, con i collegamenti risolti,
# e comprende il caso normale della prima esecuzione in cui il percorso non
# esiste ancora: una junction puo' stare su un antenato, quindi si risolve il
# piu' profondo antenato esistente.
#
# In piu' ogni script dichiara il percorso ATTESO (ricavato dalla build root) e
# il controllo pretende che il percorso da cancellare sia esattamente quello:
# un "-D" che lo sposta altrove viene rifiutato anche se restasse sotto la
# build.
#
# Uso (script CMake, nessun progetto):
#   include("${CMAKE_CURRENT_LIST_DIR}/cleanup_guard.cmake")
#   gg_cleanup_is_safe("${DIR}" "${ATTESO}" "${BUILD_ROOT}" ok motivo)
#   if(NOT ok)
#       message(FATAL_ERROR "pulizia rifiutata: ${motivo}")
#   endif()
#   file(REMOVE_RECURSE "${DIR}")

# file(REAL_PATH) basta da CMake 3.19 e cmake_path da 3.20: il progetto dichiara
# 3.20. La policy (dove esiste) decide l'ordine fra risoluzione dei collegamenti
# e collasso dei "..": per un controllo di sicurezza serve risolvere prima.
if(POLICY CMP0152)
    cmake_policy(SET CMP0152 NEW)
endif()

# gg_real_path(<percorso> <out>): percorso reale, collegamenti risolti.
# Se <percorso> non esiste si risolve il piu' profondo antenato esistente e vi
# si riaccodano i componenti mancanti (un componente inesistente non puo'
# essere una junction: non c'e' niente da seguire).
function(gg_real_path path out)
    get_filename_component(p "${path}" ABSOLUTE)
    set(tail "")
    while(NOT EXISTS "${p}")
        get_filename_component(leaf "${p}" NAME)
        get_filename_component(parent "${p}" DIRECTORY)
        if(parent STREQUAL "${p}")
            break() # radice raggiunta senza trovare niente: niente da risolvere
        endif()
        if(tail STREQUAL "")
            set(tail "${leaf}")
        else()
            set(tail "${leaf}/${tail}")
        endif()
        set(p "${parent}")
    endwhile()
    if(NOT EXISTS "${p}")
        set("${out}" "${path}" PARENT_SCOPE)
        return()
    endif()
    file(REAL_PATH "${p}" base)
    if(tail STREQUAL "")
        set("${out}" "${base}" PARENT_SCOPE)
    else()
        set("${out}" "${base}/${tail}" PARENT_SCOPE)
    endif()
endfunction()

# gg_strictly_under(<genitore> <figlio> <out>): vero se <figlio> sta *dentro*
# <genitore> e non coincide con esso. Confronto sui percorsi normalizzati e in
# minuscolo (Windows non distingue maiuscole e minuscole nei percorsi), con il
# separatore di percorso preteso: "C:/buildX" non sta dentro "C:/build".
function(gg_strictly_under parent child out)
    string(TOLOWER "${parent}" p)
    string(TOLOWER "${child}" c)
    cmake_path(NORMAL_PATH p)
    cmake_path(NORMAL_PATH c)
    if(NOT p MATCHES "/$")
        set(p "${p}/")
    endif()
    string(LENGTH "${p}" plen)
    string(LENGTH "${c}" clen)
    if(clen GREATER plen)
        string(SUBSTRING "${c}" 0 ${plen} pref)
        if(pref STREQUAL "${p}")
            set("${out}" TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    set("${out}" FALSE PARENT_SCOPE)
endfunction()

# gg_same_path(<a> <b> <out>): vero se i due percorsi sono lo stesso percorso,
# a meno di maiuscole/minuscole e di "./" e ".." (normalizzati).
function(gg_same_path a b out)
    string(TOLOWER "${a}" al)
    string(TOLOWER "${b}" bl)
    cmake_path(NORMAL_PATH al)
    cmake_path(NORMAL_PATH bl)
    if(al STREQUAL bl)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

# gg_test_dir_is_safe(<percorso> <build> <esito> <motivo>): esito TRUE solo se
# <percorso> si puo' cancellare senza rischi. Deve essere:
#   - assoluto;
#   - dentro <build> e diverso da <build>, nel percorso scritto;
#   - dentro la <build> *reale*, nel percorso reale (collegamenti risolti).
# Inoltre <build> non puo' essere la radice di un drive: "tutto sta sotto C:/"
# non e' una garanzia.
function(gg_test_dir_is_safe path build ok reason)
    set("${ok}" FALSE PARENT_SCOPE)
    set("${reason}" "percorso di pulizia non sicuro" PARENT_SCOPE)

    if(path STREQUAL "" OR build STREQUAL "")
        set("${reason}" "percorso o directory di build non definiti" PARENT_SCOPE)
        return()
    endif()
    if(NOT IS_ABSOLUTE "${path}" OR NOT IS_ABSOLUTE "${build}")
        set("${reason}" "percorso non assoluto (${path})" PARENT_SCOPE)
        return()
    endif()

    gg_real_path("${build}" breal)
    gg_real_path("${path}" preal)
    cmake_path(GET breal PARENT_PATH bparent)
    if(bparent STREQUAL "${breal}")
        set("${reason}"
            "la directory di build non puo' essere la radice di un drive (${breal})"
            PARENT_SCOPE)
        return()
    endif()

    # percorso scritto
    gg_strictly_under("${build}" "${path}" scritto_ok)
    if(NOT scritto_ok)
        set("${reason}"
            "il percorso di pulizia (${path}) non sta dentro la directory di build (${build})"
            PARENT_SCOPE)
        return()
    endif()

    # percorso reale: e' qui che si vedono le junction. Il percorso scritto
    # puo' non esistere ancora, ma se un suo antenato e' una junction il
    # percorso reale esce dalla build.
    gg_strictly_under("${breal}" "${preal}" reale_ok)
    if(NOT reale_ok)
        set("${reason}"
            "il percorso di pulizia, con i collegamenti risolti, esce dalla directory di build (${preal} non sta sotto ${breal}): un componente e' una junction o un symlink"
            PARENT_SCOPE)
        return()
    endif()

    set("${ok}" TRUE PARENT_SCOPE)
    set("${reason}" "percorso di test dentro la build (${preal})" PARENT_SCOPE)
endfunction()

# gg_cleanup_is_safe(<percorso> <atteso> <build> <esito> <motivo>): esito TRUE
# solo se <percorso> e' ESATTAMENTE <atteso> (normalizzato) e <atteso> si puo'
# cancellare senza rischi secondo gg_test_dir_is_safe. E' il solo controllo che
# gli script di generazione devono usare prima di cancellare o scrivere.
function(gg_cleanup_is_safe path expected build ok reason)
    set("${ok}" FALSE PARENT_SCOPE)
    set("${reason}" "percorso di pulizia non sicuro" PARENT_SCOPE)

    if(path STREQUAL "" OR expected STREQUAL "" OR build STREQUAL "")
        set("${reason}"
            "percorso, percorso atteso o directory di build non definiti"
            PARENT_SCOPE)
        return()
    endif()
    if(NOT IS_ABSOLUTE "${path}" OR NOT IS_ABSOLUTE "${expected}"
       OR NOT IS_ABSOLUTE "${build}")
        set("${reason}" "percorso non assoluto (${path})" PARENT_SCOPE)
        return()
    endif()

    gg_same_path("${path}" "${expected}" stesso)
    if(NOT stesso)
        set("${reason}"
            "il percorso di pulizia (${path}) non e' quello atteso (${expected})"
            PARENT_SCOPE)
        return()
    endif()

    gg_test_dir_is_safe("${path}" "${build}" dentro motivo)
    if(NOT dentro)
        set("${reason}" "${motivo}" PARENT_SCOPE)
        return()
    endif()

    set("${ok}" TRUE PARENT_SCOPE)
    set("${reason}" "percorso di test dentro la build (${path})" PARENT_SCOPE)
endfunction()
