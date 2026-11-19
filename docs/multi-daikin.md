# Multi-Daikin 2.16

## Collegare le schede

1. Installa lo sketch completo o il binario 2.16 Multi-Daikin su ogni Wemos D1 Mini. Mantieni il layout 4 MB con FS da 1 MB.
2. Apri ciascuna scheda dalla rete Wi-Fi locale. In **Preferenze → Multi-Daikin** abilita la funzione. Su una scheda premi **Genera una chiave** e copia i 64 caratteri prima di salvare.
3. Inserisci la stessa chiave sulle altre schede. Il salvataggio richiede la chiave firmware locale della scheda su cui stai operando. La scheda utilizzata per generare la chiave non ha un ruolo speciale.
4. Apri **Multi-Daikin** dal menu. Le schede con la chiave comune diventano abbinate dopo lo scambio di messaggi firmati. Per usare le programmazioni condivise serve LittleFS montato. La sezione Apprendimento indica lo stato dell’archivio.
5. Se il router filtra il broadcast, inserisci in Preferenze gli IPv4 correnti delle altre schede. Sono disponibili sette indirizzi manuali per scheda. Se il broadcast è filtrato, inserisci gli IP delle altre schede su ciascun Wemos. Per IP manuali conviene usare una prenotazione DHCP.

La rete deve consentire traffico UDP tra le schede sulla porta 42162. È supportata una sola sottorete IPv4 Wi-Fi. Le reti ospiti con isolamento fra client possono impedire anche il collegamento manuale.

Con Multi-Daikin attivo ogni scheda annuncia un nome mDNS distinto nella forma `daikin-wemos-XXXXXXXX.local`. L’identificativo è ricavato dall’indirizzo MAC della scheda. L’IP corrente compare nell’elenco e viene aggiornato quando cambia via DHCP. Su più schede appena aggiornate, usa gli IP del router per il primo accesso se condividono ancora il nome mDNS predefinito.

## Comandi e configurazione

Tocca una scheda per vedere stato, temperatura ambiente, temperatura richiesta, modalità e ultimo contatto. Il timer e i comandi disponibili dipendono dallo stato dell’unità. Per Wi-Fi, pin, OTA, debug, apprendimento e orari individuali i collegamenti aprono l’interfaccia della scheda scelta, usando il suo IP corrente. Nella Home usa il selettore sotto il nome del dispositivo per scegliere quale climatizzatore comandare. La ghiera, l’accensione, la ventola, l’oscillazione, Powerful, Econo e il timer seguono la selezione. Se l’unità scelta va offline, i comandi si disabilitano fino al suo ritorno o a una tua nuova scelta. Per il controllo dalla Home aggiorna anche le schede destinatarie al firmware con il selettore.

I comandi globali mostrano una conferma e poi gli esiti separati per l’unità locale e per le unità abbinate ancora in elenco. I Wemos remoti ricevono richieste indipendenti, quindi un dispositivo offline o un errore S21 non annulla gli altri comandi. Una scrittura S21 con ACK ma senza rilettura valida è mostrata come non confermata. Il timer conferma la registrazione della scadenza sul Wemos.

Una scheda è offline dopo 45 secondi senza contatti utili e scompare dall’elenco dopo circa cinque minuti. Alla riconnessione viene riconosciuta per ID. L’accesso alle API storiche di quella scheda avviene collegandosi direttamente al suo HTTP locale.

## Programmazioni condivise

Imposta nome, giorni e stato, poi aggiungi sino a sei azioni. Per ogni azione scegli ora, modalità e temperatura, oppure Spegni. Seleziona una o più unità individuali o **Tutti**, che include le schede abbinate che riceveranno la programmazione anche in futuro.

Ogni scheda conserva una copia su LittleFS. I record includono l’ID del creatore, un ID della programmazione, la revisione e l’ID di chi ha salvato la modifica. Vince la revisione maggiore. A parità di revisione vince l’ID maggiore dell’ultima scheda che ha modificato il record. Come ulteriore spareggio si usa l’ordine dei byte del record. Due modifiche contemporanee alla stessa programmazione possono quindi far perdere una delle due versioni. Modifiche a programmazioni diverse si uniscono. Una modifica da una pagina con versione superata riceve un conflitto e richiede di ricaricare.

