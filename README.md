# DaikinWemos

Controllo locale di un climatizzatore Daikin con interfaccia S21 tramite
Wemos D1 mini (ESP8266), con pagina web integrata e senza Home Assistant.
Versione corrente: **2.13**. Progetto indipendente, non affiliato a Daikin.

Sviluppato per **FTXM25M2V1B**. La presenza di S21 e le funzioni disponibili
vanno verificate sul proprio modello: la compatibilità con altre unità non è garantita.

## Funzioni

- Interfaccia per smartphone, menu laterale, nome dispositivo personalizzabile e temi chiaro, scuro e automatico.
- Accensione, modalità, temperatura, ventola, oscillazione, Powerful ed Econo,
  quando supportati e confermati dall'unità.
- Modifiche automatiche; ritardo temperatura configurabile, senza pulsante Applica impostazioni.
- Wi-Fi con DHCP o IPv4 statico e portale di recupero.
- TX e RX configurabili e salvati sul dispositivo.
- Otto regole settimanali di accensione/spegnimento e timer di spegnimento.
- Aggiornamenti firmware dal browser con chiave specifica del dispositivo.
- Debug S21 attivabile e persistente, da disattivare quando non serve.
- Raccolta CSV in LittleFS, grafico delle temperature e previsione neurale a dieci minuti.

La rete neurale lavora **in osservazione**: non controlla il climatizzatore.
Il modello si addestra sul PC e viene esportato solo se supera i confronti
con previsioni semplici. Non è incluso un modello addestrato né lo storico di una casa.

## Hardware

- Wemos D1 mini ESP8266, flash 4 MB.
- Convertitore di livello CYT1076 fra logica 3,3 V del Wemos e lato S21.
- Alimentazione regolata adatta al cablaggio, con step-down a 5 V e masse comuni.

Assegnazione predefinita, vista dal Wemos:

| Segnale | Pin Wemos | GPIO | Collegamento attraverso il convertitore |
| --- | --- | --- | --- |
| TX | D7 | 13 | RX del Daikin |
| RX | D6 | 12 | TX del Daikin |

In Preferenze sono disponibili D1, D2, D5, D6 e D7, con TX e RX diversi.
Una scelta già salvata viene mantenuta durante gli aggiornamenti.
Verificare orientamento del connettore e tensioni: i colori dei fili non
identificano in modo affidabile i segnali. Togliere alimentazione prima
di intervenire sull'unità; per programmare via USB scollegare Daikin e step-down.
Non collegare segnali a 5 V direttamente ai GPIO dell'ESP8266.

## Compilazione con Arduino IDE

1. Aggiungere `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
   alle URL del Gestore schede e installare **esp8266 by ESP8266 Community 3.1.2**.
2. Aprire [DaikinWemos.ino](firmware/DaikinWemos/DaikinWemos.ino).
3. Scegliere **LOLIN(WEMOS) D1 R2 & mini**, CPU 80 MHz, flash **4 MB con FS 1 MB**
   e OTA disponibile; selezionare la propria porta seriale, velocità 115200.
4. Verificare e caricare con la sola USB collegata.

Le librerie sono incluse nel core ESP8266. La pagina web compressa è nello sketch:
non occorre caricare un'immagine del filesystem. Mantenere lo stesso layout
4 MB / FS 1 MB per conservare archivio e impostazioni negli aggiornamenti.

## Compilazione con PlatformIO

```sh
python -m pip install platformio==6.2.0
python -m platformio run
# Facoltativo, con la sola USB collegata:
python -m platformio run --target upload --upload-port COM4
```

Sostituire `COM4` con la propria porta. Il binario è
`.pio/build/d1_mini/firmware.bin`. La configurazione fissa piattaforma,
scheda e layout della flash; il core Arduino usato è ESP8266 3.1.2.

## Primo avvio e aggiornamenti

1. Collegarsi a `WiFi-daikin-XXXXXX`, password iniziale `daikinsetup`.
2. Aprire `http://192.168.4.1` e configurare la rete domestica a 2,4 GHz.
3. Tornare alla rete domestica e aprire `http://daikin-wemos.local`
   oppure l'indirizzo assegnato dal router.
4. Leggere la chiave di aggiornamento dal monitor seriale a 115200 baud.
   La chiave viene generata sul dispositivo e non è contenuta nei sorgenti.
5. Per gli aggiornamenti successivi aprire il menu **Firmware**, selezionare
   il `.bin` e inserire la chiave. Non interrompere l'alimentazione.

Il filesystem si inizializza esplicitamente dalla sezione Apprendimento:
questa operazione cancella eventuali file già presenti nella partizione.
La raccolta usa campioni ogni 30 secondi, con salvataggio ogni cinque minuti
e archivio circolare fino a 512 KiB. Gli ultimi campioni possono andare persi
in caso di interruzione di alimentazione. Gli orari settimanali richiedono NTP,
con fuso Europe/Rome; il timer relativo non richiede l'orologio sincronizzato.

## Rete locale e debug

L'interfaccia usa HTTP e i normali comandi di controllo sono accessibili ai
dispositivi della rete locale. Il debug, una volta abilitato con la chiave,
rimane attivo fino alla disattivazione. Non esporre il Wemos su Internet.
Non pubblicare chiavi, credenziali Wi-Fi, esportazioni della EEPROM o dump
della flash: possono contenere i dati del proprio dispositivo.

## Documentazione e apprendimento

- [Manuale completo](docs/manuale.md)
- [Raccolta e addestramento del modello](docs/apprendimento.md)
- [Riferimenti e attribuzioni](NOTICE.md)
- [Registro delle modifiche](CHANGELOG.md)

```sh
python -m pip install -r tools/apprendimento/requirements.txt
python tools/apprendimento/apprendimento.py raccogli --url http://daikin-wemos.local
python tools/apprendimento/apprendimento.py addestra --csv dati-scaricati.csv
```

La raccolta sul Wemos continua senza PC. La raccolta alternativa sul PC
richiede che rimanga acceso; `Ctrl+C` la interrompe.

## Verifiche

```sh
python -m pip install -r requirements-dev.txt
python -m playwright install chromium
python -m unittest discover -s tests -v
```

I test servono la pagina gzip con un vero server HTTP locale e usano risposte API simulate: non inviano
comandi a un climatizzatore. Controllano navigazione mobile, applicazione automatica,
pin e compatibilità del CSV, oltre alla preparazione dei dati di apprendimento.
GitHub Actions compila il firmware ed esegue i test in Chromium e Firefox; produce il binario come artefatto.
Le funzioni S21 dipendono dall'unità e richiedono verifica hardware.

## Licenza

[GPL-3.0-or-later](LICENSE), coerente con l'intestazione dello sketch.
I marchi citati appartengono ai rispettivi titolari.

## Modifica della pagina web

Dopo aver modificato HTML, CSS o JavaScript nello sketch, eseguire:

```sh
python tools/pack_web.py
```

Questo rigenera i byte gzip incorporati nello stesso file `.ino`. Il firmware
serve quella copia compressa, con cache disabilitata. I test verificano che
corrisponda esattamente al sorgente e che il browser completi l'avvio.
Lo sketch distribuito è già pronto e si compila anche con Arduino IDE.
Le richieste hanno un limite di attesa; lo stato non avvia letture sovrapposte.
