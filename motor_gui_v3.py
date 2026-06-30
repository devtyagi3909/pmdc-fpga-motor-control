"""
motor_gui_v3.py — PMDC Motor Live Dashboard (Perfectly Scaled ILA View)
"""

import socket
import struct
import tkinter as tk
from tkinter import ttk
from collections import deque
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

# ── Connection ──────────────────────────────────────────────────────────────
HOST = "192.168.1.10"   
PORT = 7

# ── Protocol constants ──────────────────────────────────────────────────────
SAMPLES_PER_PACKET = 100
BYTES_PER_PACKET   = SAMPLES_PER_PACKET * 4    
FX_SCALE           = 4096.0                    
RPM_PER_RADS       = 9.5493
TS                 = 10e-6                     

# ── Display (DOWNSAMPLED FOR WIDE ILA VIEW) ─────────────────────────────────
DISPLAY_TIME_S = 1.5       # Show 1.5 seconds of history on screen
DOWNSAMPLE     = 100       # Draw 1 out of every 100 samples to prevent GUI freeze
WINDOW         = int((DISPLAY_TIME_S / TS) / DOWNSAMPLE) 
UPDATE_MS      = 30

ia_buf = deque([0.0] * WINDOW, maxlen=WINDOW)   
w_buf  = deque([0.0] * WINDOW, maxlen=WINDOW)   

# ── Control byte bit positions ─────────────────────────────────────────────
BIT_RESET_N = 0x01
BIT_PWMA    = 0x02
BIT_PWMB    = 0x04
BIT_PWMEN   = 0x08
BIT_TL0     = 0x10
BIT_TL1     = 0x20
BIT_TL2     = 0x40

TORQUE_PRESETS = [
    (0b000, 0.00,  "0.00 N·m  (no load)"),
    (0b001, 0.05,  "+0.05 N·m"),
    (0b010, 0.10,  "+0.10 N·m"),
    (0b011, 0.15,  "+0.15 N·m"),
    (0b100, -0.05, "-0.05 N·m"),
    (0b101, -0.10, "-0.10 N·m"),
    (0b110, -0.15, "-0.15 N·m"),
    (0b111, 0.00,  "0.00 N·m  (alt)"),
]

# ── Socket ──────────────────────────────────────────────────────────────────
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))
sock.setblocking(False)
current_ctrl = 0x00   

# ── Root window ──────────────────────────────────────────────────────────────
root = tk.Tk()
root.title("PMDC Motor v3 — Live Dashboard (Perfectly Scaled)")

main_frame = tk.Frame(root)
main_frame.pack(fill=tk.BOTH, expand=True)

plot_frame = tk.Frame(main_frame)
plot_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

ctrl_frame = tk.LabelFrame(main_frame, text="Motor Control", padx=10, pady=10)
ctrl_frame.pack(side=tk.RIGHT, fill=tk.Y, padx=10, pady=10)

# ── Plots (HARDCODED PERFECT SCALES) ────────────────────────────────────────
fig = Figure(figsize=(9, 6), dpi=100)
ax_ia = fig.add_subplot(211)
ax_w  = fig.add_subplot(212)

# X axis is now in true seconds
t_axis = [i * (TS * DOWNSAMPLE) for i in range(WINDOW)]   

line_ia, = ax_ia.plot(t_axis, list(ia_buf), linewidth=1.5, color='#1f77b4')
line_w,  = ax_w.plot(t_axis, list(w_buf),  linewidth=1.5, color='#2ca02c')

# PERMANENT ILA-STYLE Y-AXIS LIMITS (No Auto-Scale Needed)
ax_ia.set_ylim(-0.2, 1.0)
ax_ia.set_xlim(0, t_axis[-1])
ax_ia.set_ylabel("Current ia (A)")
ax_ia.set_title("Armature Current — Inrush Transient (sfix16_En12)")
ax_ia.grid(True, alpha=0.3)

ax_w.set_ylim(-1.0, 6.0)
ax_w.set_xlim(0, t_axis[-1])
ax_w.set_ylabel("Speed ω (rad/s)")
ax_w.set_xlabel("Time window (Seconds)")
ax_w.set_title("Angular Speed Ramp-Up")
ax_w.grid(True, alpha=0.3)

fig.tight_layout()
canvas = FigureCanvasTkAgg(fig, master=plot_frame)
canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
canvas.draw()

