# Manuale DaikinWemos 2.12

## DAIKIN WEMOS - VERSIONE AUTONOMA PER ARDUINO IDE

Aprire DaikinWemos.ino mantenendolo nella cartella DaikinWemos.
Questa versione include la pagina web nello sketch. Usa solo librerie del core ESP8266 e prepara la memoria dei dati dal pannello Apprendimento.
E' una versione separata e semplificata rispetto al precedente progetto
PlatformIO: niente MQTT. Turbo (Powerful) ed Eco (Econo) sono disponibili
quando l'unita risponde ai comandi S21 F6/D6 e F7/D7. I pulsanti mostrano
lo stato riletto dallo split e sono disabilitati se manca una lettura valida.

## CONFIGURAZIONE ARDUINO IDE
1. File > Preferenze > URL aggiuntive per il Gestore schede:
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   Se ci sono altre URL, mantenerle e aggiungere questa.
2. Gestore schede: installare esp8266 by ESP8266 Community, versione 3.1.2.
3. Strumenti > Scheda: LOLIN(WEMOS) D1 R2 & mini.
4. Porta: selezionare quella del proprio Wemos (USB-SERIAL CH340).
5. Upload Speed: 115200; CPU: 80 MHz; Flash Size: 4 MB
   (FS: 1 MB, OTA disponibile; mantenere questo layout negli aggiornamenti).
6. Premere Verifica, poi Carica con soltanto la USB collegata.

## MODALITA' FLASH
Se Arduino IDE si ferma su Connecting:
- scollegare la USB;
- collegare D3 (GPIO0) a GND;
- ricollegare la USB e riprovare Carica.
Al termine scollegare la USB, rimuovere il ponticello D3-GND e ricollegare.
Se resta bloccato, comunicare il log completo: il file .ino non risolve
da solo un problema di boot o cablaggio della scheda.

## PRIMO AVVIO
- Collegarsi alla rete WiFi-daikin-XXXXXX (suffisso diverso per ogni Wemos).
- Password: daikinsetup
- Aprire http://192.168.4.1 e inserire il Wi-Fi domestico a 2,4 GHz.
- Dopo il riavvio ricollegarsi alla rete domestica e aprire
  http://daikin-wemos.local oppure l'IP assegnato dal router.
- L'indirizzo IP compare anche nel monitor seriale a 115200 baud.
- Con credenziali errate o rete assente il portale torna dopo 25 secondi.
- In DHCP il portale si spegne quando il Wemos si collega alla rete domestica.

## DHCP E IP STATICO
In Configurazione Wi-Fi selezionare Automatico (DHCP) o IP statico.
Per l'IP statico indicare IPv4, maschera e gateway. Il DNS e' facoltativo
e, se lasciato vuoto, viene impostato sul gateway. Usare un IP libero
della propria sottorete, fuori dal pool DHCP o riservato al Wemos sul router.
Per mantenere la rete e la password gia' salvate, lasciare invariato il nome
della rete e vuota la password. La casella Rete senza password cancella
la password salvata per la rete scelta.
Dopo il salvataggio il Wemos si riavvia. In modalita' statica aprire il nuovo IP.
Se l'IP non e' raggiungibile, riavviare il Wemos e collegarsi entro due minuti
alla rete WiFi-daikin-XXXXXX per correggere i dati o tornare a DHCP.
Il portale risponde su http://192.168.4.1, oppure http://172.31.255.1 se
la sottorete impostata comprende 192.168.4.1. In assenza di collegamento
alla rete domestica il portale resta disponibile.
- Il firmware legge lo stato all'avvio e non invia comandi di accensione.
- Senza collegamento S21 mostra Nessuna risposta recente dal Daikin:
  e' normale durante la prova con la sola USB.

## AGGIORNAMENTO FIRMWARE DAL BROWSER
Se e' gia' presente la 1.3 con aggiornamento web, caricare il .bin dal browser.
Con versioni precedenti, installare lo sketch 2.12 via USB con Arduino IDE.
Al riavvio aprire il Monitor seriale a 115200 baud e leggere la riga
'Chiave aggiornamento firmware'. La chiave e' specifica del Wemos e resta
in memoria insieme alle impostazioni di rete. Se non compare, premere RST.
Gli aggiornamenti successivi si fanno dal pannello Aggiornamento firmware.
Selezionare il file .bin per ESP8266 D1 mini, inserire la chiave e premere
Carica firmware e riavvia. Lasciare alimentato il dispositivo fino al riavvio.
Per compilare un proprio .bin usare Sketch > Esporta binario compilato
in Arduino IDE, con la stessa scheda e lo stesso layout della flash.
Usare il pannello sulla rete locale. Gli aggiornamenti vanno eseguiti sulla rete locale.

