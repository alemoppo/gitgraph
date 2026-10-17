# check_no_reparse.ps1 - verifica che un percorso non sia un collegamento del
# filesystem (giunzione o collegamento simbolico), e con -IncludeAncestors anche
# tutti i suoi componenti superiori.
#
# Uso (da make_test_repo.bat):
#   powershell -NoProfile -ExecutionPolicy Bypass -File check_no_reparse.ps1 -Path <percorso> [-IncludeAncestors]
#
# Perche' serve: il confronto testuale dei percorsi (%~f) normalizza "/" e "\"
# ma NON risolve i collegamenti, quindi una radice che e' una giunzione verso
# una directory esterna passerebbe il confronto con il padre della
# destinazione, e la cancellazione ricorsiva raggiungerebbe contenuti esterni.
# Gli antenati vengono controllati solo per la radice autorizzata: per la
# destinazione gli antenati sono la radice stessa, gia' verificata.
#
# Codici di uscita:
#   0  nessun reparse point (nel percorso, e nei suoi antenati se richiesto)
#   1  non ispezionabile (percorso inesistente, permessi, errore): in caso di
#      dubbio il chiamante deve rifiutare la cancellazione
#   2  trovato un reparse point; ne viene indicato il percorso
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [switch]$IncludeAncestors
)

try {
    $reparse = [System.IO.FileAttributes]::ReparsePoint
    $info = [System.IO.DirectoryInfo]::new($Path)
    if (-not $info.Exists) {
        Write-Output "non ispezionabile (inesistente): $Path"
        exit 1
    }
    while ($null -ne $info) {
        if (($info.Attributes -band $reparse) -ne 0) {
            Write-Output "collegamento: $($info.FullName)"
            exit 2
        }
        if (-not $IncludeAncestors) {
            break
        }
        $info = $info.Parent
    }
} catch {
    Write-Output "non ispezionabile: $($_.Exception.Message)"
    exit 1
}
exit 0