"""PDF inspection report (ReportLab)."""
from datetime import datetime
from io import BytesIO
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import Image, Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle

GREEN, AMBER, RED, DARK = colors.HexColor("#00a86b"), colors.HexColor("#d98a00"), colors.HexColor("#d63050"), colors.HexColor("#0b1120")
ST = {"ok": ("OK", GREEN), "warning": ("WARNING", AMBER), "critical": ("CRITICAL", RED)}


def build(path, res, img_bytes, qual, sensors=None):
    s = getSampleStyleSheet()
    h = ParagraphStyle("h", parent=s["Title"], textColor=DARK, fontSize=20, alignment=0)
    h2 = ParagraphStyle("h2", parent=s["Heading2"], textColor=DARK, fontSize=12, spaceBefore=10)
    n = ParagraphStyle("n", parent=s["Normal"], fontSize=9.5, leading=13)
    doc = SimpleDocTemplate(path, pagesize=A4, leftMargin=18*mm, rightMargin=18*mm, topMargin=16*mm, bottomMargin=16*mm,
                            title="VITAL Inspection Report")
    e = [Paragraph("VITAL Vehicle Inspection Report", h),
         Paragraph("Vehicle Inspection Through AI and Live Sensors", n),
         Paragraph(datetime.now().strftime("Generated: %d %b %Y, %H:%M"), n), Spacer(1, 8)]
    verdict, score = res["verdict"], res["score"]
    vc = {"PASS": GREEN, "CAUTION": AMBER, "FAIL": RED}.get(verdict, colors.grey)
    top = Table([[f"Part: {res['part_label']}", f"Score: {score if score is not None else '--'}/100", verdict]],
                colWidths=[75*mm, 50*mm, 45*mm])
    top.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, -1), colors.HexColor("#eef2f7")),
                             ("TEXTCOLOR", (2, 0), (2, 0), vc), ("FONTNAME", (0, 0), (-1, -1), "Helvetica-Bold"),
                             ("FONTSIZE", (0, 0), (-1, -1), 11), ("PADDING", (0, 0), (-1, -1), 9)]))
    e += [top, Spacer(1, 8)]
    try:
        im = Image(BytesIO(img_bytes)); r = min(80*mm / im.drawWidth, 70*mm / im.drawHeight, 1)
        im.drawWidth *= r; im.drawHeight *= r; im.hAlign = "LEFT"; e += [im, Spacer(1, 4)]
    except Exception:
        pass
    e.append(Paragraph(f"<b>AI detected:</b> {res.get('detected', '-')}", n))
    e.append(Paragraph("Findings", h2))
    rows = [["Item", "Status", "Detail"]]
    for f in res["findings"]:
        t, c = ST[f["status"]]
        rows.append([Paragraph(f["item"], n), Paragraph(f'<font color="{c.hexval()}"><b>{t}</b></font>', n), Paragraph(f["detail"], n)])
    if len(rows) == 1:
        rows.append(["-", "-", "No findings available"])
    t = Table(rows, colWidths=[40*mm, 25*mm, 105*mm], repeatRows=1)
    t.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, 0), DARK), ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                           ("GRID", (0, 0), (-1, -1), .4, colors.HexColor("#c5cfdd")), ("VALIGN", (0, 0), (-1, -1), "TOP"),
                           ("FONTSIZE", (0, 0), (-1, -1), 9)]))
    e += [t, Paragraph("Recommended action", h2), Paragraph(res.get("advice", "-"), n)]
    if sensors and sensors.get("cells") is not None:
        e.append(Paragraph("Live sensor snapshot (ESP32-S3)", h2))
        f = lambda v, u="", d=1: "--" if v is None else f"{v:.{d}f}{u}"
        join = lambda xs, d=2: " / ".join(f(x, "", d) for x in xs)
        rows = [["Pack voltage", f(sensors.get("pack"), " V", 2)], ["Cell voltages (V)", join(sensors.get("cells", []))],
                ["Temperatures T1/T2/T3 (C)", join(sensors.get("temps", []), 1)], ["Current", f(sensors.get("current_ma"), " mA")],
                ["Vibration", f(sensors.get("vib"), " m/s2", 2)], ["Noise", f(sensors.get("noise_db"), " dB")],
                ["Oil purity", f(sensors.get("oil_pct"), " %")], ["Metal debris", "DETECTED" if sensors.get("metal") else "Not detected"],
                ["Insulation", "OK" if sensors.get("insulation") else "FAIL"]]
        st = Table(rows, colWidths=[60*mm, 110*mm])
        st.setStyle(TableStyle([("GRID", (0, 0), (-1, -1), .4, colors.HexColor("#c5cfdd")), ("BACKGROUND", (0, 0), (0, -1), colors.HexColor("#eef2f7")),
                                ("FONTSIZE", (0, 0), (-1, -1), 9)]))
        e.append(st)
    e.append(Paragraph("Image quality (OpenCV)", h2))
    e.append(Paragraph(f"Resolution {qual['width']}x{qual['height']} | Sharpness {qual['sharpness']} | Brightness {qual['brightness']}", n))
    e += [Spacer(1, 10), Paragraph("<i>AI-assisted visual check from a single photo. It does not replace a mechanic's inspection.</i>", n)]
    doc.build(e)
