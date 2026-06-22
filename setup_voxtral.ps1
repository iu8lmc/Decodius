# ============================================================================
#  Decodius - Attivazione voce con VOXTRAL (Mistral) via transformers
#  STT locale piu' accurato di Whisper. Scarica un Python 3.12 portatile con
#  PyTorch (CUDA) + transformers + bitsandbytes + il modello Voxtral-Mini-3B
#  in <cartella app>\voxtral. Il modello gira in 4-bit sulla GPU (~3.6 GB VRAM),
#  resta caldo, trascrive in ~1.5-2 s. ~13 GB di download la prima volta.
#  RICHIEDE una GPU NVIDIA (CUDA). Imposta il cervello su glm-4.7:cloud (primario, serve
#  'ollama signin') con riserva LOCALE qwen3:4b: se il cloud finisce i crediti / non e'
#  loggato, Decodius passa da solo a qwen3:4b (convive con Voxtral negli 8 GB).
#  Per tornare a Whisper: decodius_stt.txt = whisper.
# ============================================================================
$ErrorActionPreference = "Stop"

# --- Auto-elevazione ---------------------------------------------------------
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    exit
}

function Info($m){ Write-Host "  $m" -ForegroundColor Cyan }
function Ok($m){   Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn($m){ Write-Host "  [!] $m" -ForegroundColor Yellow }
function Err($m){  Write-Host "  [X] $m" -ForegroundColor Red }

Write-Host ""
Write-Host "  =================================================" -ForegroundColor White
Write-Host "   DECODIUS - Voce con VOXTRAL (Mistral, locale GPU)" -ForegroundColor White
Write-Host "  =================================================" -ForegroundColor White
Write-Host ""

$base = Join-Path $PSScriptRoot "voxtral"
$py   = Join-Path $base "py"
$REPO = "mistralai/Voxtral-Mini-3B-2507"

# GPU NVIDIA presente?
$hasNv = $false
try { nvidia-smi | Out-Null; if ($LASTEXITCODE -eq 0) { $hasNv = $true } } catch {}
if (-not $hasNv) {
    Warn "Non rilevo una GPU NVIDIA (CUDA). Voxtral senza GPU sarebbe lentissimo."
    $go = Read-Host "  Continuo lo stesso? (s/N)"
    if ($go -notmatch '^[sS]') { exit 1 }
}

# Gia' installato?
if ((Test-Path "$py\python.exe") -and (Test-Path "$base\voxtral_server.py")) {
    Ok "Voxtral risulta gia' installato in: $base"
    $ans = Read-Host "  Reinstallare da zero? (s/N)"
    if ($ans -notmatch '^[sS]') {
        Set-Content (Join-Path $PSScriptRoot "decodius_stt.txt") "voxtral" -Encoding ascii -NoNewline
        # cervello: primario CLOUD + riserva LOCALE (fallback automatico) su due righe
        Set-Content (Join-Path $PSScriptRoot "decodius_model.txt") "glm-4.7:cloud`r`nqwen3:4b" -Encoding ascii -NoNewline
        Ok "Engine STT = voxtral, cervello = glm-4.7:cloud (riserva qwen3:4b). Riavvia Decodius."
        Read-Host "  Premi INVIO per chiudere"; exit 0
    }
    Remove-Item $base -Recurse -Force
}

Info "Preparo Voxtral (PyTorch + modello). ~13 GB di download: ci vorra' un po'."
Write-Host ""

try {
    New-Item -ItemType Directory -Force -Path $base, $py | Out-Null
    $env:HF_HUB_DISABLE_SYMLINKS_WARNING = "1"

    # 1) Python 3.12 embeddable
    Info "1/6  Scarico Python 3.12 portatile..."
    $zip = "$env:TEMP\py312_voxtral.zip"
    Invoke-WebRequest "https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive $zip $py -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path "$py\python.exe")) { throw "Python portatile non estratto." }
    $pth = Get-ChildItem "$py\python*._pth" | Select-Object -First 1
    (Get-Content $pth.FullName) -replace '#\s*import site', 'import site' | Set-Content $pth.FullName

    Info "2/6  Installo pip..."
    Invoke-WebRequest "https://bootstrap.pypa.io/get-pip.py" -OutFile "$py\get-pip.py" -UseBasicParsing
    & "$py\python.exe" "$py\get-pip.py" --no-warn-script-location 2>&1 | Select-Object -Last 1

    # 3) PyTorch CUDA (wheel grande ~2.5 GB)
    Info "3/6  Installo PyTorch (CUDA 12.4)..."
    & "$py\python.exe" -m pip install --no-warn-script-location torch --index-url https://download.pytorch.org/whl/cu124 2>&1 | Select-Object -Last 2

    # 4) transformers + Voxtral + audio + microfono
    Info "4/6  Installo transformers + bitsandbytes + librosa + mic..."
    & "$py\python.exe" -m pip install --no-warn-script-location "transformers>=4.56" accelerate bitsandbytes "mistral-common[audio]" librosa soundfile sounddevice numpy huggingface_hub 2>&1 | Select-Object -Last 2
    & "$py\python.exe" -c "import torch,transformers,bitsandbytes,librosa,sounddevice,mistral_common; assert torch.cuda.is_available(), 'CUDA non disponibile in torch'; print('import OK, CUDA', torch.cuda.is_available())"
    if ($LASTEXITCODE -ne 0) { throw "Verifica librerie/CUDA non riuscita." }

    # 5) Modello Voxtral (HF, non gated: niente account) -> cache HF di default
    Info "5/6  Scarico il modello Voxtral-Mini-3B (~9 GB)..."
    & "$py\python.exe" -c "from huggingface_hub import snapshot_download; snapshot_download('$REPO')"
    if ($LASTEXITCODE -ne 0) { throw "Download del modello Voxtral non riuscito." }

    # 6) Server + attivazione
    Info "6/6  Finalizzo..."
    Copy-Item (Join-Path $PSScriptRoot "voxtral_server.py") "$base\voxtral_server.py" -Force
    & "$py\python.exe" -m pip cache purge 2>&1 | Out-Null

    Set-Content (Join-Path $PSScriptRoot "decodius_stt.txt") "voxtral" -Encoding ascii -NoNewline
    # cervello: primario CLOUD glm-4.7:cloud (serve 'ollama signin') + riserva LOCALE qwen3:4b.
    # Se il cloud finisce i crediti / non e' loggato, Decodius passa da solo a qwen3:4b.
    Set-Content (Join-Path $PSScriptRoot "decodius_model.txt") "glm-4.7:cloud`r`nqwen3:4b" -Encoding ascii -NoNewline
    # la riserva locale convive con Voxtral negli 8 GB: scarico qwen3:4b se Ollama c'e'
    try { ollama pull qwen3:4b 2>&1 | Out-Null } catch { Warn "Ollama non trovato: scarica qwen3:4b a parte." }
}
catch {
    Err "Attivazione non riuscita: $($_.Exception.Message)"
    Err "Controlla connessione/GPU e riprova."
    Read-Host "`n  Premi INVIO per chiudere"; exit 1
}

$gb = [math]::Round((Get-ChildItem $base -Recurse -File | Measure-Object Length -Sum).Sum/1GB, 1)
Write-Host ""
Write-Host "  =================================================" -ForegroundColor Green
Write-Host "   FATTO! Voce con Voxtral attiva ($gb GB)." -ForegroundColor Green
Write-Host "   STT = voxtral, cervello = glm-4.7:cloud (riserva qwen3:4b). Riavvia Decodius e usa il microfono." -ForegroundColor Green
Write-Host "   (Per tornare a Whisper: scrivi 'whisper' in decodius_stt.txt)" -ForegroundColor Green
Write-Host "  =================================================" -ForegroundColor Green
Write-Host ""
Read-Host "  Premi INVIO per chiudere"