# ── Pause / Resume ──────────────────────────────────────────────────────────
running = True
def toggle_pause():
    global running
    running = not running
    btn_pause.config(text="Resume" if not running else "Pause")

bottom_bar = tk.Frame(plot_frame)
bottom_bar.pack(fill=tk.X, pady=4)
btn_pause = tk.Button(bottom_bar, text="Pause", command=toggle_pause, width=10)
btn_pause.pack(side=tk.LEFT, padx=4)

# Removed the auto-scale button to prevent accidental clicks causing DMA Overruns

# ── High-precision live readout ─────────────────────────────────────────────
readout_frame = tk.LabelFrame(ctrl_frame, text="Live Readout (fixed-point, /4096)", padx=8, pady=8)
readout_frame.pack(fill=tk.X, pady=4)

ia_raw_var = tk.StringVar(value="ia_raw = 0")
ia_val_var = tk.StringVar(value="ia     = 0.00000000 A")
w_raw_var  = tk.StringVar(value="w_raw  = 0")
w_val_var  = tk.StringVar(value="w      = 0.00000000 rad/s")
rpm_var    = tk.StringVar(value="rpm    = 0.00000")

for var, fg in [(ia_raw_var, "#888"), (ia_val_var, "#1f77b4"),
                 (w_raw_var, "#888"),  (w_val_var, "#2ca02c"),
                 (rpm_var,  "#2ca02c")]:
    tk.Label(readout_frame, textvariable=var, font=("Courier", 10, "bold"),
             fg=fg, anchor="w").pack(fill=tk.X)

# ── Drive control ────────────────────────────────────────────────────────────
drive_frame = tk.LabelFrame(ctrl_frame, text="Drive (pwma/pwmb/pwmen)", padx=8, pady=8)
drive_frame.pack(fill=tk.X, pady=4)

status_var = tk.StringVar(value="Not connected")

def send_ctrl():
    try:
        sock.sendall(struct.pack('B', current_ctrl & 0x7F))
        status_var.set(f"Sent ctrl=0x{current_ctrl:02X}")
    except Exception as e:
        status_var.set(f"Send error: {e}")

def set_drive(forward=False, reverse=False, coast=False, stop=False):
    global current_ctrl
    if stop:
        current_ctrl &= ~(BIT_RESET_N | BIT_PWMA | BIT_PWMB | BIT_PWMEN)
    elif coast:
        current_ctrl |= BIT_RESET_N
        current_ctrl &= ~(BIT_PWMA | BIT_PWMB | BIT_PWMEN)
    elif forward:
        current_ctrl |= (BIT_RESET_N | BIT_PWMEN | BIT_PWMA)
        current_ctrl &= ~BIT_PWMB
    elif reverse:
        current_ctrl |= (BIT_RESET_N | BIT_PWMEN | BIT_PWMB)
        current_ctrl &= ~BIT_PWMA
    send_ctrl()

btn_cfg = {"font": ("Arial", 10, "bold"), "width": 16, "pady": 3}

tk.Button(drive_frame, text="▶  Forward",
          command=lambda: set_drive(forward=True),
          bg="#2196F3", fg="white", **btn_cfg).pack(pady=3)
tk.Button(drive_frame, text="◀  Reverse",
          command=lambda: set_drive(reverse=True),
          bg="#9C27B0", fg="white", **btn_cfg).pack(pady=3)
tk.Button(drive_frame, text="∅  Coast (PWM off)",
          command=lambda: set_drive(coast=True),
          bg="#FF9800", fg="white", **btn_cfg).pack(pady=3)
tk.Button(drive_frame, text="■  Stop (Reset)",
          command=lambda: set_drive(stop=True),
          bg="#F44336", fg="white", **btn_cfg).pack(pady=3)

# ── Load torque control ──────────────────────────────────────────────────────
torque_frame = tk.LabelFrame(ctrl_frame, text="Load Torque (Tl2,Tl1,Tl0)", padx=8, pady=8)
torque_frame.pack(fill=tk.X, pady=4)

torque_var = tk.StringVar(value=TORQUE_PRESETS[0][2])

def set_torque(idx):
    global current_ctrl
    code, value, label = TORQUE_PRESETS[idx]
    current_ctrl &= ~(BIT_TL0 | BIT_TL1 | BIT_TL2)
    if (code >> 0) & 1: current_ctrl |= BIT_TL0
    if (code >> 1) & 1: current_ctrl |= BIT_TL1
    if (code >> 2) & 1: current_ctrl |= BIT_TL2
    torque_var.set(label)
    send_ctrl()

