# API HTTP Multi-Daikin 2.16

Le API sono servite dal Wemos su cui è aperta l’interfaccia. I POST accettano `application/x-www-form-urlencoded` e richiedono l’header `X-Multi-Request: 1` da un IP della stessa sottorete del Wi-Fi. L’API di configurazione richiede anche `X-Firmware-Key`. I POST non abilitano CORS. Gli endpoint esistenti del singolo climatizzatore mantengono le proprie regole.

| Metodo e percorso | Dati e risposta |
| --- | --- |
| GET `/api/multi/devices` | Stato locale, ID `self`, abilitazione, IP manuali, unità note e informazioni di sincronizzazione. `devices` include ID, nome, versione, IP, `online`, `paired`, `age` in secondi, disponibilità S21, letture, capacità e secondi del timer. `swing` usa i bit 1 e 2 per verticale e orizzontale, il bit 4 per supporto. `turbo` ed `eco` valgono -1 quando indisponibili. |
| POST `/api/multi/config` | `enabled` 0 o 1, `key` esadecimale di 64 caratteri o vuota per mantenere la chiave, `ip0…ip6` facoltativi. Gli IP vuoti rimuovono la relativa voce manuale. La chiave non viene restituita. Un POST con solo `clock` e i due header impostati sincronizza l’orologio con un timestamp Unix in secondi. |
| POST `/api/multi/command` | `id` numerico, `action`, `value`, eventuale `minutes`. Per `id=0` serve `confirmed=1` e un’azione globale. Risponde con `{batch,jobs:[{id,code}]}`. |
| GET `/api/multi/jobs` | Ultimo lotto di esiti. Verificare che `batch` corrisponda a quello del POST. |
| GET `/api/multi/plans` | `used`, `limit` e `plans`. Ogni piano espone `creator`, `id`, `editor`, `revision`, `name`, `days`, `enabled`, `steps`. Ogni azione contiene `minute`, `mode`, `half`, `targets`. |
| POST `/api/multi/plans` | Per creare usa `creator=id=editor=revision=0`. Per modificare o eliminare invia gli identificativi e la revisione letti. `delete=1` elimina. Altrimenti invia `name`, `days`, `enabled`, `count` e, per ogni indice da 0, `minuteN`, `modeN`, `halfN`, `targetsN`. |

Per i piani, domenica è il bit 0 di `days`. L’ora è in minuti dalla mezzanotte, la temperatura in mezzi gradi. Per spegnere usa modalità `0` e temperatura 0. Per selezionare Tutti passa esplicitamente `targetsN=`. Per unità specifiche passa i rispettivi ID decimali separati da virgole. Gli identificativi si leggono da `/api/multi/devices`.

| Azione | Valore |
| --- | --- |
| 1 Accensione | 0 spento, 1 acceso |
| 2 Temperatura | 32–60 in mezzi gradi, rispettando i minimi della modalità |
| 3 Modalità | Codice ASCII decimale di `1`, `2`, `3`, `4` o `6` |
| 4 Ventola | Codice ASCII decimale di `A`, `B` o `3`–`7` |
| 5 Oscillazione | 0 ferma, 1 verticale, 2 orizzontale, 3 entrambe |
| 6 Powerful, 7 Econo | 0 spento, 1 acceso |
| 10 Impostazioni dalla Home di un’unità | `value` 0/1 per alimentazione, `mode` in ASCII decimale, `fan` in ASCII decimale o 0 per conservare la ventola, `minutes` per il setpoint in mezzi gradi. Non applicabile a tutti. |
| 8 Timer | `value=0` e `minutes` da 0 a 1440, con 0 per annullare |

Il lotto finale contiene un codice per scheda. 0 attende, 200 conferma, 202 indica scrittura senza rilettura, 403 autorizzazione scaduta, 409 incongruenza o funzione indisponibile, 502 scrittura non confermata, 503 unità indisponibile, 504 scadenza. Il 200 HTTP del POST indica l’accodamento, mentre il 200 nella singola voce indica l’esito confermato. Un lotto nuovo può sostituire quello concluso, quindi il browser controlla l’identificativo prima di mostrare i risultati.
