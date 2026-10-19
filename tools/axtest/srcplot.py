# Spectrograms + image energy of the SRC test WAVs written by axtest.c.
# usage: srcplot.py <dir with axtest WAVs> <out.png>
import sys, wave, numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt
d, out = sys.argv[1], sys.argv[2]
names = ["up_linear", "up_poly0", "up_poly2", "down_linear", "down_poly0", "down_poly1"]
titles = ["ratio 0.5, linear", "ratio 0.5, polyphase coef 0 (8 kHz)", "ratio 0.5, polyphase coef 2 (16 kHz)",
          "ratio 1.5, linear", "ratio 1.5, polyphase coef 0", "ratio 1.5, polyphase coef 1 (12 kHz)"]
fig, ax = plt.subplots(2, 3, figsize=(15, 7))
for a, n, t in zip(ax.flat, names, titles):
    w = wave.open(f"{d}/{n}.wav"); x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)[:, 0].astype(float)
    a.specgram(x, NFFT=1024, Fs=32000, noverlap=768, cmap="magma", vmin=-20, vmax=80)
    a.set_title(t, fontsize=10); a.set_ylim(0, 16000)
plt.suptitle("AX SRC test: log sine sweep 50 Hz-15.9 kHz (source at 32 kHz) resampled by dsp_ax.c; images/aliases are the faint mirrored curves")
plt.tight_layout(); plt.savefig(out, dpi=70)
# upsampling by 2: a 32 kHz source played at half rate has no content above 8 kHz, so everything there is an image
for n in names[:3]:
    w = wave.open(f"{d}/{n}.wav"); x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)[:, 0].astype(float)
    X = np.abs(np.fft.rfft(x)) ** 2; f = np.fft.rfftfreq(len(x), 1 / 32000)
    print(f"{n}: image energy above 8 kHz = {10 * np.log10(X[f > 8000].sum() / X.sum()):.1f} dB rel. total")
