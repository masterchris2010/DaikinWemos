# Modifiche

## 2.15

- Abilitato il comando di sola lettura Re nelle sonde debug per l'umidita S21.
- Pulsante Leggi umidita S21 nel pannello Debug, con validazione della risposta.
- Segnalato il valore fisso 50% come non confermato, per le unita senza sensore.
- La lettura diagnostica non modifica i comandi o il ciclo di polling del climatizzatore.

## 2.14

- Home ridisegnata con ghiera centrale, colori caldo/freddo, nome stanza e sensori compatti.
- Accensione rapida nell'intestazione e controlli meno frequenti in pannelli dal basso.
- Modalita, ventola, direzione aria, timer e programmazione accessibili da righe compatte.
- Prossima azione calcolata dalla programmazione e dall'orologio del Wemos.
- Pannelli chiudibili anche quando il Daikin si scollega; temi e layout smartphone verificati.
- Conservate pagina gzip, protezioni dai timeout, pin e impostazioni esistenti.

## 2.13

- Pagina web gzip da circa 17 KiB, anziché oltre 60 KiB, per ridurre il trasferimento dal Wemos.
- Disabilitata la cache della pagina e aggiunto un avviso di mancato avvio dello script.
- Limite di attesa sulle richieste, attivo anche durante la lettura del corpo della risposta.
- Impediti aggiornamenti dello stato sovrapposti e liberati i comandi dopo un timeout.
- Compatibilità senza ResizeObserver e verifica della pagina compressa tramite HTTP reale.
- Test estesi a Chromium e Firefox, incluse risposte interrotte e timer.

## 2.12

- Nome del dispositivo modificabile in Preferenze e salvato in EEPROM.
- Nome visualizzato nell'intestazione e nel titolo della pagina, con indirizzo di rete invariato.
- Timer con durate rapide 15/30 minuti, 1/2 ore e durata personalizzata in ore e minuti.
- Conto alla rovescia, ora prevista di spegnimento e annullamento nella scheda del timer attivo.
- Menu: Home, Programmazione Orari, Apprendimento, Preferenze, Firmware, Debug.
- Configurazione Wi-Fi spostata in Preferenze; il vecchio collegamento #wifi resta valido.

## 2.11

- Menu laterale a scomparsa con le sette sezioni e chiusura dopo la scelta.
- Layout per smartphone e aree toccabili di almeno 44 pixel per i controlli principali.
- Navigazione con tastiera, chiusura con Escape e gestione del focus nel menu.

## 2.10

- Rimosso Applica impostazioni; invio automatico di alimentazione, modalità e ventola.
- Ritardo configurabile per la temperatura; zero applica immediatamente.
- Le modifiche durante un invio in corso attendono la sua conclusione.

## 2.9

- TX e RX configurabili e salvati in EEPROM.
- Predefiniti definitivi: TX D7, RX D6. Mantenute le assegnazioni già salvate.

## 2.8

- Grafico sul Wemos con intervalli 1, 6, 24 ore o tutto lo storico.
- Temperature ambiente, esterna e impostata, con previsioni quando disponibili.
- Compatibilità con i CSV precedenti senza la colonna delle previsioni.

## Funzioni precedenti

Controllo S21, Wi-Fi, IP statico, aggiornamenti web, debug persistente,
orari settimanali, timer di spegnimento, temi, compensazione riscaldamento,
raccolta dati LittleFS e modello neurale in osservazione.
