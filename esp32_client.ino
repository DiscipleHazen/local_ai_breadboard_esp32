#include <WiFi.h>             // Core library to connect the ESP32 to a wireless network.
#include <WebSocketsClient.h> // Library to establish and manage the WebSocket connection to the Python server.
#include <Wire.h>             // I2C library required for communicating with the OLED displays.
#include <Adafruit_GFX.h>     // Core graphics library for drawing primitive shapes and text.
#include <Adafruit_SSD1306.h> // Hardware-specific library for driving SSD1306 OLED panels.
#include <driver/i2s.h>       // Native ESP32 driver for I2S (Inter-IC Sound) to handle digital audio streaming.

// ---------------------------------------------------------
// WIFI & WEBSOCKET SETTINGS
// ---------------------------------------------------------
const char* ssid = "SSID";     // The SSID (name) of your local WiFi network.
const char* password = "SSID PASSWORD";  // The password for your local WiFi network.
const char* serverIP = "LOCAL IPV4 ADDRESS OF PC RUNNING SERVER";       // The local IPv4 address of the PC running the Python server.
const uint16_t serverPort = 8765;            // The port the Python WebSocket server is listening on.

WebSocketsClient webSocket;                  // Instantiates the WebSocket client object.

// ---------------------------------------------------------
// OLED DISPLAY SETTINGS
// ---------------------------------------------------------
#define SCREEN_WIDTH 128                     // Defines the width of the OLED displays in pixels.
#define SCREEN_HEIGHT 64                     // Defines the height of the OLED displays in pixels.