Il menu a scomparsa contiene Home, Programmazione Orari, Apprendimento, Preferenze, Firmware e Debug.
La configurazione Wi-Fi e' dentro Preferenze.

## TIMER DI SPEGNIMENTO

Nella Home scegliere 15 minuti, 30 minuti, 1 ora o 2 ore, oppure Durata
personalizzata per inserire ore e minuti (da un minuto a 24 ore).
Premere Imposta spegnimento: la sola scelta della durata non avvia il timer.
Il timer attivo mostra il tempo rimanente e l'ora prevista di spegnimento,
calcolata con l'orologio del browser. Aggiorna spegnimento sostituisce il
timer con la nuova durata; Annulla spegnimento lo cancella.
Il conteggio prosegue anche chiudendo il browser. Il riavvio del Wemos e
lo spegnimento dello split annullano il timer. Alla scadenza il Wemos invia
OFF e verifica la risposta, ritentando per un massimo di un minuto.
Il timer funziona anche senza sincronizzazione NTP.

## NOME DEL DISPOSITIVO

In Preferenze inserire un nome, per esempio Clima soggiorno, e premere
Salva nome. Il nome compare nell'intestazione e nel titolo della pagina,
e resta salvato anche dopo il riavvio. Il campo ammette fino a 32 caratteri,
con un limite di 48 byte UTF-8 per i caratteri accentati o i simboli.
Il nome non cambia l'indirizzo IP, il nome mDNS o la rete di configurazione.

## PROGRAMMAZIONE SETTIMANALE
Aprire la scheda Orari e aggiungere fino a otto orari.
Per ogni regola scegliere giorni, ora e azione. Le azioni di accensione richiedono una temperatura. Spegni usa solo giorno e ora.
Premere Salva programmazione. Le regole sono salvate nel Wemos.
L'orologio si sincronizza via NTP (fuso Roma, cambio ora automatico).
Controllare l'orologio mostrato nel pannello dopo l'aggiornamento.
La regola parte nel minuto impostato. Se il Wemos è spento o non ha
ancora sincronizzato l'orologio, l'orario passato non viene recuperato.
In Riscaldamento si applica la compensazione salvata. La ventola conserva
la modalità riletta dal Daikin. Se lo split è già acceso, la regola
applica la modalità e la temperatura previste.
Esempio: lun-ven 07:00 Riscaldamento a 20 °C, lun-ven 22:30 Spegni.
La compilazione e il salvataggio dal browser con dati simulati sono
verificati. Accensione e spegnimento programmati restano da provare sul Daikin.

APPLICAZIONE AUTOMATICA - v2.10
Il pulsante Applica impostazioni e' stato rimosso.
Modalita, alimentazione e ventola si applicano subito dopo la selezione.
In Preferenze scegliere il ritardo temperatura da 0 a 60 secondi:
0 applica subito, il valore iniziale e' 3 secondi. Il ritardo viene salvato
nel browser. Ogni modifica della temperatura riavvia il conteggio.
Le modifiche effettuate durante un altro invio vengono applicate al termine.
La sola modifica della temperatura conserva la regolazione della ventola.

## PREFERENZE
Il tema Automatico usa colori per modalità e temperatura. Sono disponibili
anche Chiaro e Scuro. La scelta del tema è salvata nel browser.
La compensazione di riscaldamento va da 0 a -5 °C a passi di 0,5 °C.
0 la disattiva. È salvata nel Wemos e si applica alla successiva modifica
dei comandi dalla pagina in Riscaldamento. Per esempio, 20 °C richiesti con
-2,5 °C di compensazione inviano 17,5 °C. La pagina mostra anche
l'impostazione confermata dal Daikin e il suo obiettivo interno, se disponibile.
Dopo aver cambiato la compensazione, scegli nuovamente la temperatura
desiderata per applicarla automaticamente.
Le chiamate a /api/set senza compensate=1 usano il valore S21 diretto.

## DEBUG REMOTO
Dalla pagina del Wemos aprire Debug remoto, inserire la chiave
di aggiornamento e premere Attiva debug. La chiave serve solo
per attivare o disattivare la sessione. Durante la sessione i dispositivi
che raggiungono il Wemos in rete locale possono leggere i log e fare sonde.
Il debug resta attivo fino a Disattiva, anche dopo riavvio.

