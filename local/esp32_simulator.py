#!/usr/bin/env python3
"""Simula un ESP32 intermittentemente connesso usando mosquitto_sub.

La sessione MQTT è persistente: stesso client-id + clean-session=false (-c).
Il command_id già eseguito viene simulato tramite un piccolo file JSON.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from dotenv import load_dotenv
from pathlib import Path

load_dotenv()

BROKER_HOST = os.getenv("MQTT_HOST")
BROKER_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME_ESP32")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD_ESP32")
TOPIC = os.getenv("MQTT_TOPIC", "opengate/cmd")
ACK_TOPIC = os.getenv("MQTT_ACK_TOPIC", "opengate/ack")
CA_PATH = os.getenv("MQTT_CA_PATH", "/etc/ssl/certs/")
CLIENT_ID = os.getenv("MQTT_CLIENT_ID", "ESP32_GATE_SIM_01")
STATE_FILE = Path(os.getenv("MQTT_STATE_FILE", "./esp32_nvs_sim.json"))
MAX_PROCESSED_IDS = 20


class GateState:
    def __init__(self, path: Path):
        self.path = path
        self.data = {"processed_command_ids": [], "last_command_id": None}
        self._load()

    def _load(self) -> None:
        try:
            self.data = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (json.JSONDecodeError, OSError) as exc:
            print(f"[ESP32] Attenzione: impossibile leggere {self.path}: {exc}")

    def has_processed(self, command_id: str) -> bool:
        return command_id in self.data.get("processed_command_ids", [])

    def mark_processed(self, command_id: str) -> None:
        ids = self.data.setdefault("processed_command_ids", [])
        if command_id in ids:
            return
        ids.append(command_id)
        self.data["last_command_id"] = command_id
        self.data["processed_command_ids"] = ids[-MAX_PROCESSED_IDS:]
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = self.path.with_suffix(self.path.suffix + ".tmp")
        temp_path.write_text(json.dumps(self.data, indent=2), encoding="utf-8")
        temp_path.replace(self.path)


def gate_open(command_id: str) -> None:
    """Simula l'azionamento del GPIO del cancello."""
    print(f"[ESP32] *** GPIO -> OPEN (comando {command_id}) ***")
    time.sleep(1)
    print("[ESP32] *** Cancello azionato ***")


def publish_ack(command_id: str, result: str = "OK") -> None:
    payload = json.dumps({
        "id": command_id,
        "result": result,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }, separators=(",", ":"))

    cmd = [
        "mosquitto_pub",
        "-h", BROKER_HOST,
        "-p", str(BROKER_PORT),
        "-u", MQTT_USERNAME,
        "-P", MQTT_PASSWORD,
        "--capath", CA_PATH,
        "-V", "mqttv311",
        "-q", "1",
        "-t", ACK_TOPIC,
        "-m", payload,
        "-d",
    ]

    try:
        result_run = subprocess.run(cmd, check=False, text=True)
    except FileNotFoundError:
        print("[ESP32] ERRORE: mosquitto_pub non è installato.", file=sys.stderr)
        return

    if result_run.returncode != 0:
        print(f"[ESP32] Attenzione: ACK non pubblicato (exit {result_run.returncode}).")
    else:
        print(f"[ESP32] ACK pubblicato su {ACK_TOPIC}: {payload}")


