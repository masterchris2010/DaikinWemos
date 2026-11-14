"""Raccolta e addestramento locale per DaikinWemos. Nessun comando al climatizzatore."""

import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import time
from urllib.error import HTTPError, URLError
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parent
FEATURES = ["room", "outside", "target", "internalTarget", "rpm", "trend5m",
            "power", "heat", "cool", "auto", "turbo", "eco"]
FIELDS = ["utc", "timestamp", "online", "version", "mode", "fan", "room",
          "outside", "target", "internalTarget", "rpm", "power", "turbo", "eco"]


def collect(args):
    path = Path(args.csv)
    path.parent.mkdir(parents=True, exist_ok=True)
    status_path = path.with_suffix(".status.json")
    stop = path.with_suffix(".stop")
    stop.unlink(missing_ok=True)
    samples = 0
    with path.open("a", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        if path.stat().st_size == 0:
            writer.writeheader()
        while not stop.exists():
            started = time.monotonic()
            now = time.time()
            error = ""
            try:
                with urlopen(args.url.rstrip("/") + "/api/state", timeout=6) as response:
                    state = json.load(response)
                row = {name: state.get(name) for name in FIELDS}
                row.update(utc=datetime.fromtimestamp(now, timezone.utc).isoformat(),
                           timestamp=now, rpm=state.get("fanRpm"))
                for flag in ("online", "power", "turbo", "eco"):
                    row[flag] = int(state[flag]) if state.get(flag) is not None else ""
                writer.writerow(row)
                stream.flush()
                samples += 1
            except (OSError, ValueError, HTTPError, URLError) as exc:
                error = str(exc)
            status_path.write_text(json.dumps({
                "utc": datetime.now(timezone.utc).isoformat(), "running": True,
                "samples_this_run": samples, "error": error,
            }, indent=2), encoding="utf-8")
            if args.once:
                break
            remaining = max(0, args.interval - (time.monotonic() - started))
            deadline = time.monotonic() + remaining
            while time.monotonic() < deadline and not stop.exists():
                time.sleep(min(1, max(0, deadline - time.monotonic())))
    status_path.write_text(json.dumps({"running": False, "samples_this_run": samples}), encoding="utf-8")


def load_examples(path):
    import numpy as np

    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        raw_rows = list(csv.DictReader(stream))
    rows = []
    for row in raw_rows:
        try:
            numeric = {key: float(row[key]) for key in (
                "timestamp", "online", "room", "outside", "target", "internalTarget",
                "rpm", "power", "turbo", "eco")}
            if not all(math.isfinite(value) for value in numeric.values()) or not numeric["online"]:
                continue
            numeric.update(mode=row["mode"], fan=row["fan"])
            rows.append(numeric)
        except (ValueError, KeyError):
            continue
    # Remove overlapping collections, then preserve temporal order.
    rows = sorted({row["timestamp"]: row for row in rows}.values(), key=lambda r: r["timestamp"])
    if not rows:
        return np.empty((0, len(FEATURES))), np.array([]), np.array([]), np.array([])
    times = np.array([row["timestamp"] for row in rows])
    features, labels, stamps, room_values = [], [], [], []
    control_keys = ("mode", "fan", "target", "power", "turbo", "eco")
    for index, row in enumerate(rows):
        if row["timestamp"] - times[0] < 300:
            continue
        previous = int(np.searchsorted(times, row["timestamp"] - 300))
        future = int(np.searchsorted(times, row["timestamp"] + 600))
        if previous >= index or future >= len(rows):
            continue
        if abs(times[previous] - (times[index] - 300)) > 35 or times[future] - times[index] > 635:
            continue
        if np.max(np.diff(times[previous:future + 1])) > 65:
            continue
        # The prediction assumes that the current commands stay unchanged for ten minutes.
        if any(any(rows[j][key] != row[key] for key in control_keys) for j in range(index, future + 1)):
            continue
        trend = row["room"] - rows[previous]["room"]
        features.append([row["room"], row["outside"], row["target"], row["internalTarget"],
                         row["rpm"], trend, row["power"], int(row["mode"] == "4"),
                         int(row["mode"] == "3"), int(row["mode"] == "1"), row["turbo"], row["eco"]])
        labels.append(rows[future]["room"] - row["room"])
        stamps.append(row["timestamp"])
        room_values.append(row["room"])
    return np.asarray(features), np.asarray(labels), np.asarray(stamps), np.asarray(room_values)


def train(args):
    import numpy as np
    from sklearn.metrics import mean_absolute_error
    from sklearn.neural_network import MLPRegressor
    from sklearn.preprocessing import StandardScaler

    x, y, stamps, rooms = load_examples(args.csv)
    if len(x) < 240 or stamps[-1] - stamps[0] < 24 * 3600:
        raise SystemExit(f"Dati insufficienti: {len(x)} esempi validi. Servono almeno 24 ore e 240 esempi stabili. Preferibili più giorni.")
    # Temporal holdout with a horizon-sized gap, so training labels cannot overlap validation.
    split = int(len(x) * 0.8)
    validation_start = stamps[split]
    training = stamps < validation_start - 600
    validation = stamps >= validation_start
    if training.sum() < 180 or validation.sum() < 48:
        raise SystemExit("Dati insufficienti per separare addestramento e verifica temporale.")
    scaler = StandardScaler().fit(x[training])
    model = MLPRegressor(hidden_layer_sizes=(8,), activation="tanh", solver="lbfgs",
                         alpha=1.0, max_iter=1200, random_state=17)
    model.fit(scaler.transform(x[training]), y[training])
    prediction = model.predict(scaler.transform(x[validation]))
    network_mae = float(mean_absolute_error(y[validation], prediction))
    persistence_mae = float(np.mean(np.abs(y[validation])))
    trend_mae = float(mean_absolute_error(y[validation], 2 * x[validation, 5]))
    # Export only if the network beats both simple forecasts by at least ten percent.
    accepted = network_mae < 0.9 * min(persistence_mae, trend_mae)
    report = {
        "examples": len(x), "training": int(training.sum()), "validation": int(validation.sum()),
        "horizon_minutes": 10, "network_mae_c": network_mae,
        "persistence_mae_c": persistence_mae, "trend_mae_c": trend_mae,
        "accepted": bool(accepted), "features": FEATURES,
        "assumption": "Comandi invariati nei dieci minuti successivi",
    }
    destination = Path(args.output)
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "valutazione.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    if not accepted:
        raise SystemExit("La rete non migliora sufficientemente le previsioni semplici. Nessun modello pubblicato.")
    values = np.concatenate([scaler.mean_, scaler.scale_, x[training].min(axis=0),
                             x[training].max(axis=0), model.coefs_[0].ravel(),
                             model.intercepts_[0], model.coefs_[1].ravel(), model.intercepts_[1]])
    assert len(values) == 161 and np.isfinite(values).all()
    # This exact layout is read by the standalone ESP8266 sketch.
    payload = "DWMODEL1\n" + " ".join(format(float(value), ".9g") for value in values) + "\n"
    (destination / "modello-wemos.txt").write_text(payload, encoding="ascii")
    print("Modello pronto in", destination / "modello-wemos.txt")


def merge(args):
    """Unisce download sovrapposti senza duplicare i campioni con lo stesso timestamp."""
    rows = {}
    for name in args.files:
        with Path(name).open(encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            if not reader.fieldnames or not set(FIELDS).issubset(reader.fieldnames):
                raise SystemExit(f"Intestazione CSV non riconosciuta: {name}")
            for row in reader:
                try:
                    stamp = float(row["timestamp"])
                except (ValueError, TypeError):
                    continue
                if math.isfinite(stamp):
                    rows[stamp] = {field: row.get(field, "") for field in FIELDS}
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".tmp")
    with temporary.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows[stamp] for stamp in sorted(rows))
    temporary.replace(destination)
    print(f"Uniti {len(rows)} campioni in {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collector = commands.add_parser("raccogli")
    collector.add_argument("--url", default="http://daikin-wemos.local")
    collector.add_argument("--csv", default=str(ROOT / "dati.csv"))
    collector.add_argument("--interval", type=int, default=30)
    collector.add_argument("--once", action="store_true")
    trainer = commands.add_parser("addestra")
    trainer.add_argument("--csv", default=str(ROOT / "dati.csv"))
    trainer.add_argument("--output", default=str(ROOT / "modello"))
    merger = commands.add_parser("unisci")
    merger.add_argument("files", nargs="+")
    merger.add_argument("--output", default=str(ROOT / "dati-uniti.csv"))
    args = parser.parse_args()
    if args.command == "raccogli":
        if args.interval < 10:
            parser.error("Intervallo minimo 10 secondi")
        collect(args)
    elif args.command == "addestra":
        train(args)
    else:
        merge(args)


if __name__ == "__main__":
    main()