GET /api/state mostra la versione e debugActive, true o false.
GET /api/debug/log mostra gli ultimi 12 scambi S21, i tempi, gli errori,
il segnale Wi-Fi, il tempo di attivita' e la RAM disponibile.
POST /api/debug/probe accetta command=F1, F5, F6, F7, RG, RH, RI, RL,
RX, Ra, Rd o Rg. Impostare l'header X-Debug-Request: 1.
Esempio (modificare l'IP se necessario):
curl.exe -H "X-Debug-Request: 1" -d "command=F1" http://daikin-wemos.local/api/debug/probe
Le sonde leggono lo stato S21. Lo spegnimento, la temperatura e le
altre impostazioni si comandano dalle normali funzioni del pannello.
I test automatici usano risposte simulate; verificare le sonde S21 sul proprio dispositivo.

CABLAGGIO S21 (verificare numerazione e tensioni sulla propria scheda)
S21 pin 2 TX -> CYT1076 HV1 / LV1 -> Wemos D6 (GPIO12 RX)
S21 pin 3 RX <- CYT1076 HV2 / LV2 <- Wemos D7 (GPIO13 TX)
S21 pin 4 alimentazione -> IN+ step-down
S21 pin 5 GND -> massa comune
Step-down OUT+ regolato a 5 V -> Wemos 5V e CYT1076 HV
Wemos 3V3 -> CYT1076 LV
GND comune: S21, step-down IN-/OUT-, Wemos, CYT1076
S21 pin 1 lasciato scollegato.
Non alimentare contemporaneamente da USB e step-down per queste prove.
Eseguire i collegamenti con il climatizzatore disalimentato.

## VALIDAZIONE
Compilazione riuscita con core ESP8266 3.1.2, target d1_mini.
RAM: circa 39 kB; firmware: circa 464 kB.
Le verifiche automatiche non sostituiscono i test sul proprio climatizzatore.
Sono implementati S21 F1/D1, F5/D5, RH, Ra, RG, RL, F6/D6 e F7/D7. Funzioni e sensori disponibili
dipendono dal modello. Un ACK indica ricezione: la pagina rilegge lo stato.

## RIFERIMENTI
https://github.com/MassiPi/DaikinS21
https://github.com/revk/xESP32-Faikout/wiki/S21-Protocol
Licenza dello sketch: GPL-3.0-or-later.

## APPRENDIMENTO PREDITTIVO
La v2.5 include una rete di 12 ingressi e 8 neuroni nascosti per previsioni
a dieci minuti in osservazione. Il modello deve essere addestrato sul PC.
Il pacchetto Apprendimento contiene raccolta CSV, addestramento e istruzioni.
La nuova scheda mostra la previsione dopo il caricamento di un modello valido.
Le previsioni non modificano i comandi al climatizzatore.

## ARCHIVIO SUL WEMOS
La v2.8 registra nella flash dalla scheda Apprendimento. Se richiesto,
inizializzare la memoria dei file con la chiave. Poi e' possibile spegnere
il PC e scaricare il CSV in seguito. La registrazione e' ogni 30 secondi,
con salvataggio ogni cinque minuti e archivio circolare da 512 KiB.
I dati piu vecchi vengono sostituiti quando l'archivio si riempie.
Le istruzioni complete sono in Apprendimento/LEGGIMI.txt.

GRAFICO SUL WEMOS - v2.8
Nella scheda Apprendimento il grafico legge lo storico direttamente dal Wemos.
Periodi selezionabili: 1 ora, 6 ore, 24 ore o tutto lo storico.
Serie: ambiente, esterno, temperatura impostata e previsione a dieci minuti.
Premere Aggiorna grafico per rileggere i campioni. Non richiede Internet.
I vecchi CSV sono compatibili; la nuova colonna prediction10m viene registrata
solo da questa versione e contiene valori solo con previsioni disponibili.
Le previsioni sono disegnate dieci minuti dopo il campione di origine.

PIN TX / RX CONFIGURABILI - v2.9
Preferenze > Collegamento Daikin: scegliere separatamente TX e RX.
Pin disponibili: D1/GPIO5, D2/GPIO4, D5/GPIO14, D6/GPIO12, D7/GPIO13.
Predefiniti: TX D7, RX D6. Sono richiesti due pin diversi.
La scelta viene salvata nella EEPROM senza cambiare Wi-Fi, orari o modello
e applicata subito senza riavvio. Per modificare il cablaggio togliere
alimentazione. TX del Wemos va al RX Daikin, RX Wemos al TX Daikin,
sempre attraverso il convertitore di livello.

Le assegnazioni personalizzate gia salvate rimangono valide dopo l'aggiornamento.

INTERFACCIA SMARTPHONE - v2.11
Le sei sezioni sono nascoste nel menu laterale, aperto dal pulsante in alto.
Scegliere una sezione per aprirla e chiudere il menu. Si puo chiudere anche
con la X, toccando lo sfondo oppure premendo Escape sulla tastiera.
La barra mostra il nome della sezione corrente. I comandi hanno aree
toccabili di almeno 44 pixel; modalita, pin e orari si dispongono in colonne
adatte agli schermi stretti. Interfaccia verificata con dati simulati a
320, 390, 430 e 844 pixel, inclusi i controlli di applicazione automatica.
