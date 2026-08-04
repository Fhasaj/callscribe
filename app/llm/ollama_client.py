"""Thin REST client for a local Ollama daemon -- list installed models,
pull one on demand, and run a JSON-mode generate call."""
from __future__ import annotations

import json
from typing import Callable, Optional

import requests


class OllamaError(RuntimeError):
    pass


def is_running(host: str) -> bool:
    try:
        requests.get(f"{host}/api/tags", timeout=3)
        return True
    except requests.RequestException:
        return False


def list_models(host: str) -> list[str]:
    try:
        resp = requests.get(f"{host}/api/tags", timeout=10)
        resp.raise_for_status()
    except requests.RequestException as exc:
        raise OllamaError(f"Could not reach Ollama at {host}: {exc}") from exc
    data = resp.json()
    return [m["name"] for m in data.get("models", [])]


def pull_model(host: str, model: str, on_progress: Optional[Callable[[str], None]] = None) -> None:
    try:
        with requests.post(
            f"{host}/api/pull", json={"name": model, "stream": True}, stream=True, timeout=None
        ) as resp:
            resp.raise_for_status()
            for line in resp.iter_lines():
                if not line:
                    continue
                payload = json.loads(line)
                if on_progress:
                    status = payload.get("status", "")
                    if "completed" in payload and "total" in payload and payload["total"]:
                        pct = 100 * payload["completed"] / payload["total"]
                        status = f"{status} ({pct:.0f}%)"
                    on_progress(status)
                if payload.get("error"):
                    raise OllamaError(payload["error"])
    except requests.RequestException as exc:
        raise OllamaError(f"Failed to pull {model}: {exc}") from exc


def generate_json(host: str, model: str, prompt: str, timeout: int = 300) -> dict:
    """Runs a JSON-mode generate call and returns the parsed object."""
    try:
        resp = requests.post(
            f"{host}/api/generate",
            json={"model": model, "prompt": prompt, "stream": False, "format": "json"},
            timeout=timeout,
        )
        resp.raise_for_status()
    except requests.RequestException as exc:
        raise OllamaError(f"Ollama generate failed: {exc}") from exc
    raw = resp.json().get("response", "")
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        start, end = raw.find("{"), raw.rfind("}")
        if start != -1 and end != -1:
            try:
                return json.loads(raw[start : end + 1])
            except json.JSONDecodeError:
                pass
        raise OllamaError(f"Model did not return valid JSON: {raw[:300]!r}") from exc


def generate_text(host: str, model: str, prompt: str, timeout: int = 300) -> str:
    try:
        resp = requests.post(
            f"{host}/api/generate",
            json={"model": model, "prompt": prompt, "stream": False},
            timeout=timeout,
        )
        resp.raise_for_status()
    except requests.RequestException as exc:
        raise OllamaError(f"Ollama generate failed: {exc}") from exc
    return resp.json().get("response", "").strip()
