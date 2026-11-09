#!/usr/bin/env python3
"""
prepare_data.py
Завантажує мініфіковані бібліотеки графіків (Chart.js та адаптер date-fns),
стискає їх за допомогою gzip та зберігає у папку data/ для подальшої прошивки у LittleFS.
"""

import os
import gzip
import urllib.request

CHART_JS_URL = "https://cdn.jsdelivr.net/npm/chart.js@4.4.3/dist/chart.umd.min.js"
ADAPTER_JS_URL = "https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js"

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")

def download_and_gzip(url, output_gz_path):
    print(f"Завантаження {url} ...")
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    with urllib.request.urlopen(req, timeout=30) as response:
        content = response.read()
    
    print(f"Розмір оригінального файлу: {len(content):,} байт")
    gz_content = gzip.compress(content, compresslevel=9)
    print(f"Розмір після gzip стиснення: {len(gz_content):,} байт")
    
    with open(output_gz_path, "wb") as f:
        f.write(gz_content)
    print(f"Успішно збережено: {output_gz_path}\n")

def main():
    os.makedirs(DATA_DIR, exist_ok=True)
    
    chart_gz = os.path.join(DATA_DIR, "chart.js.gz")
    adapter_gz = os.path.join(DATA_DIR, "adapter.js.gz")
    
    download_and_gzip(CHART_JS_URL, chart_gz)
    download_and_gzip(ADAPTER_JS_URL, adapter_gz)
    
    print("Всі статичні ресурси успішно підготовлені у папці data/")

if __name__ == "__main__":
    main()