#define SDA_1 3                              // I2C Data pin for the first OLED display.
#define SCL_1 4                              // I2C Clock pin for the first OLED display.
// Initializes the first display object using the primary I2C bus (&Wire).
Adafruit_SSD1306 display1(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define SDA_2 1                              // I2C Data pin for the second OLED display.
#define SCL_2 2                              // I2C Clock pin for the second OLED display.
// Initializes the second display object using the secondary I2C bus (&Wire1).
Adafruit_SSD1306 display2(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, -1);

// ---------------------------------------------------------
// I2S AUDIO PINS
// ---------------------------------------------------------
#define I2S_MIC_PORT I2S_NUM_0               // Assigns the microphone to hardware I2S port 0.
#define I2S_MIC_WS 9                         // Word Select (Left/Right Clock) pin for the microphone.
#define I2S_MIC_SCK 10                       // Serial Clock (Bit Clock) pin for the microphone.
#define I2S_MIC_SD 17                        // Serial Data IN pin from the microphone.

#define I2S_AMP_PORT I2S_NUM_1               // Assigns the speaker amplifier to hardware I2S port 1.
#define I2S_AMP_LRC 6                        // Word Select (Left/Right Clock) pin for the amplifier.
#define I2S_AMP_BCLK 5                       // Serial Clock (Bit Clock) pin for the amplifier.
#define I2S_AMP_DIN 7                        // Serial Data OUT pin to the amplifier.

#define BUFFER_SIZE 1024                     // Total bytes allocated for reading raw I2S data.
#define AUDIO_BUFFER_SIZE 512                // Number of 16-bit audio samples to store in a packet.

struct AudioPacket {                         // Defines a custom data structure for audio packets.
  int16_t data[AUDIO_BUFFER_SIZE];           // Array holding the 16-bit audio samples.
  size_t length;                             // The actual number of bytes in this specific packet.
};

QueueHandle_t audioQueue;                    // FreeRTOS queue to safely pass audio data between processor cores.
TaskHandle_t AudioTaskHandle;                // Handle to keep track of the FreeRTOS audio task.

int32_t micBuffer32[BUFFER_SIZE / 4];        // Array to hold the raw 32-bit audio data read directly from the I2S mic.
// ---------------------------------------------------------
// MULTICORE WAVEFORM GLOBALS
// ---------------------------------------------------------
volatile bool newAudioFrame = false;         // Flag used across threads to indicate new audio data is ready to be drawn.
int16_t displayBuffer[SCREEN_WIDTH];         // Buffer holding audio data specifically scaled for drawing the waveform.
TaskHandle_t DisplayTaskHandle;              // Handle to keep track of the FreeRTOS display task.

// ---------------------------------------------------------
// INITIALIZATION FUNCTIONS
// ---------------------------------------------------------
void setupI2S() {
  // Configures the I2S interface for the Microphone (Reading Audio)
  i2s_config_t i2s_mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), // Sets ESP32 as the master clock and sets to Receive (RX) mode.
    .sample_rate = 16000,                                // 16kHz sample rate (standard for wake word and speech recognition).
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,        // Reads 32 bits per sample (common for INMP441 mics).
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,         // Captures audio only from the left channel.
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,   // Standard Philips I2S communication format.
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,            // Assigns a low priority (Level 1) interrupt.
    .dma_buf_count = 8,                                  // Number of Direct Memory Access (DMA) buffers.
    .dma_buf_len = 256,                                  // Size of each DMA buffer (in samples).
    .use_apll = false,                                   // Disables the high-precision Audio PLL clock (not needed here).
    .tx_desc_auto_clear = false,                         // Does not auto-clear the transmit buffer (we are only receiving).
    .fixed_mclk = 0                                      // Does not use a fixed Master Clock.
  };
  i2s_pin_config_t mic_pins = {                          // Maps the logical I2S functions to the physical ESP32 pins.
    .bck_io_num = I2S_MIC_SCK,                           
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,                   // Disables data output on the mic port.
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL); // Installs the driver in the ESP32 kernel.
  i2s_set_pin(I2S_MIC_PORT, &mic_pins);                       // Applies the pin routing.

  // Configures the I2S interface for the Amplifier (Writing Audio)
  i2s_config_t i2s_amp_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), // Sets ESP32 as the master clock and sets to Transmit (TX) mode.
    .sample_rate = 22050,                                // 22.05kHz sample rate (matches Piper TTS output quality).
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,        // Outputs 16 bits per sample (standard PCM audio).
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,         // Outputs to the left channel only.
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,   // Standard Philips I2S format.
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,            // Level 1 hardware interrupt.
    .dma_buf_count = 8,                                  // Number of DMA buffers to prevent audio stuttering.
    .dma_buf_len = 256,                                  // Size of each buffer.
    .use_apll = false,                                   // Audio PLL disabled.
    .tx_desc_auto_clear = true,                          // Clears the transmit buffer automatically when empty (prevents buzzing/looping).
    .fixed_mclk = 0                                      // No fixed Master Clock.
  };
  i2s_pin_config_t amp_pins = {                          // Maps the logical I2S functions to physical amplifier pins.
    .bck_io_num = I2S_AMP_BCLK,
    .ws_io_num = I2S_AMP_LRC,
    .data_out_num = I2S_AMP_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE                     // Disables data input on the amp port.
  };
  i2s_driver_install(I2S_AMP_PORT, &i2s_amp_config, 0, NULL); // Installs the amp driver.
  i2s_set_pin(I2S_AMP_PORT, &amp_pins);                       // Applies the pin routing.
}

