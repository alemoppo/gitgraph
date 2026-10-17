#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# make_test_repo.sh [percorso]
#
# Crea un repository Git isolato e ripetibile per i test di gitgraph.
# Default: ${TMPDIR:-/tmp}/gitgraph_test_repo
#
# NOTA: gitgraph e' supportato solo su Windows (vedi README). Questo script e'
# il gemello POSIX di make_test_repo.bat: viene mantenuto allineato per un
# eventuale porting, ma non e' eseguito dalla suite di test.
#
# SICUREZZA: lo script cancella una directory solo se e' una directory di test
# dedicata: il nome finale deve essere "gitgraph_test_repo" oppure
# "gitgraph_test_repo_<scenario>", il percorso non puo' contenere "." o "..",
# non puo' essere la radice, la directory corrente, la home o quella
# temporanea e, se la directory esiste, deve contenere il marcatore
# .gitgraph_test_repo con il testo esatto "gitgraph test repository" e nessun
# file diverso da quelli creati qui. Altrimenti lo script fallisce senza
# toccare nulla: il marcatore da solo non autorizza nessuna cancellazione.
#
# Stesso contenuto della versione .bat: main(A,B,C), branch1 da B, branch2
# da B2, merge --no-ff, fast-forward, branch cancellato dopo il merge, gruppo
# di alias e branch figlio ambiguo, gruppo isolato con date come spareggio
# (oA/oC/oB da R), remote-tracking con punte diverse, specchio con la stessa
# punta, ramo nato dal secondo genitore di un merge, merge ripetuti, storia
# ribasata, ramo senza antenati (genealogia non determinabile), branch con
# slash, piu' riferimenti sullo stesso commit, messaggio con accenti e piu'
# righe, uno stash.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="${1:-${GG_TEST_REPO:-${TMPDIR:-/tmp}/gitgraph_test_repo}}"
MARKER=".gitgraph_test_repo"
MARKERTEXT="gitgraph test repository"
BASENAME="gitgraph_test_repo"

die() { echo "ERRORE: $*" >&2; exit 1; }

# --- controlli di sicurezza sul percorso --------------------------------------
[ -n "$REPO" ] || die "percorso del repository di prova vuoto"
# percorsi con "." o "..": rifiutati, non normalizzati (nessuna risoluzione
# ambigua del percorso)
case "$REPO" in
    *..*|*/.|*/..|.|..|*/) die "\"$REPO\" contiene \".\" o \"..\": rifiutato" ;;
esac
# percorso assoluto con la directory superiore risolta fisicamente
case "$REPO" in
    /*) ;;
    *) REPO="$PWD/$REPO" ;;
esac
base="${REPO##*/}"
parent="${REPO%/*}"
[ -n "$parent" ] || parent="/"
if [ -d "$parent" ]; then
    parent="$(cd "$parent" && pwd -P)"
fi
REPO="$parent/$base"

# whitelist del nome: nessuna directory arbitraria puo' essere cancellata
if [ "$base" != "$BASENAME" ]; then
    case "$base" in
        "${BASENAME}_"?*) ;;
        *) die "\"$REPO\" non e' una directory di test ammessa: il nome deve essere \"$BASENAME\" o \"${BASENAME}_<scenario>\"" ;;
    esac
fi

[ "$REPO" = "/" ] && die "\"$REPO\" e' la radice del filesystem: rifiutato"
[ "$REPO" = "$PWD" ] && die "\"$REPO\" e' la directory corrente: rifiutato"
[ "$REPO" = "$HOME" ] && die "\"$REPO\" e' la home utente: rifiutato"
[ -n "${TMPDIR:-}" ] && [ "$REPO" = "${TMPDIR%/}" ] && die "\"$REPO\" e' la directory temporanea: rifiutato"
[ "$REPO" = "/tmp" ] && die "\"$REPO\" e' la directory temporanea: rifiutato"

if [ -d "$REPO" ]; then
    # il marcatore da solo non basta: testo esatto e nessun file imprevisto
    [ -f "$REPO/$MARKER" ] ||
        die "\"$REPO\" esiste ma non e' una directory di test di gitgraph: niente viene cancellato"
    [ "$(cat "$REPO/$MARKER")" = "$MARKERTEXT" ] ||
        die "\"$REPO/$MARKER\" non contiene il testo atteso: niente viene cancellato"
    for entry in "$REPO"/* "$REPO"/.[!.]*; do
        name="${entry##*/}"
        case "$name" in
            "$MARKER"|.git|hello.txt|wip.txt|msg-c.txt) ;;
            *) die "\"$name\" in \"$REPO\" non e' un file creato da questo script: niente viene cancellato" ;;
        esac
    done
    rm -rf -- "$REPO"
