# cleanup_guard.cmake - protezione delle operazioni distruttive degli script di
# test (la pulizia prima della generazione dei repository).
#
# I repository di prova vivono sotto la directory di build, ma "sotto" non si
# giudica con un confronto lessicale del prefisso: un componente del percorso
# puo' essere una junction (o un symlink, o un altro reparse point) che porta
# fuori dalla build, e cancellare quel percorso cancellerebbe lontano. Il
# controllo si fa quindi anche sul percorso reale, con i collegamenti risolti,
# e comprende il caso normale della prima esecuzione in cui il percorso non
# esiste ancora: una junction puo' stare su un antenato. I componenti si
# risolvono da sinistra a destra e i ".." si valutano dopo la risoluzione; un
# componente che non si riesce a verificare (collegamento rotto) fa rifiutare
# il percorso invece di dichiararlo sicuro.
#
# In piu' ogni script dichiara il percorso ATTESO (ricavato dalla build root) e
# il controllo pretende che il percorso da cancellare sia esattamente quello:
# un "-D" che lo sposta altrove viene rifiutato anche se restasse sotto la
# build.
#
# Confine di fiducia: gli script ricevono la build root (GG_BUILD_ROOT) e il
# percorso da pulire (GG_*_REPO) come parametri. La guardia ricava il percorso
# ATTESO dalla build root e da un nome scritto nel codice, non dal parametro da
# pulire, quindi un percorso atteso contraffatto *da solo* non passa. Se invece
# vengono alterati *tutti* i parametri (build root arbitraria e percorso da
# pulire costruito su di essa), la guardia non ha modo di sapere quale fosse la
# vera build del progetto: protegge dalla configurazione sbagliata e dagli
# errori, non da chi invoca lo script con parametri scelti ad arte. Una verifica
# piu' forte dovrebbe ancorarsi a qualcosa creato in fase di configure (per
# esempio un marcatore scritto da CMakeLists.txt dentro la build, o un file
# sentinella lasciato dai nostri stessi test nella directory da rimuovere) e
# rifiutare cio' che non lo contiene: e' una proposta, non una garanzia, perche'
# chi puo' riscrivere i parametri puo' anche ricreare il marcatore.
#
# Uso (script CMake, nessun progetto):
#   include("${CMAKE_CURRENT_LIST_DIR}/cleanup_guard.cmake")
#   gg_cleanup_is_safe("${DIR}" "${ATTESO}" "${BUILD_ROOT}" ok motivo)
#   if(NOT ok)
#       message(FATAL_ERROR "pulizia rifiutata: ${motivo}")
#   endif()
#   file(REMOVE_RECURSE "${DIR}")

# file(REAL_PATH) basta da CMake 3.19 e cmake_path da 3.20: il progetto dichiara
# 3.20. La policy CMP0152 (dove esiste) decide l'ordine fra risoluzione dei
# collegamenti e collasso dei "..", ma da sola NON basta per un controllo di
# sicurezza:
#   - su Windows file(REAL_PATH) di un percorso che passa da una junction
#     restituisce il percorso con i ".." comunque collassati, perche' e' il
#     sistema operativo a normalizzarli prima di aprire il percorso;
#   - una normalizzazione lessicale preliminare (get_filename_component
#     ABSOLUTE) annulla del tutto l'effetto della policy.
# gg_real_path risolve quindi i collegamenti PRIMA di collassare i "..",
# componente per componente, senza dipendere dalla policy. La policy si imposta
# comunque (NEW) come difesa in profondita' per ogni altro uso di REAL_PATH.
if(POLICY CMP0152)
    cmake_policy(SET CMP0152 NEW)
endif()

# gg_real_path(<percorso assoluto> <out>): percorso reale, con i collegamenti
# risolti e i ".." valutati *dopo* la risoluzione (semantica conservativa).
# Il risultato e' "" se un componente non e' verificabile in modo affidabile
# (per esempio un collegamento rotto o un percorso non assoluto): in quel caso
# il chiamante deve rifiutare.
#
# Perche' non basta file(REAL_PATH) sull'intero percorso: se il percorso non
# esiste ancora il risultato non e' affidabile, e su Windows un percorso che
# passa da una junction puo' avere i ".." collassati lessicalmente, cioe'
# prima della risoluzione, che e' proprio il caso da non credere. Qui si
# percorrono i componenti da sinistra a destra: ogni componente che esiste
# viene risolto subito (file(REAL_PATH)), un ".." toglie il padre del percorso
# *gia' risolto*, un componente inesistente viene accodato cosi' com'e' (non
# puo' essere un collegamento).
function(gg_real_path path out)
    set("${out}" "" PARENT_SCOPE)
    if(NOT IS_ABSOLUTE "${path}")
        return()
    endif()
    file(TO_CMAKE_PATH "${path}" p)
    cmake_path(GET p ROOT_PATH radice)
    cmake_path(GET p RELATIVE_PART resto)
    set(res "${radice}")
    string(REPLACE "/" ";" componenti "${resto}")
    foreach(c IN LISTS componenti)
        if(c STREQUAL "" OR c STREQUAL ".")
            continue()
        elseif(c STREQUAL "..")
            cmake_path(GET res PARENT_PATH padre)
            set(res "${padre}")
        else()
            if(res MATCHES "/$")
                set(cand "${res}${c}")
            else()
                set(cand "${res}/${c}")
            endif()
            if(EXISTS "${cand}")
                file(REAL_PATH "${cand}" risolto)
                set(res "${risolto}")
            elseif(IS_SYMLINK "${cand}")
                # collegamento rotto: non si puo' sapere dove porta
                return()
            else()
                set(res "${cand}")
            endif()
        endif()
    endforeach()
    set("${out}" "${res}" PARENT_SCOPE)
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
    if(breal STREQUAL "" OR preal STREQUAL "")
        set("${reason}"
            "percorso non verificabile in modo affidabile (collegamento rotto o percorso non assoluto)"
            PARENT_SCOPE)
        return()
    endif()
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
