#ifndef INMP441_MICROPHONE_H
#define INMP441_MICROPHONE_H

/**
 * @file Inmp441Microphone.h
 * @brief Declares the INMP441 I2S MEMS microphone sensor for the Modest IoT Nano-framework.
 * @ingroup sensors_module
 *
 * The microphone streams audio continuously, which does not fit the framework's
 * "read one value per timer tick" model. So this sensor works in two layers:
 *
 *  - A dedicated FreeRTOS capture task drains the I2S DMA without pause, turns the
 *    24-bit samples into PCM 16-bit mono (DC removed, adjustable gain) and keeps
 *    running statistics. It never waits on Serial or the network.
 *  - The Device scheduler sends MEASURE_DATA_REQUESTED_EVENT every few milliseconds.
 *    On each request the sensor closes the statistics window, updates the noise
 *    floor and the voice-activity detector, and answers with one of:
 *      SOUND_STARTED_EVENT / SOUND_ENDED_EVENT when voice activity changes, or
 *      DATA_READ_EVENT when a level report (getLastReport()) is ready.
 *
 * The PCM frames themselves are available through readFrame() (for the future
 * WebSocket link to the edge) and startRecording() (to dump a WAV over Serial).
 *
 * Requires Arduino-ESP32 core 3.x (ESP_I2S.h).
 * @date 2026-11-06
 * @version 1.0
 */

#include <Arduino.h>
#include <ESP_I2S.h>
#include "Sensor.h"

/**
 * @brief Health of the I2S signal, derived from the raw words read from the microphone.
 */
enum class MicrophoneSignalStatus : uint8_t {
    STARTING,     ///< Warm-up period: the INMP441 needs ~300 ms after the clock starts.
    OK,           ///< Real audio is arriving.
    NO_SAMPLES,   ///< I2S returned no data (driver not started or clock pins wrong).
    ONLY_ZEROS,   ///< Both I2S slots are all zeros: SD not connected, mic without power, or L/R floating.
    STUCK_VALUE,  ///< Every word is identical (e.g. SD shorted to 3.3V or GND).
    SATURATED     ///< More than 1% of the samples are clipping: lower the gain.
};

/**
 * @brief Audio level summary over one report window.
 */
struct AudioLevelReport {
    float rmsInDbfs = -120.0f;           ///< Average level (dB relative to full scale; 0 = maximum).
    float peakInDbfs = -120.0f;          ///< Highest instantaneous level.
    int peakAmplitude = 0;               ///< Highest absolute PCM value (0..32767).
    float noiseFloorInDbfs = -120.0f;    ///< Estimated background noise level.
    float gainInDecibels = 0.0f;         ///< Digital gain applied when the window was captured.
    uint32_t clippedSamples = 0;         ///< Samples that hit +/-32767 after the gain.
    uint32_t capturedFrames = 0;         ///< Frames captured during the window.
    uint32_t droppedFrames = 0;          ///< Frames discarded because nobody drained readFrame() in time.
    uint32_t windowInMilliseconds = 0;   ///< Real length of the window.
    float measuredSampleRateInHertz = 0; ///< Samples per second actually received (should be ~16000).
    int activeSlot = -1;                 ///< I2S slot carrying the microphone: 0 = left, 1 = right, -1 = unknown.
    bool voiceActive = false;            ///< Voice-activity detector state at the end of the window.
    MicrophoneSignalStatus status = MicrophoneSignalStatus::STARTING;
};

/**
 * @brief INMP441 digital microphone over I2S, exposed as a ModestIoT Sensor.
 * @ingroup sensors_module
 */
class Inmp441Microphone : public Sensor {
public:
    /** @brief Which I2S slot to use. AUTO picks the one that carries signal (works with L/R to GND or to 3.3V). */
    enum class Channel : uint8_t { AUTO, LEFT, RIGHT };

    static constexpr size_t FRAME_SAMPLES = 320;  ///< 20 ms at 16 kHz.
    static constexpr int SOUND_STARTED_EVENT_IDENTIFIER = 60;
    static constexpr int SOUND_ENDED_EVENT_IDENTIFIER = 61;
    static constexpr Event SOUND_STARTED_EVENT = Event(SOUND_STARTED_EVENT_IDENTIFIER);
    static constexpr Event SOUND_ENDED_EVENT = Event(SOUND_ENDED_EVENT_IDENTIFIER);

    /**
     * @param sckGpioPin Bit clock (INMP441 SCK).
     * @param wsGpioPin Word select (INMP441 WS).
     * @param sdGpioPin Serial data (INMP441 SD). Stored as the Sensor's main pin.
     * @param sampleRateInHertz PCM sample rate (16000 for the voice edge).
     * @param parentHandler Optional handler (usually the Device).
     */
    Inmp441Microphone(int sckGpioPin, int wsGpioPin, int sdGpioPin,
                      uint32_t sampleRateInHertz = 16000, EventHandler* parentHandler = nullptr);
    ~Inmp441Microphone() override;

    /**
     * @brief Starts I2S and the capture task. Call it from setup(), never from a global constructor.
     * @return false if I2S or the task could not start.
     */
    bool begin(Channel channel = Channel::AUTO, UBaseType_t captureTaskPriority = 5, BaseType_t captureTaskCore = 1);

