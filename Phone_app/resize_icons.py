"""
Resize the AQM icon to all PWA sizes.
Run: python resize_icons.py
Requires: pip install Pillow
"""
from PIL import Image
import shutil, os

SRC = r"C:\Users\parth\.gemini\antigravity-ide\brain\7ca44672-b10f-4ea5-a930-6e39fe828fe5\aqm_app_icon_1786354745484.png"
OUT = r"C:\Users\parth\Desktop\AQM\Phone_app\icons"

sizes = {
    "icon-192.png":          192,
    "icon-192-maskable.png": 192,
    "icon-512.png":          512,
    "icon-512-maskable.png": 512,
}

img = Image.open(SRC).convert("RGBA")
for filename, size in sizes.items():
    out_path = os.path.join(OUT, filename)
    resized = img.resize((size, size), Image.LANCZOS)
    resized.save(out_path, "PNG")
    print(f"Saved {out_path} ({size}x{size})")

print("\nAll icons generated!")
