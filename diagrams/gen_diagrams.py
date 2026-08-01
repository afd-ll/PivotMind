#!/usr/bin/env python3
"""diagrams/gen_diagrams.py — 用 PIL 绘制架构/管道图（4x 超采样抗锯齿，矢量级清晰 PNG）
用法: python3 gen_diagrams.py   （生成 brain-regions.png + multimodal-pipeline.png）"""
from PIL import Image, ImageDraw, ImageFont
import math, os

OUT = os.path.dirname(os.path.abspath(__file__))
SS = 4  # 超采样倍数

def new_canvas(w, h):
    return Image.new("RGB", (w * SS, h * SS), "#ffffff")

def font(sz):
    try:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", sz * SS)
    except Exception:
        return ImageFont.load_default()

def save(img, name):
    img = img.resize((img.width // SS, img.height // SS), Image.LANCZOS)
    img.save(os.path.join(OUT, name))
    print(f"生成 {name} ({img.width}x{img.height})")

def box(d, x, y, w, h, label, sub, fill="#eef3fb", border="#3b6ea5"):
    d.rounded_rectangle([x * SS, y * SS, (x + w) * SS, (y + h) * SS],
                        radius=10 * SS, fill=fill, outline=border, width=2 * SS)
    f = font(15); fs = font(11)
    tw = d.textlength(label, font=f)
    d.text(((x + w / 2) * SS - tw / 2, (y + h / 2 - 14) * SS), label, fill="#1a2a44", font=f)
    if sub:
        sw = d.textlength(sub, font=fs)
        d.text(((x + w / 2) * SS - sw / 2, (y + h / 2 + 6) * SS), sub, fill="#5a6a85", font=fs)

def arrow(d, x1, y1, x2, y2, color="#3b6ea5"):
    d.line([x1 * SS, y1 * SS, x2 * SS, y2 * SS], fill=color, width=2 * SS)
    ang = math.atan2(y2 - y1, x2 - x1)
    for da in (0.4, -0.4):
        d.line([x2 * SS, y2 * SS,
                (x2 - 14 * math.cos(ang - da)) * SS, (y2 - 14 * math.sin(ang - da)) * SS],
               fill=color, width=2 * SS)

# ═══════════ 图 1：脑区架构（Thalamus 中心 + 14 脑区环形） ═══════════
W, H = 1600, 1050
img = new_canvas(W, H)
d = ImageDraw.Draw(img)
cx, cy, R = W / 2, H / 2, 400

regions = [
    ("Prefrontal Cortex", "Dialog / Decision Entry"),
    ("Prefrontal Exec", "6-Mode Reasoning"),
    ("Hippocampus", "Memory Consolidation"),
    ("DMN", "Dream / Idle"),
    ("Amygdala", "Emotion"),
    ("Perception", "Web Search"),
    ("Broca", "Template Builder"),
    ("Cerebellum", "BPTT / Protect"),
    ("Brainstem", "Circadian Clock"),
    ("Hypothalamus", "Drives"),
    ("ACC", "4D Evaluation"),
    ("IdeaArena", "Candidate Competition"),
    ("Reticular", "Arousal"),
    ("VisualCortex", "Multi-Modal Pipeline"),
]
BW, BH = 230, 78
pos = []
for i, (name, sub) in enumerate(regions):
    ang = -math.pi / 2 + i * 2 * math.pi / len(regions)   # 从顶部顺时针
    bx = cx + R * math.cos(ang) - BW / 2
    by = cy + R * math.sin(ang) - BH / 2
    pos.append((bx + BW / 2, by + BH / 2))
    box(d, bx, by, BW, BH, name, sub)

# Thalamus 中心
box(d, cx - 120, cy - 55, 240, 110, "Thalamus", "Signal Bus + Resource Gate", fill="#fdf3e3", border="#c9a227")
for (px, py) in pos:
    arrow(d, px, py, cx, cy - 10 if py < cy else cy + 10)

save(img, "brain-regions.png")

# ═══════════ 图 2：多模态管道（Pipeline A/B → 拓扑网络） ═══════════
W2, H2 = 1500, 760
img2 = new_canvas(W2, H2)
d2 = ImageDraw.Draw(img2)
# 标题
f = font(17)
t = "Multi-Modal Pipeline"
tw = d2.textlength(t, font=f)
d2.text(((W2 / 2) * SS - tw / 2, 20 * SS), t, fill="#1a2a44", font=f)

def row(y0, items, title):
    f2 = font(13)
    tw2 = d2.textlength(title, font=f2)
    d2.text((60 * SS, (y0 - 26) * SS), title, fill="#c9a227", font=f2)
    xs = [60]
    for i, (name, sub) in enumerate(items):
        x = xs[-1] if i == 0 else xs[-1] + 20
        box(d2, x, y0, 175, 62, name, sub)
        xs.append(x + 175)
        if i < len(items) - 1:
            arrow(d2, x + 175, y0 + 31, x + 175 + 20, y0 + 31)
    return xs

# Pipeline A
a_end = row(110, [
    ("Video File", "subs"), ("ffprobe", "detect subs"), ("ffmpeg", "extract SRT"),
    ("SRT Parser", ""), ("article_process_line", "PMI"), ("Vocab Topology", "+edges"),
], "Pipeline A: Subtitle")
# Pipeline B 分两路
b1_end = row(250, [("Video File", "keyframes"), ("ffprobe", "keyframes"), ("512-dim", "Features")], "Pipeline B: Visual Cortex")
b2_x = 60
box(d2, b2_x, 390, 175, 62, "Video File", "SRT timestamps")
box(d2, b2_x + 195, 390, 175, 62, "ffmpeg", "timestamps")
arrow(d2, b2_x + 175, 421, b2_x + 195, 421)
# ALIGN
alx, aly = 460, 390
box(d2, alx, aly, 175, 62, "Time-window", "Alignment")
arrow(d2, b1_end[-1] - 175, 312, alx + 175, aly + 15)   # FEAT → ALIGN
arrow(d2, b2_x + 195 + 175, 421, alx, 421)              # timestamps → ALIGN
# CROSS
box(d2, alx + 195, aly, 185, 62, "Cross-Topology Edge", "vocab\u2194visual")
arrow(d2, alx + 175, 421, alx + 195, 421)
# NET 汇入
net_x = 620
box(d2, net_x, 540, 240, 70, "Topology Network", "", fill="#eefbf0", border="#2e8b57")
arrow(d2, a_end[-1] - 175 - 20 + 175, 172, net_x + 60, 540)   # Pipeline A TOPO1 → NET
arrow(d2, alx + 195 + 185, 421, net_x + 180, 540)             # CROSS → NET

save(img2, "multimodal-pipeline.png")
print("完成")
