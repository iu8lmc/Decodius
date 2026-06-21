# ============================================================================
#  Decodius - Setup automatico del "cervello" (Ollama + qwen3:1.7b, LOCALE)
#  Installa Ollama se manca, avvia il servizio e scarica il modello locale
#  qwen3:1.7b: gira sul tuo PC, NESSUN account richiesto, funziona offline.
# ============================================================================
$ErrorActionPreference = "Stop"
$MODEL = "qwen3:1.7b"

function Info($m){ Write-Host "  $m" -ForegroundColor Cyan }
function Ok($m){   Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn($m){ Write-Host "  [!] $m" -ForegroundColor Yellow }
function Err($m){  Write-Host "  [X] $m" -ForegroundColor Red }

Write-Host ""
Write-Host "  ============================================" -ForegroundColor White
Write-Host "   DECODIUS - Configurazione cervello (Ollama)" -ForegroundColor White
Write-Host "  ============================================" -ForegroundColor White
Write-Host ""

# --- 1. Ollama installato? -------------------------------------------------
function Find-Ollama {
    $c = Get-Command ollama -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = "$env:LOCALAPPDATA\Programs\Ollama\ollama.exe"
    if (Test-Path $p) { return $p }
    return $null
}

$ollama = Find-Ollama
if (-not $ollama) {
    Info "Ollama non trovato: lo scarico e installo (puo' richiedere qualche minuto)..."
    $setup = "$env:TEMP\OllamaSetup.exe"
    try {
        Invoke-WebRequest "https://ollama.com/download/OllamaSetup.exe" -OutFile $setup -UseBasicParsing
    } catch {
        Err "Download di Ollama fallito. Controlla la connessione e riprova,"
        Err "oppure installa manualmente da https://ollama.com/download"
        Read-Host "`n  Premi INVIO per chiudere"; exit 1
    }
    Info "Installazione di Ollama in corso..."
    Start-Process $setup -ArgumentList "/VERYSILENT","/NORESTART" -Wait
    Start-Sleep -Seconds 5
    $ollama = Find-Ollama
    if (-not $ollama) {
        Err "Installazione Ollama non riuscita. Installa manualmente da https://ollama.com/download"
        Read-Host "`n  Premi INVIO per chiudere"; exit 1
    }
    Ok "Ollama installato."
} else {
    Ok "Ollama gia' presente."
}

# --- 2. Servizio attivo? ---------------------------------------------------
function Test-OllamaUp {
    try { Invoke-RestMethod "http://127.0.0.1:11434/api/version" -TimeoutSec 3 | Out-Null; return $true }
    catch { return $false }
}
if (-not (Test-OllamaUp)) {
    Info "Avvio il servizio Ollama..."
    Start-Process $ollama -ArgumentList "serve" -WindowStyle Hidden
    $n = 0
    while (-not (Test-OllamaUp) -and $n -lt 25) { Start-Sleep -Seconds 1; $n++ }
    if (-not (Test-OllamaUp)) {
        Err "Il servizio Ollama non risponde su 127.0.0.1:11434."
        Read-Host "`n  Premi INVIO per chiudere"; exit 1
    }
}
Ok "Servizio Ollama attivo."

# --- 3. Scarico il modello LOCALE (nessun account richiesto) ----------------
Write-Host ""
Info "Scarico il modello locale: $MODEL  (gira sul tuo PC, niente cloud)"
Info "(la prima volta scarica ~1.4 GB; poi funziona offline)"
Write-Host ""

& $ollama pull $MODEL
if ($LASTEXITCODE -ne 0) {
    Err "Download di $MODEL non riuscito. Controlla la connessione e riprova questo setup."
    Read-Host "`n  Premi INVIO per chiudere"; exit 1
}
Ok "Modello locale pronto: $MODEL"

# --- 4. Scrivo la configurazione di Decodius -------------------------------
$modelFile = Join-Path $PSScriptRoot "decodius_model.txt"
try {
    Set-Content -Path $modelFile -Value $MODEL -Encoding UTF8 -NoNewline
    Ok "Configurazione salvata: $modelFile -> $MODEL"
} catch {
    Warn "Non ho potuto scrivere $modelFile (Decodius usera' comunque il default)."
}

# --- 5. Test finale --------------------------------------------------------
Write-Host ""
Info "Test rapido del cervello (un attimo)..."
try {
    $body = @{ model = $MODEL; prompt = "Rispondi solo: OK"; stream = $false } | ConvertTo-Json
    $r = Invoke-RestMethod "http://127.0.0.1:11434/api/generate" -Method Post -Body $body -TimeoutSec 60
    if ($r.response) { Ok "Il cervello risponde correttamente." }
} catch {
    Warn "Test non concludente, ma la configurazione e' a posto. Avvia Decodius e prova."
}

Write-Host ""
Write-Host "  ============================================" -ForegroundColor Green
Write-Host "   FATTO! Ora puoi avviare DECODIUS." -ForegroundColor Green
Write-Host "  ============================================" -ForegroundColor Green
Write-Host ""
Read-Host "  Premi INVIO per chiudere"
