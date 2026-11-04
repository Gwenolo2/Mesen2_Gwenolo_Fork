# =====================================================================
#  _push_to_Git.ps1 - Commit + envoi vers GitHub en un clic
#  Lance par _push_to_Git.bat (double-clic). Aucune commande a taper.
#
#  Securites :
#   - n'envoie jamais de fichier > 95 Mo (limite GitHub : 100 Mo) ;
#   - repare tout seul des commits locaux bloques par un gros fichier ;
#   - respecte le .gitignore (zip, 7z, _backup_claude...) ;
#   - pas de "push --force" : rien n'est jamais ecrase sur GitHub ;
#   - en cas de conflit, annule proprement et explique quoi faire.
# =====================================================================

$Branch  = 'main'
$Remote  = 'origin'
$LimitMB = 95

Set-Location -LiteralPath $PSScriptRoot
$Host.UI.RawUI.WindowTitle = "Commit + Push vers GitHub ($Branch)"

function Step($t) { Write-Host ""; Write-Host ">> $t" -ForegroundColor Cyan }
function Info($t) { Write-Host "   $t" }
function Warn($t) { Write-Host "   $t" -ForegroundColor Yellow }
function Finish($ok, $t) {
    Write-Host ""
    Write-Host "========================================================"
    if ($ok) { Write-Host "  $t" -ForegroundColor Green } else { Write-Host "  $t" -ForegroundColor Red }
    Write-Host "========================================================"
    Write-Host ""
    Read-Host "Appuyez sur Entree pour fermer" | Out-Null
    if ($ok) { exit 0 } else { exit 1 }
}
function G {
    # Transmet aussi l'entree du pipeline a git (necessaire pour "... | G cat-file")
    if ($MyInvocation.ExpectingInput) { $input | & git -c core.quotepath=false @args }
    else { & git -c core.quotepath=false @args }
}

Write-Host "========================================================"
Write-Host "  ENVOI DES MODIFICATIONS VERS GITHUB ($Branch)"
Write-Host "========================================================"

# --- 0. Verifications de base ---------------------------------------
if (-not (Test-Path -LiteralPath '.git')) { Finish $false "Ce dossier n'est pas un depot Git." }
if (-not (G config user.name)) { G config user.name "Gwenolo2" | Out-Null }

# Une operation precedente interrompue ? On la nettoie.
if ((Test-Path '.git/rebase-merge') -or (Test-Path '.git/rebase-apply')) {
    Warn "Une synchronisation precedente etait restee en cours : annulation."
    G rebase --abort 2>$null | Out-Null
}
if (Test-Path '.git/index.lock') {
    Finish $false "Git est occupe par un autre programme (Visual Studio ?). Attendez quelques secondes et relancez. Si le message persiste, fermez Visual Studio puis relancez."
}

# --- 1. Etat de GitHub -----------------------------------------------
Step "1/6 Lecture de l'etat de GitHub..."
G fetch $Remote 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Finish $false "Impossible de joindre GitHub (connexion internet ? identifiants ?)." }
$remoteRef = "$Remote/$Branch"

# --- 2. Reparation des commits locaux trop gros -----------------------
Step "2/6 Verification des commits pas encore envoyes..."
$ahead = @(G rev-list "$remoteRef..HEAD")
if ($ahead.Count -gt 0) {
    $big = @(G rev-list --objects "$remoteRef..HEAD" |
             G cat-file '--batch-check=%(objecttype) %(objectsize) %(rest)' |
             Where-Object { $_ -match '^blob (\d+) (.+)$' -and [long]$Matches[1] -gt ($LimitMB * 1MB) } |
             ForEach-Object { ($_ -split ' ', 3)[2] } | Sort-Object -Unique)
    if ($big.Count -gt 0) {
        Warn "Des commits locaux contiennent des fichiers trop gros pour GitHub :"
        $big | ForEach-Object { Warn "   - $_" }
        Warn "Reparation : ces commits sont refaits sans ces fichiers (vos fichiers sur le disque ne sont PAS touches)."
        G reset --soft $remoteRef | Out-Null
        foreach ($f in $big) { G rm --cached -q --ignore-unmatch -- $f | Out-Null }
        # Retire aussi les fichiers ajoutes depuis GitHub qui sont maintenant ignores (.gitignore)
        $added = @(G diff --cached --name-only --diff-filter=A $remoteRef)
        foreach ($f in $added) {
            G check-ignore -q --no-index -- $f
            if ($LASTEXITCODE -eq 0) { G rm --cached -q -- $f | Out-Null; Info "   ignore (.gitignore) : $f" }
        }
    } else {
        Info "$($ahead.Count) commit(s) local(aux) en attente d'envoi : OK."
    }
} else {
    Info "Rien en attente."
}

# --- 3. Ajout des modifications ---------------------------------------
Step "3/6 Ajout des fichiers modifies..."
G add -A 2>&1 | Out-Null
$staged = @(G diff --cached --name-only --diff-filter=AM)
foreach ($f in $staged) {
    if ((Test-Path -LiteralPath $f) -and ((Get-Item -LiteralPath $f).Length -gt ($LimitMB * 1MB))) {
        G reset -q -- $f | Out-Null
        Warn "Non envoye (plus de $LimitMB Mo, refuse par GitHub) : $f"
    }
}

$changes = @(G diff --cached --name-status)
if ($changes.Count -gt 0) {
    Info "Fichiers qui seront enregistres :"
    $changes | Select-Object -First 40 | ForEach-Object { Info "   $_" }
    if ($changes.Count -gt 40) { Info "   ... et $($changes.Count - 40) autre(s)" }
}

# --- 4. Commit ---------------------------------------------------------
Step "4/6 Enregistrement (commit)..."
if ($changes.Count -gt 0) {
    $msg = Read-Host "Message de commit (Entree = message automatique)"
    if ([string]::IsNullOrWhiteSpace($msg)) { $msg = "Mise a jour du " + (Get-Date -Format 'yyyy-MM-dd HH:mm') }
    G commit -q -m $msg
    if ($LASTEXITCODE -ne 0) { Finish $false "Le commit a echoue." }
    Info "Commit cree : $msg"
} else {
    Info "Aucune modification a enregistrer."
}

$ahead = @(G rev-list "$remoteRef..HEAD")
if ($ahead.Count -eq 0) { Finish $true "Rien a envoyer : GitHub est deja a jour." }

# --- 5. Synchronisation avec GitHub -------------------------------------
Step "5/6 Synchronisation avec GitHub..."
G pull --rebase -q $Remote $Branch 2>&1 | ForEach-Object { Info "$_" }
if ($LASTEXITCODE -ne 0) {
    G rebase --abort 2>$null | Out-Null
    Finish $false "Conflit : GitHub contient des modifications qui touchent les memes lignes. Rien n'a ete perdu ni envoye. Demandez de l'aide a Claude avant de relancer."
}

# --- 6. Envoi ------------------------------------------------------------
Step "6/6 Envoi vers GitHub..."
G push $Remote $Branch 2>&1 | ForEach-Object { Info "$_" }
if ($LASTEXITCODE -ne 0) { Finish $false "L'envoi a echoue (voir les messages ci-dessus). Vos commits sont conserves localement." }

Finish $true "SUCCES : tout a ete envoye sur GitHub."
