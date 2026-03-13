import serial
import wave
import struct

PORT = '/dev/ttyUSB0' 
BAUD = 115200
SAMPLE_RATE = 16000
OUTPUT_FILE = 'record.wav'

try:
    ser = serial.Serial(PORT, BAUD)
except Exception as e:
    print(f"Failed to open port {PORT}: {e}")
    exit()

recording = False
audio_data = []

print("========================================")
print("Waiting for audio trigger...")
print("Speak into the mic. Watch the volume levels below!")
print("========================================")

while True:
    try:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
    except Exception:
        continue
    
    if not line:
        continue

    if "VOICE DETECTED" in line:
        print("\n" + "🔴"*10)
        print("🔴 TRIGGERED! RECORDING IN PROGRESS...")
        print("🔴 SPEAK NOW!!!")
        print("🔴"*10 + "\n")
        
    elif "Current Max Volume" in line:
        print(f"[Live Volume] {line}")
        
    elif line == "---WAV_START---":
        print("⏳ Recording finished. Downloading data to PC (this takes time)...")
        recording = True
        audio_data = []
        
    elif line == "---WAV_END---":
        print(f"💾 Saving to {OUTPUT_FILE}...")
        with wave.open(OUTPUT_FILE, 'w') as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(SAMPLE_RATE)
            
            for sample in audio_data:
                wav_file.writeframesraw(struct.pack('<h', sample))
        print("✅ Done! You can play the file now.")
        print("\n========================================")
        print("Waiting for next trigger...")
        print("========================================")
        recording = False
        
    elif recording:
        try:
            val = int(line)
            val = max(-32768, min(32767, val))
            audio_data.append(val)
        except ValueError:
            pass
    else:
        print(f"[ESP32 RAW]: {line}")