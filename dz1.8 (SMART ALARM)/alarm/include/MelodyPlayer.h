#pragma once

#include <Arduino.h>

class MelodyPlayer {
  private:
    const uint8_t pin_;
    const int (*notes_)[2] = nullptr;  // nullptr = nothing is playing

    size_t count_ = 0;
    size_t index_ = 0;

    uint32_t wholeNoteMs_ = 0;
    uint32_t noteStartMs_ = 0;
    uint32_t noteDurationMs_ = 0;

    bool sounding_ = false;

    void start(
        const int (*notes)[2],
        size_t count,
        uint32_t tempoBpm
    );
    void startNote();

  public:
    explicit MelodyPlayer(const uint8_t pin);

    void init();

    template <size_t N>
    void play(
        const int (&melody)[N][2], 
        uint32_t tempoBpm
    ) {
        start(melody, N, tempoBpm);
    }

    void stop();
    void update();

    bool isPlaying() const;
    bool isSounding() const;
};