    /** @brief Digital gain applied on top of the 24-bit to 16-bit conversion (0..48 dB). */
    void setGainInDecibels(float gainInDecibels);
    [[nodiscard]] float getGainInDecibels() const;

    /** @brief How many dB above the noise floor counts as voice (default 12 dB). */
    void setVoiceThresholdInDecibels(float decibelsAboveNoiseFloor);
    /** @brief How long the level must stay low before SOUND_ENDED_EVENT (default 600 ms). */
    void setVoiceHangoverInMilliseconds(unsigned long hangoverInMilliseconds);
    /** @brief How often DATA_READ_EVENT is emitted (default 1000 ms). */
    void setReportIntervalInMilliseconds(unsigned long reportIntervalInMilliseconds);

    [[nodiscard]] AudioLevelReport getLastReport() const;
    /** @brief Level of the most recent scheduler tick (for a live VU meter). */
    [[nodiscard]] float getInstantLevelInDbfs() const;
    [[nodiscard]] bool isVoiceActive() const;
    [[nodiscard]] uint32_t getSampleRate() const;
    [[nodiscard]] int getActiveSlot() const;

    /**
     * @brief Copies the next `sampleCapacity` samples into `buffer` from the capture task.
     * @return false if a recording is already running or the arguments are invalid.
     */
    bool startRecording(int16_t* buffer, size_t sampleCapacity);
    [[nodiscard]] bool isRecording() const;
    [[nodiscard]] size_t getRecordedSampleCount() const;
    void cancelRecording();

    /**
     * @brief Enables the frame queue consumed with readFrame(). Off by default so no audio piles up.
     * @param depthInFrames Queue length (each frame is FRAME_SAMPLES samples, 20 ms).
     */
    bool enableFrameQueue(size_t depthInFrames);
    /** @brief Pops one frame of FRAME_SAMPLES samples. Returns false on timeout. */
    bool readFrame(int16_t* destination, TickType_t waitTicks);

    static const char* describeStatus(MicrophoneSignalStatus status);
    static const char* describeStatusHint(MicrophoneSignalStatus status);

protected:
    /** @brief Closes the statistics window and decides which event to propagate. */
    void processEvent(Event& event) override;

private:
    /** Statistics written by the capture task and taken by processEvent(). */
    struct CaptureWindow {
        uint64_t sumOfSquares = 0;
        uint32_t sampleCount = 0;
        int peakAmplitude = 0;
        uint32_t clippedSamples = 0;
        uint32_t capturedFrames = 0;
        uint32_t droppedFrames = 0;
        uint32_t zeroFrames = 0;   ///< Frames where both slots were all zeros.
        uint32_t stuckFrames = 0;  ///< Frames where the active slot never changed value.
    };

    static void captureTaskEntry(void* taskParameters);
    void captureLoop();
    void convertFrame(const int32_t* stereoWords, size_t frameCount, int slot);
    int chooseSlot(const int32_t* stereoWords, size_t frameCount);
    static float toDbfs(float amplitude);

    I2SClass i2s;
    const int sckPin;
    const int wsPin;
    const uint32_t sampleRate;
    Channel channelMode = Channel::AUTO;
    TaskHandle_t captureTaskHandle = nullptr;
    QueueHandle_t frameQueue = nullptr;
    mutable portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    volatile bool running = false;

    // Capture task state (only touched by the capture task).
    int32_t stereoBuffer[FRAME_SAMPLES * 2] = {};
    int16_t pcmFrame[FRAME_SAMPLES] = {};
    float dcPreviousInput = 0.0f;
    float dcPreviousOutput = 0.0f;
    uint32_t warmUpFramesLeft = 0;
    volatile int activeSlot = -1;
    uint32_t framesFavouringOther = 0;

    // Shared between tasks (guarded by `lock`).
    CaptureWindow window;
    float gainLinear = 1.0f;
    float gainInDecibels = 0.0f;
    int16_t* recordBuffer = nullptr;
    size_t recordCapacity = 0;
    size_t recordCount = 0;
    AudioLevelReport lastReport;
    float instantLevelInDbfs = -120.0f;

    // Scheduler-side state (only touched inside processEvent()).
    CaptureWindow reportWindow;
    unsigned long reportWindowStart = 0;
    unsigned long lastTickTimestamp = 0;
    unsigned long belowThresholdSince = 0;
    float noiseFloorInDbfs = 0.0f;
    bool noiseFloorReady = false;
    bool voiceActive = false;
    bool voiceStatePending = false;  ///< A voice transition is waiting to be reported.
    float voiceThresholdInDecibels = 12.0f;
    unsigned long voiceHangoverInMilliseconds = 600;
    unsigned long reportIntervalInMilliseconds = 1000;

    static constexpr uint32_t WARM_UP_MILLISECONDS = 300;
    static constexpr float DC_FILTER_POLE = 0.995f;          ///< ~13 Hz high-pass at 16 kHz.
    static constexpr float MINIMUM_VOICE_LEVEL_IN_DBFS = -65.0f;
    static constexpr float NOISE_FLOOR_RISE_RATE = 0.01f;     ///< Slow rise so speech does not lift the floor.
    static constexpr float NOISE_FLOOR_FALL_RATE = 0.30f;     ///< Fast fall when the room gets quieter.
    static constexpr int PROPAGATION_SUPPRESSION_IDENTIFIER = -1;
};

#endif // INMP441_MICROPHONE_H
