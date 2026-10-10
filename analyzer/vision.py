"""AI part inspection using Google Gemini vision (google-genai SDK)."""
import json, os, re, time
from google import genai
from google.genai import types

# Models are tried in order. Models from VITAL_MODEL (.env) go first, then these fallbacks.
FALLBACKS = ["gemini-3.8-flash", "gemini-3.7-flash", "gemini-3.5-flash", "gemini-flash-latest"]
_env = [m.strip() for m in os.getenv("VITAL_MODEL", "").split(",") if m.strip()]
MODELS = list(dict.fromkeys(_env + FALLBACKS))

PARTS = {
    "tyre":    ("Tyre", "tread depth and wear pattern, sidewall cracks, bulges or bubbles, exposed cord/fabric, punctures or embedded objects, dry rot"),
    "battery": ("Battery", "terminal corrosion (white/blue powder), case bulging or cracks, electrolyte leakage, loose clamps"),
    "brake":   ("Brake (disc and pad)", "pad thickness, disc scoring or grooves, rust, glazing, uneven wear"),
    "engine":  ("Engine bay (incl. oil / coolant)", "oil or coolant leaks, fluid level and colour (milky or sludgy is bad), cracked hoses, belt cracks or fraying, loose connections, dirt build-up"),
    "dash":    ("Dashboard warning lights", "which warning lights are lit (check engine, battery, oil pressure, ABS, brake, temperature)"),
}

BUSY = ("503", "UNAVAILABLE", "429", "RESOURCE_EXHAUSTED")


class VisionUnavailable(Exception):
    pass


def _call(client, img_bytes, media_type, prompt):
    """Try each model; retry busy errors, then fall back to the next model."""
    last = None
    for model in MODELS:
        for attempt in range(2):
            try:
                resp = client.models.generate_content(
                    model=model,
                    contents=[types.Part.from_bytes(data=img_bytes, mime_type=media_type), prompt],
                    config=types.GenerateContentConfig(response_mime_type="application/json", temperature=0.2),
                )
                print(f"[VITAL] model used: {model}")
                return resp
            except Exception as e:
                last = e
                if any(c in str(e) for c in BUSY):
                    time.sleep(2 * (attempt + 1))
                    continue
                break  # 404 / invalid model etc: go to next model
    raise RuntimeError(f"Ella models um fail aachu. Last error: {last}")


def analyze(img_bytes: bytes, media_type: str, part: str) -> dict:
    if part not in PARTS:
        raise ValueError("Unknown part")
    key = os.getenv("GEMINI_API_KEY") or os.getenv("GOOGLE_API_KEY")
    if not key:
        raise VisionUnavailable("GEMINI_API_KEY set pannala. .env file la key podunga (README paarunga).")
    label, checks = PARTS[part]
    prompt = (
        f"You are a vehicle inspection assistant. The user says this photo shows: {label}.\n"
        "1) Decide whether the photo really shows that part.\n"
        f"2) If yes, inspect only what is actually visible. Typical checks: {checks}.\n"
        "Be conservative: never invent problems or hide visible ones; omit any check you cannot judge from the photo.\n"
        "Reply with ONLY valid JSON, no markdown:\n"
        '{"is_part": true/false, "detected": "what you actually see in one short sentence", '
        '"findings": [{"item": "short name", "status": "ok|warning|critical", "detail": "one sentence"}], '
        '"advice": "one or two sentences: what the owner should do next"}'
    )
    client = genai.Client(api_key=key)
    resp = _call(client, img_bytes, media_type, prompt)
    text = resp.text or ""
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        raise RuntimeError("AI response parse aagala, thirumba try pannunga")
    data = json.loads(m.group(0))
    data["part_label"] = label
    return _score(data)


def _score(d: dict) -> dict:
    pts = {"ok": 100, "warning": 60, "critical": 25}
    f = [x for x in d.get("findings", []) if x.get("status") in pts]
    for x in f:
        x["item"] = x["item"][:1].upper() + x["item"][1:]
    d["findings"] = f
    if d.get("is_part") and f:
        d["score"] = round(sum(pts[x["status"]] for x in f) / len(f))
        worst = {x["status"] for x in f}
        d["verdict"] = "FAIL" if "critical" in worst else "CAUTION" if "warning" in worst else "PASS"
    else:
        d["score"], d["verdict"] = None, "REJECTED" if not d.get("is_part") else "UNKNOWN"
    return d