elif [ -e "$REPO" ]; then
    die "\"$REPO\" esiste e non e' una directory: rifiutato"
fi
mkdir -p "$REPO"
# marcatore: da qui in poi la directory e' riconoscibile come nostra
echo "$MARKERTEXT" > "$REPO/$MARKER"

export GIT_AUTHOR_NAME="Test User"
export GIT_AUTHOR_EMAIL="test@example.com"
export GIT_COMMITTER_NAME="Test User"
export GIT_COMMITTER_EMAIL="test@example.com"

git -C "$REPO" init -q -b main
git -C "$REPO" config commit.gpgsign false
git -C "$REPO" config core.autocrlf false

commit() {  # commit <data-iso> <messaggio...>
    GIT_AUTHOR_DATE="$1" GIT_COMMITTER_DATE="$1" \
        git -C "$REPO" commit -q --allow-empty -m "$2"
}

# --- main: A, B, C ----------------------------------------------------------
echo hello > "$REPO/hello.txt"
git -C "$REPO" add hello.txt
GIT_AUTHOR_DATE="2026-01-02T10:00:00+0000" GIT_COMMITTER_DATE="2026-01-02T10:00:00+0000" \
    git -C "$REPO" commit -q -m "A: prima revisione"
commit "2026-01-02T10:02:00+0000" "B: seconda revisione"

# C: messaggio con accenti e piu' righe (UTF-8)
printf '%s\n' "terza revisione: àccenti e più righe" "" "corpo del messaggio, con ünïcode: ✓" \
    > "$REPO/msg-c.txt"
GIT_AUTHOR_DATE="2026-01-02T10:04:00+0000" GIT_COMMITTER_DATE="2026-01-02T10:04:00+0000" \
    git -C "$REPO" commit -q --allow-empty -F "$REPO/msg-c.txt"
rm "$REPO/msg-c.txt"

# piu' riferimenti sullo stesso commit (C)
git -C "$REPO" tag v1.0-lw
GIT_COMMITTER_DATE="2026-01-02T11:40:00+0000" git -C "$REPO" tag -a v1.0 -m "versione 1.0"

# --- feature/x da C ---------------------------------------------------------
git -C "$REPO" checkout -q -b feature/x main
commit "2026-01-02T10:06:00+0000" "FX1: lavoro su feature/x"
commit "2026-01-02T10:08:00+0000" "FX2: altro lavoro su feature/x"

# --- branch1 da B -----------------------------------------------------------
git -C "$REPO" checkout -q -b branch1 main~1
commit "2026-01-02T10:10:00+0000" "B1: inizio su branch1"
commit "2026-01-02T10:12:00+0000" "B2: branch2 nascera' da qui"
commit "2026-01-02T10:14:00+0000" "B3: fine di branch1"
git -C "$REPO" tag b1-final branch1

# --- branch2 da B2 ----------------------------------------------------------
git -C "$REPO" checkout -q -b branch2 branch1~1
commit "2026-01-02T10:20:00+0000" "C1: inizio su branch2"
commit "2026-01-02T10:22:00+0000" "C2: fine di branch2"

# --- caso ambiguo: ambbase (X), amb1 e amb2 su X, ambchild da X -------------
# X poggia sulla radice A, per non interferire con branch1/branch2
git -C "$REPO" checkout -q -b ambbase main~2
commit "2026-01-02T10:30:00+0000" "X: punto di diramazione ambiguo"
git -C "$REPO" checkout -q -b amb1 ambbase
git -C "$REPO" checkout -q -b amb2 ambbase
git -C "$REPO" checkout -q -b ambchild ambbase
commit "2026-01-02T10:32:00+0000" "AC1: da X, padre non univoco"
commit "2026-01-02T10:34:00+0000" "AC2: fine di ambchild"

# --- merge --no-ff di branch1 in main ---------------------------------------
git -C "$REPO" checkout -q main
GIT_AUTHOR_DATE="2026-01-02T10:40:00+0000" GIT_COMMITTER_DATE="2026-01-02T10:40:00+0000" \
    git -C "$REPO" merge -q --no-ff branch1 -m "Merge branch 'branch1' (--no-ff)"
M1=$(git -C "$REPO" rev-parse HEAD)
# basi usate dagli scenari di genealogia
git -C "$REPO" tag base-b main~2
git -C "$REPO" tag base-c main~1

# --- branch temp: merge --no-ff poi cancellazione ---------------------------
git -C "$REPO" checkout -q -b temp main
commit "2026-01-02T10:50:00+0000" "T1: lavoro del branch temp"
git -C "$REPO" checkout -q main
GIT_AUTHOR_DATE="2026-01-02T11:00:00+0000" GIT_COMMITTER_DATE="2026-01-02T11:00:00+0000" \
    git -C "$REPO" merge -q --no-ff temp -m "Merge branch 'temp' (--no-ff)"
