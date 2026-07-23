# ============================================================================
#  Decodius - Setup automatico del "cervello" (Ollama + qwen3:1.7b, LOCALE)
#  Installa Ollama se manca, avvia il servizio e scarica il modello locale
#  qwen3:1.7b: gira sul tuo PC, NESSUN account richiesto, funziona offline.
# ============================================================================
$ErrorActionPreference = "Stop"
$MODEL = "qwen3:1.7b"   # default: leggero, gira ovunque

function Info($m){ Write-Host "  $m" -ForegroundColor Cyan }
function Ok($m){   Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn($m){ Write-Host "  [!] $m" -ForegroundColor Yellow }
function Err($m){  Write-Host "  [X] $m" -ForegroundColor Red }

Write-Host ""
Write-Host "  ============================================" -ForegroundColor White
Write-Host "   DECODIUS - Configurazione cervello (Ollama)" -ForegroundColor White
Write-Host "  ============================================" -ForegroundColor White
Write-Host ""

# --- 0. Scelta del cervello: base (leggero) o potenziato (piu' sveglio) -----
# Il potenziato e' la Unsloth Dynamic UD-Q4_K_XL di Qwen3-4B: qualita' quasi da
# modello pieno a parita' di spazio, ma serve piu' RAM/GPU ed e' un po' piu' lento.
Write-Host "  Quale cervello vuoi installare?" -ForegroundColor White
Write-Host "    [1] Base      - Qwen3 1.7b           (~1.4 GB, veloce, gira su PC modesti)  [predefinito]"
Write-Host "    [2] Potenziato- Qwen3 4B Dynamic XL  (~2.5 GB, piu' sveglio, serve piu' RAM/GPU)"
$scelta = Read-Host "  Premi 1 o 2 (INVIO = 1)"
if ($scelta -eq "2") {
    $MODEL = "hf.co/unsloth/Qwen3-4B-GGUF:UD-Q4_K_XL"
    Info "Cervello scelto: POTENZIATO (Qwen3 4B Dynamic UD-Q4_K_XL)."
} else {
    Info "Cervello scelto: BASE (Qwen3 1.7b)."
}
Write-Host ""

# --- 0b. KV cache quantizzata (q8_0) + flash attention ----------------------
# Variabili del SERVER Ollama, persistenti (user-scope): dimezzano la memoria della
# KV cache con perdita trascurabile. Le imposto PRIMA di avviare Ollama qui sotto, cosi'
# il server appena avviato le eredita; se Ollama era gia' attivo valgono al prossimo avvio.
[Environment]::SetEnvironmentVariable("OLLAMA_FLASH_ATTENTION", "1",    "User")
[Environment]::SetEnvironmentVariable("OLLAMA_KV_CACHE_TYPE",   "q8_0", "User")
$env:OLLAMA_FLASH_ATTENTION = "1"; $env:OLLAMA_KV_CACHE_TYPE = "q8_0"
Ok "KV cache q8_0 + flash attention attivati (meno memoria, ~nessuna perdita)."
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
# Test REALE sull'endpoint /api/chat (lo stesso che usa Decodius, con think:false):
# se il modello e' scaricato a meta'/corrotto, qui Ollama risponde con errore 500.
function Test-Brain {
    try {
        $b = @{ model=$MODEL; messages=@(@{role="user";content="ciao"}); stream=$false; think=$false; keep_alive=-1 } | ConvertTo-Json -Depth 5
        $r = Invoke-RestMethod "http://127.0.0.1:11434/api/chat" -Method Post -Body $b -TimeoutSec 120
        return [bool]$r.message.content
    } catch { $script:LastBrainErr = $_.Exception.Message; return $false }
}

Write-Host ""
Info "Scarico il modello locale: $MODEL  (gira sul tuo PC, niente cloud)"
Info "(la prima volta scarica ~1.4-2.5 GB secondo il modello; poi funziona offline)"
Write-Host ""

& $ollama pull $MODEL
if ($LASTEXITCODE -ne 0) {
    Err "Download di $MODEL non riuscito. Controlla la connessione e riprova questo setup."
    Read-Host "`n  Premi INVIO per chiudere"; exit 1
}

# Auto-riparazione: se il modello NON si carica (500 = scaricato a meta'/corrotto),
# lo rimuovo e lo riscarico pulito una volta. Cosi' "riprova il setup" ripara davvero.
Info "Verifico che il modello si carichi..."
if (-not (Test-Brain)) {
    Warn "Il modello non si carica ($script:LastBrainErr). Provo a ripararlo: rimuovo e riscarico..."
    & $ollama rm $MODEL 2>$null
    & $ollama pull $MODEL
}
Ok "Modello locale pronto: $MODEL"

# --- 4. Scrivo la configurazione di Decodius (copia UTENTE, sempre scrivibile) ---
# Decodius legge PRIMA questa copia (%APPDATA%\Decodius\Decodius), poi il default
# dell'installer in Program Files. Cosi' il setup funziona anche con l'app in Program
# Files SENZA admin: e' la correzione del caso "404 / modello inesistente" (il vecchio
# script scriveva in Program Files e falliva in silenzio, lasciando la config sbagliata).
$cfgDir    = Join-Path $env:APPDATA "Decodius\Decodius"
$modelFile = Join-Path $cfgDir "decodius_model.txt"
try {
    if (-not (Test-Path $cfgDir)) { New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null }
    # UTF-8 SENZA BOM: in PowerShell 5.1 'Set-Content -Encoding UTF8' aggiunge il BOM
    # (EF BB BF) -> Decodius leggerebbe il nome modello come "﻿qwen3..." e Ollama da 404.
    [System.IO.File]::WriteAllText($modelFile, $MODEL, (New-Object System.Text.UTF8Encoding($false)))
    Ok "Configurazione salvata: $modelFile -> $MODEL"
    # Un eventuale provider cloud salvato prima avrebbe la PRECEDENZA sul modello locale
    # e reintrodurrebbe l'errore: lo rimuovo cosi' si usa davvero il locale.
    $provFile = Join-Path $cfgDir "decodius_provider.txt"
    if (Test-Path $provFile) {
        Remove-Item $provFile -Force -ErrorAction SilentlyContinue
        Info "Rimosso il provider cloud precedente: ora Decodius usa il modello locale."
    }
} catch {
    Warn "Non ho potuto scrivere $modelFile (Decodius usera' comunque il default)."
}

# --- 5. Test finale --------------------------------------------------------
Write-Host ""
Info "Test rapido del cervello (un attimo)..."
if (Test-Brain) {
    Ok "Il cervello risponde correttamente."
} else {
    Err "Il modello e' scaricato ma NON si carica: $script:LastBrainErr"
    Err "Causa probabile: memoria RAM insufficiente o GPU non compatibile su questo PC."
    Err "Suggerimento: chiudi altri programmi pesanti e riprova; in alternativa usa un"
    Err "Provider cloud gratuito (opzione 2 nella finestra di Decodius)."
}

Write-Host ""
Write-Host "  ============================================" -ForegroundColor Green
Write-Host "   FATTO! Ora puoi avviare DECODIUS." -ForegroundColor Green
Write-Host "  ============================================" -ForegroundColor Green
Write-Host ""
Read-Host "  Premi INVIO per chiudere"
