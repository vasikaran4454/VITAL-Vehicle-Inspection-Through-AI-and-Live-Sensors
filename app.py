import json, os, re, uuid
from flask import Flask, jsonify, render_template, request, send_file
try:
    from dotenv import load_dotenv; load_dotenv()
except ImportError:
    pass
from analyzer import cv_metrics, report, vision

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 10 * 1024 * 1024
REPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports")
os.makedirs(REPORTS, exist_ok=True)


@app.get("/")
def index():
    return render_template("index.html")


@app.post("/api/analyze")
def analyze():
    f = request.files.get("image")
    part = request.form.get("part", "tyre")
    if not f:
        return jsonify(error="Photo upload pannunga"), 400
    data = f.read()
    try:
        qual = cv_metrics.quality(cv_metrics.decode(data))
        if not qual["ok"]:
            return jsonify(error="Photo quality sari illa: " + "; ".join(qual["notes"])), 422
        mt = f.mimetype if f.mimetype in ("image/jpeg", "image/png", "image/webp") else "image/jpeg"
        res = vision.analyze(data, mt, part)
    except vision.VisionUnavailable as e:
        return jsonify(error=str(e)), 503
    except ValueError as e:
        return jsonify(error=str(e)), 400
    except Exception as e:
        return jsonify(error=f"AI analysis fail: {e}"), 502
    sensors = None
    try:
        sensors = json.loads(request.form.get("sensors", "")) or None
    except ValueError:
        pass
    res["id"] = uuid.uuid4().hex[:12]
    res["quality"] = qual
    if res["verdict"] != "REJECTED":
        report.build(os.path.join(REPORTS, res["id"] + ".pdf"), res, data, qual, sensors)
        res["pdf"] = f"/api/report/{res['id']}"
    return jsonify(res)


@app.get("/api/report/<rid>")
def get_report(rid):
    if not re.fullmatch(r"[0-9a-f]{12}", rid):
        return jsonify(error="bad id"), 400
    p = os.path.join(REPORTS, rid + ".pdf")
    if not os.path.exists(p):
        return jsonify(error="not found"), 404
    return send_file(p, mimetype="application/pdf", download_name=f"VITAL_report_{rid}.pdf")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
