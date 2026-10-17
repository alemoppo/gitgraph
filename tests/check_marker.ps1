# check_marker.ps1 - validazione esatta del marcatore di un repository di test.
#
# Uso (da make_test_repo.bat):
#   powershell -NoProfile -ExecutionPolicy Bypass -File check_marker.ps1 <file> <testo atteso>
#
# Il contenuto ammesso e' esattamente il testo atteso, da solo o seguito dal
# solo terminatore di riga (LF oppure CRLF): per "gitgraph test repository" sono
# 24, 25 o 26 byte. Qualsiasi altro byte viene rifiutato: righe in piu', testo
# prima o dopo il marcatore, terminatori misti.
#
# Il confronto e' fatto sui byte, non sulla prima riga: leggere solo la prima
# riga (per esempio con "set /p") accetterebbe contenuti aggiuntivi che non
# cambiano quella riga.
#
# Codici di uscita:
#   0  contenuto ammesso
#   1  file mancante o non leggibile
#   2  contenuto diverso da quello ammesso
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Expected
)

try {
    if (-not [System.IO.File]::Exists($Path)) {
        Write-Output "marcatore mancante: $Path"
        exit 1
    }
    # nomi distinti da quelli dei parametri: in PowerShell le variabili non
    # distinguono maiuscole/minuscole, quindi $expected sovrascriverebbe il
    # parametro [string] $Expected convertingolo in testo
    $want = [System.Text.Encoding]::ASCII.GetBytes($Expected)
    $wantLf = [byte[]]($want + [byte]0x0A)
    $wantCrlf = [byte[]]($want + [byte]0x0D + [byte]0x0A)
    $allowed = @($want, $wantLf, $wantCrlf)

    $actual = [System.IO.File]::ReadAllBytes($Path)
    foreach ($candidate in $allowed) {
        if ($candidate.Length -ne $actual.Length) {
            continue
        }
        $same = $true
        for ($i = 0; $i -lt $candidate.Length; $i++) {
            if ($candidate[$i] -ne $actual[$i]) {
                $same = $false
                break
            }
        }
        if ($same) {
            exit 0
        }
    }
    Write-Output "contenuto inatteso: $Path ($($actual.Length) byte, attesi $($want.Length), $($wantLf.Length) o $($wantCrlf.Length))"
    exit 2
} catch {
    Write-Output "non leggibile: $($_.Exception.Message)"
    exit 1
}
exit 1