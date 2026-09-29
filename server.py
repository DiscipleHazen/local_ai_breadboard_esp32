import os      # Standard Python OS library, used for environment variable manipulation.
import glob    # Used to search the filesystem for files matching a pattern (like regex for directories).

# ---------------------------------------------------------
# NVIDIA DLL PATH FIX FOR WINDOWS
# ---------------------------------------------------------
# Python 3.8+ changed how DLLs are loaded for security reasons. It no longer checks the PATH variable automatically.
# We search for the exact CUDA v12.6 folder and explicitly tell Python to load DLLs from there.
# This prevents "cublas64_12.dll not found" errors when trying to run AI models on the GPU.
cuda_paths = glob.glob(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin")
if cuda_paths:
    for path in cuda_paths:
        os.add_dll_directory(path)

import asyncio           # Core library for handling asynchronous operations (preventing blocking loops).
import websockets        # Library to create the WebSocket server to talk to the ESP32.
import numpy as np       # High-performance math library used to manipulate the raw audio byte arrays.
import requests          # Used to make HTTP POST requests to the local Ollama API.
import time              # Used for timing and rate-limiting console prints.
from faster_whisper import WhisperModel # Highly optimized implementation of OpenAI's Whisper (Speech-to-Text).
from openwakeword.model import Model    # Library for detecting custom wake words (like "Hey Jarvis").
import openwakeword                     # Main openwakeword library.

# ---------------------------------------------------------
# 1. AI MODEL CONFIGURATION
# ---------------------------------------------------------
print("Loading models into VRAM...")

# Loads the Whisper "base.en" model into the GPU (CUDA) and uses FP16 math to double inference speed and halve VRAM usage.
whisper_model = WhisperModel("base.en", device="cuda", compute_type="float16")

# Downloads required model files if missing, then loads the specific "hey_jarvis" wake word model.
openwakeword.utils.download_models()
oww_model = Model(wakeword_models=["hey_jarvis"])

# API endpoint for your local Ollama instance.
OLLAMA_URL = "http://localhost:11434/api/generate"
LLM_MODEL = "llama3.1:8b"                       # The specific text model we are targeting.

PIPER_EXE = "python"                            # The command line executable for Piper (TTS).
PIPER_MODEL = "en_US-bryce-medium.onnx"         # The specific voice model file Piper will use.

# ---------------------------------------------------------
# 2. WEBSOCKET SERVER LOGIC
# ---------------------------------------------------------
async def handle_client(websocket):             # Async function called every time an ESP32 connects.
    print("Arduino connected!")
    
    listening = False                           # Tracks if the wake word was heard and we are currently recording the command.
    is_busy = False                             # Tracks if the AI is currently processing/speaking (halts new recordings).
    audio_buffer = []                           # List to store the recorded speech after the wake word.
    frames_recorded = 0                         # Counter for how much audio we've recorded.
    RECORDING_LENGTH = 16000 * 4                # 16,000 samples per sec * 4 seconds = 64,000 samples (4 seconds total audio).
    
    oww_chunk_buffer = np.array([], dtype=np.int16) # Temporary buffer holding audio just for the wake word detector.
    OWW_CHUNK_SIZE = 1280                           # OpenWakeWord requires audio to be fed in exact chunks of 1280 samples.
    
    last_print_time = time.time()               # Used to throttle console output.

    def unlock():                               # Helper function to reset the system state after speaking finishes.
        nonlocal is_busy, oww_chunk_buffer
        is_busy = False                         # Opens the gate for new incoming audio.
        oww_chunk_buffer = np.array([], dtype=np.int16) # Flushes the wake word buffer.
        oww_model.reset()                       # CRITICAL: Wipes the AI's internal state so it doesn't falsely trigger again based on past audio.
        print("\nMic unlocked. Ready for next wake word.")

    try:
        async for message in websocket:         # Infinite async loop waiting for packets from the ESP32.
            if isinstance(message, bytes):      # Ensures the incoming packet is raw binary audio.
                if is_busy:                     # If the AI is talking, drop the packet (ignore the microphone).
                    continue

                # Converts the raw byte payload into an array of 16-bit integers.
                raw_data = np.frombuffer(message, dtype=np.int16)
                # Removes DC offset (centers the waveform around 0) to clean up microphone static.
                pcm_data = raw_data - np.mean(raw_data)
                pcm_data = pcm_data.astype(np.int16)
                
                # Calculates the loudest peak in this packet for our volume meter.
                volume = np.max(np.abs(pcm_data.astype(np.int32)))
                current_time = time.time()
                
                # If it's somewhat loud, and 0.1 seconds have passed, update the volume text in the terminal.
                # `end="\r"` rewrites the same line instead of spamming thousands of new lines.
                if volume > 100 and (current_time - last_print_time) > 0.1: 
                    print(f"Mic Volume: {volume}/32768   ", end="\r", flush=True)
                    last_print_time = current_time
                
                if not listening:               # IF WE ARE WAITING FOR THE WAKE WORD:
                    # Append the incoming audio to the wake word buffer.
                    oww_chunk_buffer = np.concatenate((oww_chunk_buffer, pcm_data))
                    
                    # Prevent the buffer from growing infinitely (cap it at 1 second of audio).
                    if len(oww_chunk_buffer) > 16000:
                        oww_chunk_buffer = oww_chunk_buffer[-OWW_CHUNK_SIZE:]
                    
                    # Process the buffer in exactly 1280-sample blocks as required by OpenWakeWord.
                    while len(oww_chunk_buffer) >= OWW_CHUNK_SIZE:
                        chunk_to_process = oww_chunk_buffer[:OWW_CHUNK_SIZE]
                        oww_chunk_buffer = oww_chunk_buffer[OWW_CHUNK_SIZE:]
                        
                        # Pass the chunk to the AI model. `to_thread` prevents the heavy AI math from freezing the WebSocket loop.
                        prediction = await asyncio.to_thread(oww_model.predict, chunk_to_process)
                        
                        wake_detected = False
                        # Loop through the results (in case you load multiple wake words).
                        for model_name, score in prediction.items():
                            if score > 0.02:    # Show partial detections in the console for debugging.
                                print(f"\n🔍 {model_name} Score: {score:.3f}   ", end="\r", flush=True)
                            
                            if score > 0.5:     # 0.5 is the confidence threshold. If above 50%, we heard "Hey Jarvis"!
                                print(f"\nWake word detected! ({model_name} - Score: {score:.2f})")
                                listening = True             # Flip the state to start recording.
                                frames_recorded = 0          # Reset the recording timer.
                                audio_buffer = []            # Clear the main audio buffer.
                                await websocket.send("Listening...") # Send text to ESP32 Display 1.
                                wake_detected = True
                                break
                                
                        if wake_detected:
                            oww_chunk_buffer = np.array([], dtype=np.int16) # Clear the buffer completely.
                            break
                else:                           # IF WE ARE ACTIVELY RECORDING THE USER'S COMMAND:
                    audio_buffer.extend(pcm_data)            # Append the audio to our main recording list.
                    frames_recorded += len(pcm_data)         # Track how many samples we've recorded.
                        
                    if frames_recorded >= RECORDING_LENGTH:  # Once we hit exactly 4 seconds of recorded audio...
                        listening = False                    # Stop recording.
                        is_busy = True                       # Lock the system so we can process.
                        await websocket.send("Thinking...")  # Update ESP32 Display 1.
                        audio_data = np.array(audio_buffer, dtype=np.int16) # Convert list to a numpy array.
                        # Fire off the heavy AI pipeline as a separate async background task so the WebSocket doesn't crash.
                        asyncio.create_task(process_ai_pipeline(websocket, unlock, audio_data))
                            
    except websockets.exceptions.ConnectionClosed:
        print("\nArduino disconnected.")

# ---------------------------------------------------------
# 3. THE AI PIPELINE (STT -> LLM -> TTS)
# ---------------------------------------------------------
async def process_ai_pipeline(websocket, unlock_callback, audio_data):
    
        print("\nProcessing audio through Whisper...")
        # Whisper requires audio normalized between -1.0 and 1.0. We divide by the 16-bit max (32768) to achieve this.
        audio_float32 = audio_data.astype(np.float32) / 32768.0
        
        # Runs transcription on a background thread. `beam_size=5` improves accuracy at the cost of slight speed.
        segments, _ = await asyncio.to_thread(
            whisper_model.transcribe, audio_float32, beam_size=5
        )
        # Combines the generator of text segments into one final string.
        user_text = "".join([segment.text for segment in segments]).strip()
        
        print(f"User said: {user_text}")
        if not user_text:                            # If Whisper heard nothing but silence/noise...
            await websocket.send("I didn't catch that.")
            unlock_callback()                        # Unlock the mic and abort.
            return

        print(f"Sending to Ollama ({LLM_MODEL})...")
        payload = {                                  # Format the JSON payload for the Ollama API.
            "model": LLM_MODEL,
            "prompt": f"Respond concisely in 1 to 2 short sentences: {user_text}", # System prompt + user string.
            "stream": False                          # Wait for the full response instead of streaming tokens.
        }
        
        # Sends the POST request on a background thread.
        response = await asyncio.to_thread(requests.post, OLLAMA_URL, json=payload)
        ai_response = response.json().get("response", "").strip() # Extract the text from the JSON.
        
        # DeepSeek/Reasoning Model Filter: If the model uses <think> tags, strip them out so the TTS doesn't read the internal monologue aloud.
        if "</think>" in ai_response:
            ai_response = ai_response.split("</think>")[-1].strip()
            
        print(f"AI: {ai_response}")
        await websocket.send(ai_response)            # Send the final text to ESP32 Display 1.
        
        print("Generating TTS audio with Piper...")
        
        # Spawns a background OS process to run Piper. We pass data via standard input/output streams rather than saving files.
        process = await asyncio.create_subprocess_exec(
            "piper", "--model", PIPER_MODEL, "--output_raw",
            stdin=asyncio.subprocess.PIPE,           # We will feed text in here.
            stdout=asyncio.subprocess.PIPE,          # Piper will spit audio out here.
            stderr=asyncio.subprocess.PIPE           # Capture errors instead of letting them silently fail.
        )
        
        # Pushes the AI text into Piper and waits for it to finish synthesizing the raw audio array.
        stdout_data, stderr_data = await process.communicate(input=ai_response.encode())
        
        # Safety Check: If Piper failed and output 0 bytes of audio...
        if len(stdout_data) == 0:
            print(f"\nPIPER CRASHED! Error log:\n{stderr_data.decode()}") # Print the exact C++ error log.
            unlock_callback()
            return
        
        CHUNK_SIZE = 1024                            # Break the massive audio file into 1KB chunks.
        # Spoon-feed the audio back to the ESP32 to prevent overflowing its tiny WiFi buffer and crashing it.
        for i in range(0, len(stdout_data), CHUNK_SIZE):
            chunk = stdout_data[i:i+CHUNK_SIZE]
            try:
                await websocket.send(chunk)          # Send 1KB over WiFi.
                
                # Sleep for the exact physical duration it takes the speaker to play that chunk.
                # 1024 bytes (512 samples) at 44100Hz = roughly 0.023 seconds of real-world time.
                await asyncio.sleep(len(chunk) / 44100.0) 
            except websockets.exceptions.ConnectionClosed:
                print("\n Client disconnected during playback.")
                break
            
        await asyncio.sleep(0.5)                     # Brief pause to ensure the final audio buffer drains.
        unlock_callback()                            # Trigger the reset function, returning the system to wake-word hunting mode.

# ---------------------------------------------------------
# 4. START SERVER
# ---------------------------------------------------------
async def main():
    print("Starting WebSocket Server on port 8765...")
    # Binds the async websocket handler to all network interfaces (0.0.0.0) on port 8765.
    async with websockets.serve(handle_client, "0.0.0.0", 8765):
        await asyncio.Future()                       # Keeps the main event loop running forever.

if __name__ == "__main__":
    asyncio.run(main())                              # Python entry point to launch the async loop.