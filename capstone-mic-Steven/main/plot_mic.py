import matplotlib
matplotlib.use('TkAgg') # Force the interactive Tkinter backend to avoid "FigureCanvasAgg" error

import serial
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import numpy as np

# --- Configuration Parameters ---
PORT = '/dev/ttyUSB0'  # Your ESP32 serial port (e.g., 'COM3' on Windows, '/dev/ttyACM0' on Linux/WSL)
BAUD = 115200          # Baud rate matching your ESP32 configuration
FFT_BINS = 127         # We output 128 - 1 = 127 frequency bins from ESP32
FREQ_RES = 31.25       # Frequency resolution (32000Hz / 1024 = 31.25Hz)

# Initialize serial connection
try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print(f"Failed to open serial port {PORT}: {e}")
    exit()

# Initialize plot
fig, ax = plt.subplots(figsize=(10, 6))

# X-axis: Calculate the actual frequency for each bin
x_data = np.arange(1, FFT_BINS + 1) * FREQ_RES 
y_data = np.zeros(FFT_BINS)

# Create the line plot
line, = ax.plot(x_data, y_data, '-', color='blue', linewidth=2)

# Set axis limits and labels
ax.set_ylim(0, 5000)  # Y-axis amplitude range (increase if the wave clips the top)
ax.set_xlim(0, 4000)  # X-axis frequency range (0 - 4000Hz covers human voice)
ax.set_title("Real-time Human Voice Frequency Spectrum")
ax.set_xlabel("Frequency (Hz)")
ax.set_ylabel("Amplitude")
ax.grid(True, linestyle='--', alpha=0.6)

def update(frame):
    try:
        # Read a line of serial data
        raw_line = ser.readline().decode('utf-8').strip()
        
        # Filter for our defined data packet header
        if raw_line.startswith("FFT_DATA:"):
            # Strip the header and split by comma
            # [:-1] removes the empty string at the end caused by the trailing comma in C code
            data_str = raw_line.replace("FFT_DATA:", "").split(',')[:-1] 
            
            # Ensure we received the correct number of data points
            if len(data_str) == FFT_BINS:
                # Convert strings to float array
                new_y_data = np.array([float(val) for val in data_str])
                
                # Update the plot data
                line.set_ydata(new_y_data)
                
    except Exception as e:
        # Ignore occasional serial reading errors (e.g., incomplete lines during startup)
        pass 
        
    return line,

# Use FuncAnimation for high frame rate rendering
# Added cache_frame_data=False to silence the caching warning
ani = animation.FuncAnimation(fig, update, interval=20, blit=True, cache_frame_data=False)

plt.show()