def receive_one_message(timeout_s: int):
    """Si connette, ripristina la sessione persistente e attende un messaggio.

    - -c  => clean-session=false (MQTT 3.1.1)
    - -i  => client ID stabile, fondamentale per ritrovare la sessione
    - -q 1 => subscription QoS 1
    - -C 1 => termina dopo aver ricevuto un messaggio
    """
    cmd = [
        "mosquitto_sub",
        "-h", BROKER_HOST,
        "-p", str(BROKER_PORT),
        "-u", MQTT_USERNAME,
        "-P", MQTT_PASSWORD,
        "--capath", CA_PATH,
        "-V", "mqttv311",
        "-c",
        "-i", CLIENT_ID,
        "-q", "1",
        "-t", TOPIC,
        "-C", "1",
        "-W", str(timeout_s),
        "-v",
    ]

    print(f"[ESP32] Connessione MQTT: client_id={CLIENT_ID}, persistent session=true")
    print(f"[ESP32] Attendo un comando per massimo {timeout_s} s...")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_s + 10,
            check=False,
        )
    except FileNotFoundError:
        print("ERRORE: mosquitto_sub non è installato o non è nel PATH.", file=sys.stderr)
        sys.exit(2)
    except subprocess.TimeoutExpired:
        return None

    if result.stderr:
        # Il debug di mosquitto è utile, ma lo rendiamo meno invasivo.
        print(result.stderr, end="")

    if result.returncode != 0 and not result.stdout.strip():
        print(f"[ESP32] Nessun messaggio ricevuto (exit {result.returncode}).")
        return None

    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        print("[ESP32] Nessun messaggio ricevuto.")
        return None

    line = lines[-1]
    # Con -v il formato è: <topic> <payload>
    parts = line.split(" ", 1)
    if len(parts) != 2:
        print(f"[ESP32] Messaggio inatteso: {line}")
        return None

    topic, payload = parts
    print(f"[ESP32] Ricevuto topic={topic}, payload={payload}")

    try:
        message = json.loads(payload)
    except json.JSONDecodeError as exc:
        print(f"[ESP32] Payload JSON non valido: {exc}")
        return None

    return message


def run_cycle(state: GateState, awake_timeout: int) -> None:
    print("\n================ ESP32 WAKE UP ================")
    message = receive_one_message(awake_timeout)

    if message is None:
        print("[ESP32] Nessun comando pendente.")
        print("[ESP32] -> DEEP SLEEP")
        return

    command_id = message.get("id")
    command = message.get("command")
    if not command_id or not command:
        print("[ESP32] Messaggio senza id/command: ignorato.")
        print("[ESP32] -> DEEP SLEEP")
        return

    if state.has_processed(command_id):
        print(f"[ESP32] Comando DUPLICATO {command_id}: NON eseguo il GPIO.")
        publish_ack(command_id, "DUPLICATE")
    elif command == "OPEN":
        gate_open(command_id)
        state.mark_processed(command_id)
        print(f"[ESP32] command_id salvato in {STATE_FILE}")
        publish_ack(command_id, "OK")
    else:
        print(f"[ESP32] Comando sconosciuto: {command}")
        publish_ack(command_id, "UNKNOWN_COMMAND")

    print("[ESP32] -> DEEP SLEEP")


def main() -> None:
    parser = argparse.ArgumentParser(description="Simulatore ESP32 MQTT intermittente")
    parser.add_argument("--cycles", type=int, default=1, help="Numero di cicli wake/sleep da simulare")
    parser.add_argument("--sleep", type=int, default=60, help="Secondi di deep sleep tra due wake-up")
    parser.add_argument("--awake-timeout", type=int, default=15, help="Secondi di ascolto MQTT durante il wake-up")
    parser.add_argument("--state-file", default=str(STATE_FILE), help="File che simula la NVS")
    args = parser.parse_args()

    state = GateState(Path(args.state_file))
    print("[ESP32] Simulatore avviato")
    print(f"[ESP32] Broker : {BROKER_HOST}:{BROKER_PORT}")
    print(f"[ESP32] Topic  : {TOPIC}")
    print(f"[ESP32] Client : {CLIENT_ID}")
    print(f"[ESP32] NVS    : {state.path.resolve()}")

    for cycle in range(1, args.cycles + 1):
        print(f"\n[ESP32] CICLO {cycle}/{args.cycles}")
        run_cycle(state, args.awake_timeout)
        if cycle < args.cycles:
            print(f"[ESP32] Sleep per {args.sleep} s...")
            time.sleep(args.sleep)


if __name__ == "__main__":
    main()
