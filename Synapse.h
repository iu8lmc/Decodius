#pragma once
#include <QString>

// ─────────────────────────────────────────────────────────────────────────────
//  Synapse — "sinapsi virtuali" locali per Decodius (zero ML, gira su Raspberry).
//
//  Recupero ASSOCIATIVO della memoria: i fatti del vault Obsidian sono NODI; le
//  SINAPSI sono le entità che condividono (nominativi, bande, modi, Paesi e i
//  [[wikilink]]). Alla domanda dell'utente attivo i nodi che combaciano e
//  l'attivazione SI PROPAGA (spreading activation, con decadimento) ai nodi
//  collegati: restituisco solo i fatti più "accesi", entro un budget di caratteri.
//  È una GraphRAG leggera e deterministica: niente embedding, solo grafo + interi.
// ─────────────────────────────────────────────────────────────────────────────

// Costruisce il grafo dai .md del vault e ritorna i fatti più attivati dalla
// domanda (uno per riga, già compattati). Stringa vuota se nulla è pertinente
// (così non si sprecano token quando la memoria non serve).
QString synapseRecall(const QString& vaultPath, const QString& query,
                      int maxFacts = 6, int maxChars = 600);

// Codec "stile Morse/Q-code": codice a lunghezza variabile a dizionario (come
// Huffman) che sostituisce le frasi ham/JSON frequenti con la loro forma corta
// che il modello già conosce (QTH, QRG, 73, FT8…), toglie i prefissi-data e
// comprime gli spazi. Riduce i token SENZA dover insegnare un dizionario al
// modello. NB: il Morse *letterale* allungherebbe il testo — qui se ne usa solo
// il principio (simboli corti per i concetti frequenti).
QString hamCompact(const QString& text);

// Stima rapida dei token (≈ parole + punteggiatura), per misurare i risparmi.
int approxTokens(const QString& text);
