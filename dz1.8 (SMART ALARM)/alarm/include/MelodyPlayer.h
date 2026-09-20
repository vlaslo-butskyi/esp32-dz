#pragma once

#include <Arduino.h>

class MelodyPlayer {
  private:
    const uint8_t pin_;
    const int (*notes_)[2] = nullptr;  // nullptr = nothing is playing

    size_t count_ = 0;
    size_t index_ = 0;

    unsigned long wholeNoteMs_ = 0;
    unsigned long noteStartMs_ = 0;
    unsigned long noteDurationMs_ = 0;

    bool sounding_ = false;

    void start(
        const int (*notes)[2],
        size_t count,
        unsigned long tempoBpm
    );
    void startNote();

  public:
    explicit MelodyPlayer(const uint8_t pin);

    void init();

    template <size_t N>
    void play(
        const int (&melody)[N][2], 
        unsigned long tempoBpm
    ) {
        start(melody, N, tempoBpm);
    }

    void stop();
    void update();

    bool isPlaying() const;
    bool isSounding() const;
};
