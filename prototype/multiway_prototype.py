import numpy as np
import matplotlib.pyplot as plt
from scipy.io import wavfile



rate, data = wavfile.read('input.wav')
data_normalized = data / np.max(np.abs(data))
time = np.arange(data_normalized.shape[0]) / rate

if data_normalized.ndim > 1:
    data_normalized = data_normalized[:, 0]
    
fft_size = 2048
hop = fft_size // 4        
window = np.hanning(fft_size + 1)[:-1]   
window = np.sqrt(window)     

num_frames = 1 + (len(data_normalized) - fft_size) // hop
output = np.zeros(len(data_normalized))
window_sum = np.zeros(len(data_normalized))   

freqs = np.fft.rfftfreq(fft_size, d=1/rate)
num_bins = len(freqs)

delay_seconds = np.full(num_bins, 0.15)  

delay_frames_float = delay_seconds * rate / hop
frame_offset = np.floor(delay_frames_float).astype(int)

max_delay_frames = int(frame_offset.max()) + 1
history = np.zeros((max_delay_frames, num_bins), dtype=complex)

for i in range(num_frames):
    start = i * hop
    frame = data_normalized[start : start + fft_size] * window
    spectrum = np.fft.rfft(frame)
    
    history[i % max_delay_frames] = spectrum

    delayed_spectrum = np.zeros(num_bins, dtype=complex)
    for k in range(num_bins):
        source_frame = i - frame_offset[k]
        if source_frame >= 0:
            delayed_spectrum[k] = history[source_frame % max_delay_frames, k]

    spectrum = delayed_spectrum
    
    
    
    out_frame = np.fft.irfft(spectrum) * window
    output[start : start + fft_size] += out_frame
    window_sum[start : start + fft_size] += window ** 2
    
output /= np.maximum(window_sum, 1e-8)


plt.figure()
plt.plot(window_sum)
plt.title("Window sum (COLA check)")
plt.show()


plt.figure()
plt.plot(data_normalized[:5000], label="input")
plt.plot(output[:5000], label="output", alpha=0.7)
plt.legend()
plt.title("Input vs Output (first 5000 samples)")
plt.show()

start_idx = 50000
end_idx = 55000
plt.figure()
plt.plot(data_normalized[start_idx:end_idx], label="input")
plt.plot(output[start_idx:end_idx], label="output", alpha=0.7)
plt.legend()
plt.title(f"Input vs Output (samples {start_idx}-{end_idx})")
plt.show()

wavfile.write("output_test.wav", rate, output.astype(np.float32))
    