torque_dropdown = ttk.Combobox(
    torque_frame, values=[p[2] for p in TORQUE_PRESETS],
    state="readonly", width=18)
torque_dropdown.current(0)
torque_dropdown.pack(pady=2)
torque_dropdown.bind("<<ComboboxSelected>>", lambda e: set_torque(torque_dropdown.current()))
tk.Label(torque_frame, textvariable=torque_var, font=("Courier", 9), fg="#555").pack(pady=2)

# ── Manual control byte entry ───────────────────────────────────────────────
manual_frame = tk.LabelFrame(ctrl_frame, text="Manual Control Byte (hex)", padx=8, pady=8)
manual_frame.pack(fill=tk.X, pady=4)
manual_var = tk.StringVar(value="00")
manual_entry = tk.Entry(manual_frame, textvariable=manual_var, width=8, font=("Courier", 11))
manual_entry.pack(side=tk.LEFT, padx=2)

def send_manual(event=None):
    global current_ctrl
    try:
        current_ctrl = int(manual_var.get(), 16) & 0x7F
        send_ctrl()
    except ValueError:
        status_var.set("Invalid hex")

tk.Button(manual_frame, text="Send", command=send_manual, width=8).pack(side=tk.LEFT, padx=2)
manual_entry.bind("<Return>", send_manual)
tk.Label(manual_frame, text="bit:6543210\nTl2 Tl1 Tl0 en b pwmb pwma rst_n", font=("Courier", 7), fg="#888", justify=tk.LEFT).pack(anchor="w")

tk.Frame(ctrl_frame, height=2, bg="grey").pack(fill=tk.X, pady=8)
tk.Label(ctrl_frame, textvariable=status_var, wraplength=190, fg="blue", font=("Arial", 9)).pack()

# ── Receive loop ────────────────────────────────────────────────────────────
recv_raw = bytearray()
downsample_counter = 0

def signed16(v):
    return v - 65536 if v >= 32768 else v

def update_scope():
    global recv_raw, downsample_counter

    if running:
        # Replaced while loop with a bounded for loop for safety and stability
        for _ in range(100):
            try:
                chunk = sock.recv(65536)
            except BlockingIOError:
                break
            if not chunk:
                break
            recv_raw.extend(chunk)

        mv = memoryview(recv_raw)
        off = 0
        ia_tick = []
        w_tick = []
        
        ia_last_val = 0.0
        w_last_val = 0.0
        ia_raw_last = 0
        w_raw_last = 0

        # Extract complete packets
        total_complete_packets = (len(recv_raw) - off) // BYTES_PER_PACKET
        
        for _ in range(total_complete_packets):
            samples = struct.unpack_from("<100I", mv, off)
            off += BYTES_PER_PACKET
            
            for s in samples:
                ia_raw_last = signed16(s & 0xFFFF)
                w_raw_last  = signed16((s >> 16) & 0xFFFF)
                ia_last_val = ia_raw_last / FX_SCALE
                w_last_val  = w_raw_last / FX_SCALE
                
                # Downsample logic: only plot 1 out of every DOWNSAMPLE points
                downsample_counter += 1
                if downsample_counter >= DOWNSAMPLE:
                    downsample_counter = 0
                    ia_tick.append(ia_last_val)
                    w_tick.append(w_last_val)

        mv.release()
        if off:
            del recv_raw[:off]

        # Update the live plot if we collected new visual points
        if ia_tick:
            ia_buf.extend(ia_tick)
            w_buf.extend(w_tick)

            line_ia.set_ydata(list(ia_buf))
            line_w.set_ydata(list(w_buf))

            ia_raw_var.set(f"ia_raw = {ia_raw_last}")
            ia_val_var.set(f"ia     = {ia_last_val:+.8f} A")
            w_raw_var.set(f"w_raw  = {w_raw_last}")
            w_val_var.set(f"w      = {w_last_val:+.8f} rad/s")
            rpm_var.set(f"rpm    = {w_last_val * RPM_PER_RADS:+.5f}")

            canvas.draw_idle()

    root.after(UPDATE_MS, update_scope)

status_var.set(f"Connected to {HOST}:{PORT}")
update_scope()
root.mainloop()