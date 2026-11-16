# Raccolta dati e apprendimento

Il Wemos può prevedere la temperatura ambiente fra dieci minuti, con comandi
invariati. La rete lavora **in osservazione**: non regola il climatizzatore.

## Raccolta sul Wemos

Aprire Apprendimento. Se richiesto, inizializzare l'archivio con la chiave
e la conferma esplicita. L'operazione cancella eventuali file LittleFS,
conservando le impostazioni EEPROM. Dopo ogni riavvio serve che l'orologio
sia sincronizzato via NTP. La raccolta continua senza PC.

Un campione viene acquisito ogni 30 secondi; dieci campioni vengono salvati
insieme ogni cinque minuti. Un'interruzione di alimentazione può perdere
i campioni ancora in RAM. L'archivio circolare usa due segmenti da 256 KiB,
per un massimo di 512 KiB. Alla rotazione viene eliminato il segmento più
vecchio. Scaricare regolarmente con **Scarica dati CSV**: il download include
anche i campioni non ancora salvati. Il timer del climatizzatore non spegne
il Wemos, che continua a raccogliere dati.

Il grafico offre intervalli 1, 6, 24 ore o tutto lo storico. **Aggiorna grafico**
rilegge l'archivio. La colonna `prediction10m` è facoltativa nei CSV precedenti
alla 2.8 e contiene valori solo quando sono disponibili previsioni.
Le previsioni sono disegnate dieci minuti dopo il campione di origine.

## Raccolta facoltativa sul PC

Python 3.10 o successivo. La sola raccolta usa librerie standard:

```sh
python tools/apprendimento/apprendimento.py raccogli --url http://daikin-wemos.local
```

Il PC deve restare acceso e raggiungere il Wemos. Specificare l'IP se il nome
locale non funziona. Usare `--csv percorso/dati.csv` per un'altra destinazione.
Interrompere con `Ctrl+C`, oppure creare un file `.stop` con lo stesso nome
base del CSV. Il file `.status.json` riporta lo stato della raccolta.

## Preparazione dei dati

Usare più giorni di normale funzionamento, con variazioni della temperatura
esterna e diversi cicli di accensione. Le letture provengono dal sensore dello
split, non da un sensore indipendente della stanza. Gli scaricamenti
sovrapposti si possono unire senza duplicare i timestamp:

```sh
python tools/apprendimento/apprendimento.py unisci giorno1.csv giorno2.csv --output dati-uniti.csv
```

Servono almeno 24 ore e 240 esempi validi: il minimo non garantisce che i dati
rappresentino adeguatamente la stanza. Il programma esclude finestre con
letture mancanti o comandi cambiati nei dieci minuti successivi.

## Addestramento e valutazione

```sh
python -m pip install -r tools/apprendimento/requirements.txt
python tools/apprendimento/apprendimento.py addestra --csv dati-uniti.csv --output modello
```

La rete ha 12 ingressi e 8 neuroni nascosti. Gli ingressi sono temperatura
ambiente, esterna, richiesta, obiettivo interno, giri ventola, variazione in
cinque minuti, alimentazione, modalità caldo/freddo/auto e stati turbo/eco.
La separazione fra addestramento e verifica è cronologica, con una distanza
di dieci minuti per evitare sovrapposizioni delle etichette.

Il rapporto `valutazione.json` confronta l'errore medio con temperatura
costante e prosecuzione della tendenza. Il modello viene esportato solo se
migliora entrambi di almeno il 10%. Se viene respinto, non caricare un vecchio
file eventualmente presente nella directory di destinazione.

## Caricamento del modello

In Apprendimento scegliere `modello/modello-wemos.txt` e inserire la chiave.
Il modello è salvato in EEPROM: non serve ricompilare il firmware. Dopo un
riavvio il Wemos ricostruisce cinque minuti di letture prima di produrre
previsioni. Il firmware rifiuta file incompleti, valori non finiti e condizioni
esterne all'intervallo rappresentato dai dati di addestramento.

Proseguire l'osservazione su giornate successive: una buona verifica storica
non garantisce la qualità delle previsioni future. Il repository non contiene
un modello preaddestrato né dati personali di un impianto.

## Apprendimento locale automatico · v2.16

Il Wemos apprende dalle nuove letture e conserva i pesi in LittleFS. Il modello
PC è facoltativo. La scheda mostra esempi maturati, ore di letture da acceso,
errori recenti rispetto a temperatura costante e tendenza, fase della verifica
e ultimo salvataggio. Gli errori recenti riguardano la rete che si allena; la
verifica usa una copia separata e ferma su letture future.

Servono almeno 2.400 esempi validi da acceso (20 ore di esempi stabili), poi
almeno 480 esempi di verifica, di cui 240 da acceso. I tempi effettivi possono
essere maggiori, perché i primi cinque minuti, i cambi di comando, le letture
mancanti e le sequenze prive di dieci minuti successivi validi non producono
esempi. Non è garantito che la rete superi la verifica.

I progressi si salvano ogni ora; **Salva ora** permette di conservarli prima
di spegnere il Wemos. Due file alternati con checksum proteggono dal salvataggio
incompleto. L'inizializzazione del filesystem cancella anche questi file.
La colonna CSV `onlineCandidate10m` contiene la previsione provvisoria, con
precisione di tre decimali, e non va confusa con una previsione verificata.
Nessun algoritmo modifica automaticamente temperatura, ventola o alimentazione.
