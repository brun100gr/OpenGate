#!/usr/bin/env python3
"""Simula l'app Android: pubblica un comando OPEN via mosquitto_pub."""

import argparse
import json
import os
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from dotenv import load_dotenv

load_dotenv()

BROKER_HOST = os.getenv("MQTT_HOST")
BROKER_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME_ANDROID")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD_ANDROID")
TOPIC = os.getenv("MQTT_TOPIC", "opengate/cmd")
CA_PATH = os.getenv("MQTT_CA_PATH", "/etc/ssl/certs/")


def publish_command(command: str) -> str:
    command_id = str(uuid.uuid4())
    payload = {
        "id": command_id,
        "command": command,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    payload_json = json.dumps(payload, separators=(",", ":"))

    cmd = [
        "mosquitto_pub",
        "-h", BROKER_HOST,
        "-p", str(BROKER_PORT),
        "-u", MQTT_USERNAME,
        "-P", MQTT_PASSWORD,
        "--capath", CA_PATH,
        "-V", "mqttv311",
        "-q", "1",
        "-t", TOPIC,
        "-m", payload_json,
        "-d",
    ]

    print("[ANDROID] Pubblico:")
    print(f"  topic   : {TOPIC}")
    print(f"  payload : {payload_json}")
    print("  qos     : 1")
    print("  retain  : false")

    try:
        result = subprocess.run(cmd, check=False, text=True)
    except FileNotFoundError:
        print("ERRORE: mosquitto_pub non è installato o non è nel PATH.", file=sys.stderr)
        sys.exit(2)

    if result.returncode != 0:
        print(f"ERRORE: mosquitto_pub ha restituito exit code {result.returncode}.", file=sys.stderr)
        sys.exit(result.returncode)

    print(f"[ANDROID] Comando pubblicato con ID: {command_id}")
    return command_id


def main() -> None:
    parser = argparse.ArgumentParser(description="Simulatore Android MQTT per apertura cancello")
    parser.add_argument("--command", default="OPEN", choices=["OPEN"], help="Comando da inviare")
    args = parser.parse_args()

    publish_command(args.command)


if __name__ == "__main__":
    main()