La scoperta scambia un riepilogo delle copie salvate. Se le copie differiscono, le schede si scambiano piccoli record a rotazione. Le ricezioni identiche non causano scritture. Le revisioni in arrivo vengono raggruppate prima del salvataggio. La scheda mostra sincronizzazione in attesa, unità non raggiungibili e problemi di memoria o validazione. Attendi che le unità interessate risultino sincronizzate prima di contare sul nuovo orario.

L’orario locale ha precedenza se cade nello stesso minuto, anche quando il relativo comando locale non viene confermato. Se coincidono più azioni condivise sulla stessa unità, vince la programmazione con coppia creatore/ID più bassa e poi la prima azione nell’ordine della scheda. Ogni Wemos usa l’orologio locale già disponibile nel progetto e può eseguire le proprie azioni mentre gli altri sono offline o il Wi-Fi è caduto. Dopo un riavvio l’orologio deve risincronizzarsi. Una scheda con orologio valido può fornire l’ora alle schede abbinate che non la conoscono ancora. Su una LAN senza accesso a NTP, usa **Preferenze → Multi-Daikin → Sincronizza ora dal browser** per dare l’ora a una scheda, inserendo la sua chiave firmware. Questo permette di avviare la programmazione senza un server esterno anche dopo un riavvio simultaneo. Gli eventi di minuti già passati non vengono recuperati.

Prima di eseguire un’azione condivisa, la scheda registra l’identificativo e il minuto in LittleFS, poi tenta la scrittura S21 e la rilettura. Un’interruzione tra registrazione e scrittura può far saltare l’azione. Il sistema non promette esecuzione esattamente una volta. Un errore S21 è riportato senza ritentare la stessa azione condivisa nello stesso minuto.

## Limiti e sicurezza

- Fino a otto schede visibili per Wemos, inclusa la propria. La tabella remota ha sette posti fissi e privilegia le schede autenticate quando lo spazio è esaurito.
- Sedici identità di programmazione, sei azioni per identità e fino a otto destinatari per azione. I sedici record occupano 4.736 byte e vengono allocati all’attivazione con abbinamento. Limiti più alti aumenterebbero l’uso di RAM, il tempo di riallineamento e l’ingombro delle risposte. Ogni datagramma rimane sotto 368 byte.
- I marcatori delle programmazioni eliminate occupano un posto. Conservarli evita che una scheda rimasta offline ripubblichi un record cancellato. Per una nuova rete con archivio vuoto serve una nuova chiave su tutte le schede. Annota e ricrea gli orari da conservare prima di cambiare chiave. Se più schede offline creano complessivamente più identità della capacità disponibile, la sincronizzazione segnala archivio pieno.
- Gli annunci di scoperta contengono nome, IP e versione. L’autorizzazione tra schede usa HMAC-SHA256 con tag di 128 bit e chiave comune. I comandi usano una sfida temporanea e un ID per riconoscere le ritrasmissioni. Qualsiasi scheda con la chiave fa parte della stessa rete di fiducia.
- HTTP locale non è cifrato. Chi può intercettare la configurazione può leggere le chiavi durante l’inserimento. I token e le credenziali Wi-Fi non sono restituiti dalle nuove API. Usa una LAN fidata e non inoltrare la porta HTTP o UDP su Internet. Un client LAN capace di chiamare le API locali di un Wemos abbinato può usare anche il suo pannello Multi-Daikin.
- I file di Multi-Daikin occupano pochi KiB di LittleFS oltre allo storico e all’apprendimento. LittleFS rimane sulla partizione esistente e i dati EEPROM già salvati restano nelle regioni originali.

## Collaudo con hardware

Prova prima su due Wemos, ciascuno collegato al proprio split. Verifica scoperta e abbinamento da entrambe le interfacce, poi cambia una concessione DHCP. Prova i comandi uno alla volta e confronta stato e telecomando, incluse modalità dry, fan, Powerful, Econo e oscillazione supportata.

Verifica uno spegnimento globale con un’unità scollegata. Salva una programmazione dal primo Wemos e controlla che il secondo la esegua, dopo la sincronizzazione, anche con il primo spento. Ripeti con riavvio, perdita del Wi-Fi, conflitto con un orario individuale e modifica simultanea dalle due schede. Verifica poi una seconda OTA e misura heap disponibile da Debug con storico, interfaccia e sincronizzazione in uso. I test automatici simulano i dispositivi e non sostituiscono queste prove.