void setupDisplays() {
  Wire.begin(SDA_1, SCL_1);                              // Starts the primary I2C bus for Display 1.
  Wire1.begin(SDA_2, SCL_2);                             // Starts the secondary I2C bus for Display 2.

  if(!display1.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {      // Initializes Display 1 with standard 3.3v power and I2C address 0x3C.
    Serial.println("Display 1 allocation failed");       // Prints an error to the serial monitor if it fails.
  }
  if(!display2.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {      // Initializes Display 2 on the secondary bus.
    Serial.println("Display 2 allocation failed");
  }

  display1.clearDisplay();                               // Wipes any random pixels from Display 1's memory.
  display1.setTextSize(1);                               // Sets text size to scale 1 (small).
  display1.setTextColor(SSD1306_WHITE);                  // Sets text color to white (pixels ON).
  display1.setTextWrap(true);                            // Enables automatic line wrapping for long text.
  display1.setCursor(0,0);                               // Moves the text drawing cursor to the top-left corner.
  display1.println("Waiting for PC...");                 // Prints the startup message to the display buffer.
  display1.display();                                    // Pushes the buffer to the physical OLED screen.
  
  display2.clearDisplay();                               // Wipes Display 2's memory.
  display2.setCursor(0,0);                               // Resets the cursor.
  display2.setTextColor(SSD1306_WHITE);                  // Sets text color.
  display2.display();                                    // Pushes the blank buffer to screen 2, leaving it ready for waveforms.
}

void displayTask(void * parameter) {                     // FreeRTOS task designed to run independently on Core 0.
  for(;;) {                                              // Infinite loop; FreeRTOS tasks must never naturally exit.
    if (newAudioFrame) {                                 // Checks if the WebSocket received new audio to draw.
      display2.clearDisplay();                           // Clears the previous frame's waveform.
      
      // Loops across the width of the screen, advancing by 3 pixels to leave gaps between bars.
      for(int i = 0; i < SCREEN_WIDTH; i += 3) {
        // Takes the raw 16-bit PCM amplitude (up to 32767), gets the absolute value, 
        // and divides by 256 to scale it down to roughly fit a 64-pixel high screen.
        int h = abs(displayBuffer[i]) / 256; 
        
        if (h > SCREEN_HEIGHT) h = SCREEN_HEIGHT;        // Caps the height so it doesn't draw off-screen.
        if (h < 2) h = 2;                                // Ensures absolute silence still draws a 2-pixel flatline.
        
        // Draws a filled rectangle (a bar). 
        // Y-coordinate math `(SCREEN_HEIGHT / 2) - (h / 2)` centers the bar vertically on the screen.
        display2.fillRect(i, (SCREEN_HEIGHT / 2) - (h / 2), 2, h, SSD1306_WHITE);
      }
      
      display2.display();                                // Pushes the completely drawn waveform frame to the OLED.
      newAudioFrame = false;                             // Resets the flag, acknowledging we've handled this frame.
    } else {
      // Yields execution to the FreeRTOS scheduler for 10 milliseconds.
      // This prevents the task from starving the CPU watchdog timer, which would cause the ESP32 to crash.
      vTaskDelay(pdMS_TO_TICKS(10)); 
    }
  }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) { // Callback fired every time a WebSocket event occurs.
  switch(type) {                                                       // Evaluates what kind of event happened.
    case WStype_DISCONNECTED:                                          // If the connection drops...
      Serial.println("Disconnected from PC!");                         // Print to serial.
      break;
    case WStype_CONNECTED:                                             // If connection is established...
      Serial.println("Connected to PC!");
      display1.clearDisplay();
      display1.setCursor(0,0);
      display1.println("AI Connected!");                               // Update Display 1 to show success.
      display1.display();
      break;
    case WStype_TEXT:                                                  // If the server sends a text string (like "Listening...").
      display1.clearDisplay();
      display1.setCursor(0,0);
      display1.println((char*)payload);                                // Cast the raw payload to a character array and print it.
      display1.display();
      break;
    case WStype_BIN:                                                   // If the server sends raw binary data (TTS audio).
      size_t bytes_written;
      // Sends the incoming binary audio byte-for-byte directly into the Amplifier's I2S DMA buffer to play it out loud.
      i2s_write(I2S_AMP_PORT, payload, length, &bytes_written, portMAX_DELAY);

      // If the display isn't busy drawing, and the audio chunk is large enough...
      if (!newAudioFrame && length >= sizeof(displayBuffer)) {
        // Copy the raw audio payload into the display buffer so the displayTask can draw it.
        memcpy(displayBuffer, payload, sizeof(displayBuffer));
        newAudioFrame = true;                                          // Trigger the displayTask to draw the new data.
      }
      break;
  }
}

void audioTask(void * parameter) {                                     // FreeRTOS task handling the microphone reading.
  AudioPacket packet;                                                  // Creates an instance of our custom audio packet struct.
  
  for(;;) {                                                            // Infinite loop for continuous microphone reading.
    size_t bytes_read;
    // Reads raw I2S data from the mic into micBuffer32, timing out after 100ms if no data arrives.
    esp_err_t result = i2s_read(I2S_MIC_PORT, &micBuffer32, BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(100));
    
    if(result == ESP_OK && bytes_read > 0) {                           // If the read was successful and data exists...
      int samples = bytes_read / 4;                                    // Divide bytes by 4 to get the number of 32-bit samples.
      if (samples > AUDIO_BUFFER_SIZE) samples = AUDIO_BUFFER_SIZE;    // Prevents overflowing our custom struct array.
      
      for(int i = 0; i < samples; i++) {
        // The INMP441 mic pads data with zeros. We shift right by 16 to extract the meaningful 16-bit audio data.
        int32_t sample = micBuffer32[i] >> 16; 
        sample = sample * 16;                                          // Applies digital gain (amplifies the volume by 16x).
        
        // Hard clipping: Prevents the amplified audio from exceeding the maximum/minimum limits of a 16-bit integer.
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        
        packet.data[i] = (int16_t)sample;                              // Cast the processed sample to a 16-bit int and save it.
      }
      packet.length = samples * 2;                                     // Calculates the final payload byte length (samples * 2 bytes each).
      
      // If the queue is full (network is lagging), we must drop the oldest packet to make room for real-time audio.
      if (uxQueueSpacesAvailable(audioQueue) == 0) {
        AudioPacket dummy;
        xQueueReceive(audioQueue, &dummy, 0);                          // Yanks the oldest packet out of the queue and discards it.
      }
      xQueueSend(audioQueue, &packet, 0);                              // Pushes the freshly recorded audio packet into the queue.
    }
  }
}

void setup() {
  Serial.begin(115200);                                                // Starts serial communication at 115200 baud.
  
  setupDisplays();                                                     // Calls our custom display initialization function.
  setupI2S();                                                          // Calls our custom I2S audio initialization function.

  // Creates the FreeRTOS queue in RAM, allowing it to hold up to 10 AudioPacket structs simultaneously.
  audioQueue = xQueueCreate(10, sizeof(AudioPacket));

  WiFi.begin(ssid, password);                                          // Begins the WiFi connection process.
  while(WiFi.status() != WL_CONNECTED) {                               // Halts execution until WiFi is successfully connected.
    delay(500);
    Serial.print(".");
  }
  
  WiFi.setSleep(false);                                                // Disables WiFi power saving mode to prevent high latency/dropped audio.

  webSocket.begin(serverIP, serverPort, "/");                          // Points the WebSocket client to the Python server address.
  webSocket.onEvent(webSocketEvent);                                   // Registers our event handler function.
  webSocket.setReconnectInterval(5000);                                // Tells the client to try reconnecting every 5 seconds if disconnected.

  // ---------------------------------------------------------
  // FREERTOS TASK PINNING
  // ---------------------------------------------------------
  // ESP32 has two cores. Core 1 handles Arduino loop/WiFi. We offload heavy tasks to Core 0.
  xTaskCreatePinnedToCore(
    displayTask,        // Function to execute
    "DisplayTask",      // Human-readable name for debugging
    4096,               // Stack size in bytes allocated for this task
    NULL,               // Task input parameter (none)
    2,                  // Priority level (2 is relatively low)
    &DisplayTaskHandle, // Pass the handle reference
    1);                 // Pin this task to Core 1

  xTaskCreatePinnedToCore(
    audioTask,          // Function to execute
    "AudioTask",        // Name
    32000,              // Massive 32KB stack because audio arrays consume a lot of local RAM
    NULL,               // No parameters
    3,                  // Higher priority (3) because dropping mic frames ruins speech-to-text
    &AudioTaskHandle,   // Pass the handle reference
    1);                 // Pin to Core 1 (keeps Core 0 free for WiFi/Networking)
}

void loop() {
  webSocket.loop();                                                    // Keeps the WebSocket connection alive and processing incoming data.

  AudioPacket packet;
  static unsigned long lastCheck = 0;
  static int packetsSent = 0;

  if (webSocket.isConnected()) {                                       // If we are actively connected to Python...
    // Attempt to pop an audio packet from the queue. `pdTRUE` means it successfully grabbed one.
    if (xQueueReceive(audioQueue, &packet, 0) == pdTRUE) {
      // Transmit the raw binary data over WiFi to the Python server.
      webSocket.sendBIN((uint8_t*)packet.data, packet.length);
      packetsSent++;                                                   // Iterate our diagnostic counter.
    }
  }

  // Debugging block: Every 2000 milliseconds (2 seconds), print the network health to the serial monitor.
  if (millis() - lastCheck > 2000) {
    Serial.printf("📡 WebSocket Connected: %s | Packets Sent last 2s: %d\n", 
                  webSocket.isConnected() ? "YES" : "NO", packetsSent);
    packetsSent = 0;                                                   // Reset the counter.
    lastCheck = millis();                                              // Update the last check time.
  }

  delay(1);                                                            // Yields 1ms to the scheduler to prevent the `loop()` watchdog from panicking.
}