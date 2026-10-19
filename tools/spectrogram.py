# Render a spectrogram of a 16-bit stereo WAV (left channel) to PNG.
# usage: spectrogram.py in.wav out.png [start_s] [dur_s]
import sys, wave
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
w = wave.open(sys.argv[1])
rate = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)[:, 0].astype(float)
start = float(sys.argv[3]) if len(sys.argv) > 3 else 0
dur = float(sys.argv[4]) if len(sys.argv) > 4 else len(x) / rate
x = x[int(start * rate):int((start + dur) * rate)]
plt.figure(figsize=(12, 5))
plt.specgram(x, NFFT=2048, Fs=rate, noverlap=1536, cmap="magma", vmin=-20)
plt.ylim(0, 8000)
plt.xlabel("s"); plt.ylabel("Hz"); plt.title(f"{sys.argv[1]} (L), {start:.0f}-{start + dur:.0f} s")
plt.tight_layout(); plt.savefig(sys.argv[2], dpi=80)
