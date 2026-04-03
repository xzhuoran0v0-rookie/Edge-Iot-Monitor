#!/usr/bin/env python3
"""
test_ollama.py — Ollama / Qwen2 Connectivity Test
===================================================
Verifies that Ollama is running and the Qwen2 model responds correctly
before integrating with the C++ backend.

Usage:
    python3 scripts/test_ollama.py
"""

import json
import urllib.request
import urllib.error

OLLAMA_ENDPOINT = "http://127.0.0.1:11434/api/generate"
MODEL           = "qwen2:7b-instruct-q4_K_M"

TEST_PROMPT = """
You are an IoT monitoring assistant. Analyze the following sensor data and respond in JSON format only.
JSON fields: severity (NORMAL/LOW/MEDIUM/HIGH/CRITICAL), diagnosis (string), recommendation (string).

Sensor window (last 60 seconds):
- Device: esp32-s3-001
- Temperature: avg=27.4°C, max=31.2°C, min=26.8°C
- Humidity: avg=68.0%, max=70.1%, min=65.3%
- Anomalies detected: 1 (temperature spike to 31.2°C at 14:32:05)

Respond ONLY with a valid JSON object, no markdown, no preamble.
"""


def test_connection():
    print(f"Testing Ollama endpoint: {OLLAMA_ENDPOINT}")
    print(f"Model: {MODEL}")
    print("-" * 48)

    payload = json.dumps({
        "model":  MODEL,
        "prompt": TEST_PROMPT,
        "stream": False,
    }).encode("utf-8")

    req = urllib.request.Request(
        OLLAMA_ENDPOINT,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            raw = resp.read().decode("utf-8")
            data = json.loads(raw)
            response_text = data.get("response", "")

            print("Raw model response:")
            print(response_text)
            print("-" * 48)

            # Attempt to parse as JSON (as instructed in prompt)
            try:
                parsed = json.loads(response_text.strip())
                print("Parsed fields:")
                print(f"  severity      : {parsed.get('severity')}")
                print(f"  diagnosis     : {parsed.get('diagnosis')}")
                print(f"  recommendation: {parsed.get('recommendation')}")
                print("\n[PASS] Ollama + Qwen2 are working correctly.")
            except json.JSONDecodeError:
                print("[WARN] Model did not return valid JSON — adjust prompt engineering.")

    except urllib.error.URLError as e:
        print(f"[FAIL] Cannot reach Ollama: {e.reason}")
        print("Make sure 'ollama serve' is running.")


if __name__ == "__main__":
    test_connection()
