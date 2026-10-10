import os
from dotenv import load_dotenv
from google import genai

load_dotenv()
client = genai.Client(api_key=os.getenv("GEMINI_API_KEY"))
for m in client.models.list():
    acts = getattr(m, "supported_actions", None) or []
    if not acts or "generateContent" in acts:
        print(m.name.replace("models/", ""))