git -C "$REPO" branch -D temp >/dev/null

# --- fast-forward: ff con F1, main si allinea, poi D ------------------------
git -C "$REPO" checkout -q -b ff main
commit "2026-01-02T11:10:00+0000" "F1: commit di ff"
git -C "$REPO" checkout -q main
git -C "$REPO" merge -q --ff-only ff
commit "2026-01-02T11:20:00+0000" "D: tip di main"

# piu' riferimenti sullo stesso commit (D)
git -C "$REPO" tag tip-lw
GIT_COMMITTER_DATE="2026-01-02T11:50:00+0000" git -C "$REPO" tag -a v2.0 -m "versione 2.0"

# --- gruppo isolato con date come spareggio (oA, oC, oB figli di R) ---------
# R e' la radice del gruppo; oA il piu' vecchio (12:02), oC 12:04, oB 12:06.
# Serve a verificare lo spareggio per data e i messaggi che lo dichiarano.
git -C "$REPO" checkout -q --orphan oA
commit "2026-01-02T12:00:00+0000" "R: radice del gruppo isolato"
commit "2026-01-02T12:02:00+0000" "OA: primo ramo isolato"
git -C "$REPO" checkout -q -b oC HEAD~1
commit "2026-01-02T12:04:00+0000" "OC: secondo ramo isolato"
git -C "$REPO" checkout -q -b oB HEAD~1
commit "2026-01-02T12:06:00+0000" "OB: terzo ramo isolato"
git -C "$REPO" checkout -q main

# --- genealogia: ramo nato dal secondo genitore di un merge -----------------
# sp parte da $M1^2, cioe' dal secondo genitore del merge (B3).
git -C "$REPO" checkout -q -b sp "$M1^2"
commit "2026-01-02T12:20:00+0000" "SP1: nato dal secondo genitore del merge"
commit "2026-01-02T12:22:00+0000" "SP2: altro lavoro su sp"

# --- genealogia: merge ripetuti dello stesso branch in main -----------------
git -C "$REPO" checkout -q main
git -C "$REPO" checkout -q -b rm base-c
commit "2026-01-02T12:30:00+0000" "RM1: lavoro su rm"
git -C "$REPO" checkout -q main
GIT_AUTHOR_DATE="2026-01-02T12:40:00+0000" GIT_COMMITTER_DATE="2026-01-02T12:40:00+0000" \
    git -C "$REPO" merge -q --no-ff rm -m "main: primo merge di rm"
git -C "$REPO" checkout -q rm
commit "2026-01-02T12:50:00+0000" "RM2: altro lavoro su rm"
git -C "$REPO" checkout -q main
GIT_AUTHOR_DATE="2026-01-02T13:00:00+0000" GIT_COMMITTER_DATE="2026-01-02T13:00:00+0000" \
    git -C "$REPO" merge -q --no-ff rm -m "main: secondo merge di rm"

# --- genealogia: storia ribasata ---------------------------------------------
# rb nasce su B, il commit viene riapplicato su C: gli hash cambiano e il fork
# deve seguire la storia (C). rb-old conserva il commit originale.
git -C "$REPO" checkout -q -b rb base-b
commit "2026-01-02T13:10:00+0000" "RB1: lavoro che verra' ribasato"
git -C "$REPO" branch rb-old
git -C "$REPO" rebase -q base-c

# --- genealogia non determinabile: ramo senza antenati condivisi --------------
git -C "$REPO" checkout -q main
git -C "$REPO" checkout -q --orphan orf
commit "2026-01-02T14:00:00+0000" "ORF1: radice senza antenati"
git -C "$REPO" checkout -q main

# --- stash: non deve comparire nel grafo ------------------------------------
echo "lavoro in corso" > "$REPO/wip.txt"
git -C "$REPO" add wip.txt
git -C "$REPO" stash push -q -m "lavoro in corso"

# --- remote-tracking: stessi nomi canonici, punte diverse --------------------
# main punta a D; origin/main e' indietro di un commit (F1); upstream/main e'
# indietro di tre (M1). Stesso nome canonico di main, punte diverse: nessuno
# specchio, nessuna relazione certa dai soli nomi.
git -C "$REPO" update-ref refs/remotes/origin/main main~1
git -C "$REPO" update-ref refs/remotes/upstream/main main~3
# --- specchio provato da Git: stesso nome canonico e stesso commit -----------
git -C "$REPO" update-ref refs/remotes/origin/branch1 branch1
# --- alias: nome diverso, stessa punta di feature/x --------------------------
git -C "$REPO" update-ref refs/remotes/origin/feature-x feature/x

echo "OK: $REPO"
