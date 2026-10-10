@echo off
cd /d %~dp0
if not exist venv (python -m venv venv)
call venv\Scripts\activate
pip install -r requirements.txt -q
start http://localhost:5000
python app.py
